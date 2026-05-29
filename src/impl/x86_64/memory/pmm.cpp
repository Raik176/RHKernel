#include "memory/pmm.h"

#include "console.h"
#include "multiboot2.h"
#include "smp/lock.h"
#include "string.h"
#include "util.h"

extern "C" {
extern uint8_t _kernel_phys_start[];
extern uint8_t _kernel_phys_end[];
extern uint8_t _boot_start[];
extern uint8_t _boot_end[];
extern uint8_t pml5_table[];
extern uint8_t pml5_table_end[];
extern uint8_t pml4_table[];
extern uint8_t pml4_table_end[];
extern uint8_t pdp_table[];
extern uint8_t pdp_table_end[];
extern uint8_t page_directory[];
extern uint8_t page_directory_end[];
extern uint8_t phys_map_pml4_table[];
extern uint8_t phys_map_pml4_table_end[];
extern uint8_t phys_map_pdp_table[];
extern uint8_t phys_map_pdp_table_end[];
extern uint8_t phys_map_pd_table[];
extern uint8_t phys_map_pd_table_end[];
extern uint8_t high_pdp_table[];
extern uint8_t high_pdp_table_end[];
extern uint8_t high_pd_table[];
extern uint8_t high_pd_table_end[];
extern uint8_t trampoline_start[];
extern uint8_t trampoline_end[];
}

namespace {
    static spinlock_t pmm_lock;

    size_t managed_bytes = 0;
    size_t free_bytes = 0;
    size_t system_bytes = 0;
    size_t physical_limit_bytes = 0;
    size_t active_max_order = pmm::MIN_ORDER;

    constexpr size_t TARGET_TOP_LEVEL_BLOCKS = 128;

    // The assembly boot page tables intentionally map only a modest amount of
    // RAM.  While pmm::init() is running, writing allocator nodes with p2v() is
    // safe only below this temporary map.  Higher usable ranges are recorded
    // and released after vmm::init() installs the full direct map.
    constexpr uint64_t EARLY_DIRECT_MAP_BYTES = 64ULL * 1024 * 1024 * 1024;

    struct DeferredSpan {
        uint64_t start;
        uint64_t end;
    };

    constexpr size_t MAX_DEFERRED_SPANS = 256;
    DeferredSpan deferred_spans[MAX_DEFERRED_SPANS];
    size_t deferred_span_count = 0;
    bool defer_unmapped_spans = true;

    constexpr size_t MAX_DIRECT_MAP_SPANS = 256;
    DeferredSpan direct_map_spans[MAX_DIRECT_MAP_SPANS];
    size_t direct_map_span_count = 0;

    uint64_t ap_trampoline_page_phys = 0;
    size_t ap_trampoline_size = 0;

    static bool is_power_of_two(uint64_t value) { return value != 0 && (value & (value - 1)) == 0; }

    static bool add_overflows(uint64_t a, uint64_t b, uint64_t *out) {
        if (a > UINT64_MAX - b) return true;
        *out = a + b;
        return false;
    }

    static bool align_up_checked(uint64_t value, uint64_t align, uint64_t *out) {
        if (!is_power_of_two(align)) return false;
        if (value > UINT64_MAX - (align - 1)) return false;
        *out = (value + align - 1) & ~(align - 1);
        return true;
    }

    static bool align_down_checked(uint64_t value, uint64_t align, uint64_t *out) {
        if (!is_power_of_two(align)) return false;
        *out = value & ~(align - 1);
        return true;
    }

    static bool range_end_checked(uint64_t start, uint64_t size, uint64_t *end) {
        return !add_overflows(start, size, end);
    }

    static void add_direct_map_span(uint64_t start, uint64_t end) {
        if (!align_down_checked(start, pmm::PAGE_SIZE, &start)) return;
        if (!align_up_checked(end, pmm::PAGE_SIZE, &end)) return;
        if (start >= end) return;

        for (size_t i = 0; i < direct_map_span_count; i++) {
            if (end < direct_map_spans[i].start || start > direct_map_spans[i].end) continue;
            if (start > direct_map_spans[i].start) start = direct_map_spans[i].start;
            if (end < direct_map_spans[i].end) end = direct_map_spans[i].end;
            direct_map_spans[i] = direct_map_spans[--direct_map_span_count];
            i = 0;
            if (direct_map_span_count == 0) break;
        }

        if (direct_map_span_count >= MAX_DIRECT_MAP_SPANS) kpanic("PMM: too many direct-map spans");
        direct_map_spans[direct_map_span_count++] = {start, end};
    }

    static bool ranges_overlap(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) {
        return a_start < b_end && b_start < a_end;
    }

    static bool reserved_range_overlaps(uint64_t start, uint64_t end, const uint64_t *reserved,
                                        size_t res_count) {
        for (size_t i = 0; i < res_count; i++) {
            if (ranges_overlap(start, end, reserved[i * 2], reserved[i * 2 + 1])) return true;
        }
        return false;
    }

    static uint64_t ap_trampoline_required_size() {
        uintptr_t start = (uintptr_t)trampoline_start;
        uintptr_t end = (uintptr_t)trampoline_end;
        if (end <= start) kpanic("PMM: invalid AP trampoline image");

        uint64_t size = end - start;
        if (!align_up_checked(size, pmm::PAGE_SIZE, &size)) {
            kpanic("PMM: AP trampoline size overflows");
        }
        return size;
    }

    static void reserve_ap_trampoline_span(multiboot_tag_mmap *mmap, const uint64_t *reserved,
                                           size_t res_count) {
        uint64_t required = ap_trampoline_required_size();
        if (required == 0 || required > 0x100000) {
            kpanic("PMM: AP trampoline cannot fit below 1 MiB");
        }

        uint64_t best = 0;
        bool found = false;

        for (auto *e = mmap->entries; (uint8_t *)e < (uint8_t *)mmap + mmap->size;
             e = (multiboot_mmap_entry *)((uint8_t *)e + mmap->entry_size)) {
            if (e->type != MULTIBOOT_MEMORY_AVAILABLE) continue;

            uint64_t entry_end = 0;
            if (!range_end_checked(e->addr, e->len, &entry_end)) {
                kpanic("PMM: usable memory-map entry overflows");
            }

            uint64_t start = e->addr;
            uint64_t end = entry_end < 0x100000 ? entry_end : 0x100000;
            if (start >= end || end - start < required) continue;

            uint64_t candidate = (end - required) & ~(pmm::PAGE_SIZE - 1);
            while (candidate >= start) {
                uint64_t candidate_end = candidate + required;
                if (!reserved_range_overlaps(candidate, candidate_end, reserved, res_count)) {
                    if (!found || candidate > best) {
                        best = candidate;
                        found = true;
                    }
                    break;
                }
                if (candidate < pmm::PAGE_SIZE) break;
                candidate -= pmm::PAGE_SIZE;
            }
        }

        if (!found) kpanic("PMM: no low memory span for AP trampoline");
        ap_trampoline_page_phys = best;
        ap_trampoline_size = required;
    }

    struct BuddySizingStats {
        uint64_t bytes;
        uint64_t largest_span;
    };

    static size_t floor_log2_u64(uint64_t value) {
        return 63 - (size_t)__builtin_clzll(value);
    }

    static size_t clamp_order(size_t order, size_t largest_order) {
        if (order > largest_order) order = largest_order;
        if (order > pmm::MAX_ORDER_LIMIT) order = pmm::MAX_ORDER_LIMIT;
        return order;
    }

    static size_t choose_max_order(uint64_t allocatable_bytes, uint64_t largest_span) {
        uint64_t pages = allocatable_bytes >> 12;
        uint64_t largest_pages = largest_span >> 12;
        if (pages == 0 || largest_pages == 0) return 0;

        size_t largest_order = floor_log2_u64(largest_pages);
        uint64_t target_pages = pages / TARGET_TOP_LEVEL_BLOCKS;
        if (target_pages == 0) return clamp_order(largest_order, largest_order);

        size_t order = floor_log2_u64(target_pages);
        if (order < pmm::MIN_ORDER) order = largest_order < pmm::MIN_ORDER ? largest_order : pmm::MIN_ORDER;
        return clamp_order(order, largest_order);
    }

    static void measure_available_range(uint64_t start, uint64_t end, const uint64_t *reserved,
                                        size_t res_count, BuddySizingStats *stats) {
        if (start >= end) return;
        for (size_t i = 0; i < res_count; i++) {
            uint64_t res_start = reserved[i * 2];
            uint64_t res_end = reserved[i * 2 + 1];
            if (!ranges_overlap(start, end, res_start, res_end)) continue;

            if (start < res_start) {
                measure_available_range(start, res_start, reserved + (i + 1) * 2,
                                        res_count - (i + 1), stats);
            }
            if (res_end < end) {
                measure_available_range(res_end, end, reserved + (i + 1) * 2,
                                        res_count - (i + 1), stats);
            }
            return;
        }

        uint64_t aligned_start = 0;
        uint64_t aligned_end = 0;
        if (!align_up_checked(start, pmm::PAGE_SIZE, &aligned_start)) return;
        if (!align_down_checked(end, pmm::PAGE_SIZE, &aligned_end)) return;
        if (aligned_start >= aligned_end) return;

        uint64_t bytes = aligned_end - aligned_start;
        if (stats->bytes > UINT64_MAX - bytes) kpanic("PMM: allocatable memory size overflows");
        stats->bytes += bytes;
        if (bytes > stats->largest_span) stats->largest_span = bytes;
    }

    static BuddySizingStats measure_buddy_sizing(multiboot_tag_mmap *mmap, const uint64_t *reserved,
                                                 size_t res_count) {
        BuddySizingStats stats{0, 0};
        for (auto *e = mmap->entries; (uint8_t *)e < (uint8_t *)mmap + mmap->size;
             e = (multiboot_mmap_entry *)((uint8_t *)e + mmap->entry_size)) {
            if (e->type != MULTIBOOT_MEMORY_AVAILABLE) continue;
            uint64_t entry_end = 0;
            if (!range_end_checked(e->addr, e->len, &entry_end)) {
                kpanic("PMM: usable memory-map entry overflows");
            }
            measure_available_range(e->addr, entry_end, reserved, res_count, &stats);
        }
        return stats;
    }

    // CoW support: sparse reference counts.
    //
    // The old implementation allocated one contiguous uint32_t per PFN up to
    // physical_limit_bytes. That makes boot depend on finding a single huge
    // block and scales with the highest physical address, including holes.
    // Instead, refcounts live in 4 KiB leaves allocated on demand. Each leaf
    // covers 1024 physical pages. Intermediate radix nodes are also 4 KiB pages
    // and contain 512 child pointers, so metadata grows only for PFN ranges that
    // are actually allocated/refcounted.
    size_t total_pages = 0;

    constexpr size_t REFCOUNTS_PER_LEAF = pmm::PAGE_SIZE / sizeof(uint32_t);
    constexpr size_t REF_RADIX_BITS = 9;
    constexpr size_t REF_RADIX_ENTRIES = 1ULL << REF_RADIX_BITS;
    constexpr size_t REF_RADIX_LEVELS = 4;
    constexpr uint64_t REF_RADIX_MASK = REF_RADIX_ENTRIES - 1;

    void *ref_root[REF_RADIX_ENTRIES];
}  // namespace

namespace pmm {

    struct FreeBlock {
        FreeBlock *next;
    };

    static FreeBlock *free_lists[MAX_ORDER_LIMIT + 1];
    static size_t free_block_counts[MAX_ORDER_LIMIT + 1];
    static uint64_t nonempty_order_mask = 0;

    size_t size_to_order(size_t size) {
        if (size == 0) return 0;
        if (size > UINT64_MAX - (PAGE_SIZE - 1)) return MAX_ORDER_LIMIT + 1;
        uint64_t pages = (size + PAGE_SIZE - 1) >> 12;
        if (pages <= 1) return 0;
        return 64 - (size_t)__builtin_clzll(pages - 1);
    }

    static uint64_t block_size(size_t order) { return (1ULL << order) * PAGE_SIZE; }
    static uint64_t buddy_of(uint64_t addr, size_t order) { return addr ^ block_size(order); }

    static void mark_order_nonempty(size_t order) { nonempty_order_mask |= 1ULL << order; }

    static void mark_order_empty(size_t order) { nonempty_order_mask &= ~(1ULL << order); }

    static size_t first_nonempty_order(size_t min_order) {
        uint64_t mask = nonempty_order_mask & (~0ULL << min_order);
        if (!mask) return active_max_order + 1;
        return (size_t)__builtin_ctzll(mask);
    }

    static void push_block(uint64_t addr, size_t order) {
        if (order > active_max_order || (addr & (block_size(order) - 1)) != 0) {
            kpanic("PMM: invalid free block");
        }
        FreeBlock *block = (FreeBlock *)p2v(addr);
        block->next = free_lists[order];
        free_lists[order] = block;
        free_block_counts[order]++;
        mark_order_nonempty(order);
        free_bytes += block_size(order);
    }

    static uint64_t pop_block(size_t order) {
        FreeBlock *block = free_lists[order];
        if (!block) return 0;
        free_lists[order] = block->next;
        free_block_counts[order]--;
        if (!free_lists[order]) mark_order_empty(order);
        free_bytes -= block_size(order);
        return v2p(block);
    }

    static bool remove_block(uint64_t phys_addr, size_t order) {
        FreeBlock **cur = &free_lists[order];
        FreeBlock *target_virt = (FreeBlock *)p2v(phys_addr);
        while (*cur) {
            if (*cur == target_virt) {
                *cur = (*cur)->next;
                free_block_counts[order]--;
                if (!free_lists[order]) mark_order_empty(order);
                free_bytes -= block_size(order);
                return true;
            }
            cur = &(*cur)->next;
        }
        return false;
    }

    static void push_range_locked(uint64_t start, uint64_t end) {
        uint64_t curr = 0;
        uint64_t last = 0;
        if (!align_up_checked(start, PAGE_SIZE, &curr)) return;
        if (!align_down_checked(end, PAGE_SIZE, &last)) return;

        while (curr < last) {
            uint64_t remaining = last - curr;
            size_t align_order = (curr == 0) ? active_max_order : (size_t)__builtin_ctzll(curr >> 12);
            size_t size_order = 63 - __builtin_clzll(remaining >> 12);
            size_t order = align_order;
            if (order > size_order) order = size_order;
            if (order > active_max_order) order = active_max_order;

            push_block(curr, order);
            curr += block_size(order);
        }
    }

    static bool range_can_hold(uint64_t block, uint64_t block_end, uint64_t size,
                               const AllocConstraints &constraints, uint64_t *candidate) {
        uint64_t min_phys = constraints.min_phys > block ? constraints.min_phys : block;
        uint64_t max_phys = constraints.max_phys_exclusive < block_end
                                ? constraints.max_phys_exclusive
                                : block_end;
        if (min_phys >= max_phys || size > max_phys - min_phys) return false;

        uint64_t aligned = 0;
        if (!align_up_checked(min_phys, constraints.align, &aligned)) return false;
        if (aligned < min_phys || aligned > max_phys || size > max_phys - aligned) return false;

        *candidate = aligned;
        return true;
    }

    static void *alloc_metadata_page_locked() {
        for (size_t i = first_nonempty_order(0); i <= active_max_order; i = first_nonempty_order(i + 1)) {
            uint64_t block = pop_block(i);
            if (!block) continue;
            while (i > 0) {
                i--;
                uint64_t buddy = block + block_size(i);
                push_block(buddy, i);
            }
            void *page = p2v(block);
            memset(page, 0, PAGE_SIZE);
            return page;
        }
        return nullptr;
    }

    static uint32_t *get_ref_slot_locked(uint64_t phys, bool create) {
        uint64_t pfn = phys / PAGE_SIZE;
        if (pfn >= total_pages) return nullptr;

        uint64_t chunk = pfn / REFCOUNTS_PER_LEAF;
        size_t leaf_index = pfn % REFCOUNTS_PER_LEAF;

        // REF_RADIX_LEVELS=4 stores chunk indexes with up to 36 bits, enough for
        // 2^(36 + 10) pages = 256 PiB of 4 KiB-addressed physical memory.
        if ((chunk >> (REF_RADIX_BITS * REF_RADIX_LEVELS)) != 0) return nullptr;

        void **node = ref_root;
        for (size_t level = REF_RADIX_LEVELS; level > 1; level--) {
            size_t shift = (level - 1) * REF_RADIX_BITS;
            size_t idx = (chunk >> shift) & REF_RADIX_MASK;
            if (!node[idx]) {
                if (!create) return nullptr;
                node[idx] = alloc_metadata_page_locked();
                if (!node[idx]) return nullptr;
            }
            node = (void **)node[idx];
        }

        size_t leaf_slot = chunk & REF_RADIX_MASK;
        if (!node[leaf_slot]) {
            if (!create) return nullptr;
            node[leaf_slot] = alloc_metadata_page_locked();
            if (!node[leaf_slot]) return nullptr;
        }

        uint32_t *leaf = (uint32_t *)node[leaf_slot];
        return &leaf[leaf_index];
    }

    static bool set_ref_locked(uint64_t phys, uint32_t value) {
        uint32_t *slot = get_ref_slot_locked(phys, value != 0);
        if (!slot) return value == 0;
        *slot = value;
        return true;
    }

    static bool init_refcounts_locked(uint64_t phys, uint64_t size) {
        for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
            if (!set_ref_locked(phys + off, 1)) return false;
        }
        return true;
    }

    static void defer_span_locked(uint64_t start, uint64_t end) {
        if (!align_up_checked(start, PAGE_SIZE, &start)) return;
        if (!align_down_checked(end, PAGE_SIZE, &end)) return;
        if (start >= end) return;
        if (deferred_span_count >= MAX_DEFERRED_SPANS) {
            kpanic("PMM: too many deferred memory-map spans");
        }
        deferred_spans[deferred_span_count++] = {start, end};
    }

    void add_span(uint64_t start, uint64_t end) {
        if (defer_unmapped_spans && end > EARLY_DIRECT_MAP_BYTES) {
            if (start < EARLY_DIRECT_MAP_BYTES) {
                add_span(start, EARLY_DIRECT_MAP_BYTES);
            }
            uint64_t deferred_start = start < EARLY_DIRECT_MAP_BYTES ? EARLY_DIRECT_MAP_BYTES : start;
            defer_span_locked(deferred_start, end);
            return;
        }

        uint64_t curr = 0;
        uint64_t last = 0;
        if (!align_up_checked(start, PAGE_SIZE, &curr)) return;
        if (!align_down_checked(end, PAGE_SIZE, &last)) return;

        while (curr < last) {
            uint64_t remaining = last - curr;
            size_t align_order = (curr == 0) ? active_max_order : (size_t)__builtin_ctzll(curr >> 12);
            size_t size_order = 63 - __builtin_clzll(remaining >> 12);
            size_t order = align_order;
            if (order > size_order) order = size_order;
            if (order > active_max_order) order = active_max_order;

            uint64_t sz = block_size(order);
            managed_bytes += sz;
            push_block(curr, order);
            curr += sz;
        }
    }

    void free_with_reservation(uint64_t start, uint64_t end, const uint64_t *reserved,
                               size_t res_count) {
        if (start >= end) return;
        for (size_t i = 0; i < res_count; i++) {
            uint64_t res_start = reserved[i * 2];
            uint64_t res_end = reserved[i * 2 + 1];
            if (start < res_end && res_start < end) {
                if (start < res_start)
                    free_with_reservation(start, res_start, reserved + (i + 1) * 2,
                                          res_count - (i + 1));
                if (end > res_end)
                    free_with_reservation(res_end, end, reserved + (i + 1) * 2,
                                          res_count - (i + 1));
                return;
            }
        }
        add_span(start, end);
    }

    void init(uint64_t mb_phys_addr) {
        managed_bytes = 0;
        free_bytes = 0;
        system_bytes = 0;
        physical_limit_bytes = 0;
        ap_trampoline_page_phys = 0;
        ap_trampoline_size = 0;
        deferred_span_count = 0;
        defer_unmapped_spans = true;
        direct_map_span_count = 0;

        for (size_t i = 0; i <= MAX_ORDER_LIMIT; i++) {
            free_lists[i] = nullptr;
            free_block_counts[i] = 0;
        }
        nonempty_order_mask = 0;
        for (size_t i = 0; i < REF_RADIX_ENTRIES; i++) ref_root[i] = nullptr;

        uint8_t *mb_ptr = (uint8_t *)p2v(mb_phys_addr);
        multiboot_tag_mmap *mmap = nullptr;

        uint64_t max_phys_addr = 0;

        for (multiboot_tag *tag = (multiboot_tag *)(mb_ptr + 8);
             tag->type != MULTIBOOT_TAG_TYPE_END;
             tag = (multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7))) {
            if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
                mmap = (multiboot_tag_mmap *)tag;
                for (auto *e = mmap->entries; (uint8_t *)e < (uint8_t *)mmap + mmap->size;
                     e = (multiboot_mmap_entry *)((uint8_t *)e + mmap->entry_size)) {
                    uint64_t entry_end = 0;
                    if (!range_end_checked(e->addr, e->len, &entry_end)) {
                        kpanic("PMM: memory-map entry overflows");
                    }
                    if (entry_end > max_phys_addr) max_phys_addr = entry_end;
                }
            }
        }

        if (!mmap) return;

        uint64_t aligned_phys_limit = 0;
        if (!align_up_checked(max_phys_addr, PAGE_SIZE, &aligned_phys_limit)) {
            kpanic("PMM: physical limit overflows");
        }
        physical_limit_bytes = aligned_phys_limit;
        if (physical_limit_bytes > paging_phys_direct_map_size_value()) {
            kpanic("PMM: physical address space exceeds direct-map window");
        }
        total_pages = physical_limit_bytes / PAGE_SIZE;

        uint64_t initramfs_start = 0, initramfs_end = 0;
        for (multiboot_tag *tag = (multiboot_tag *)(mb_ptr + 8);
             tag->type != MULTIBOOT_TAG_TYPE_END;
             tag = (multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7))) {
            if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
                multiboot_tag_module *mod = (multiboot_tag_module *)tag;
                if (strcmp(mod->cmdline, "initramfs") == 0) {
                    initramfs_start = mod->mod_start;
                    initramfs_end = mod->mod_end;
                    break;
                }
            }
        }

        uint64_t reserved[] = {0,
                               0x100000,
                               (uint64_t)&_kernel_phys_start,
                               (uint64_t)&_kernel_phys_end,
                               (uint64_t)&_boot_start,
                               (uint64_t)&_boot_end,
                               (uint64_t)&pml5_table,
                               (uint64_t)&pml5_table_end,
                               (uint64_t)&pml4_table,
                               (uint64_t)&pml4_table_end,
                               (uint64_t)&pdp_table,
                               (uint64_t)&pdp_table_end,
                               (uint64_t)&page_directory,
                               (uint64_t)&page_directory_end,
                               (uint64_t)&phys_map_pml4_table,
                               (uint64_t)&phys_map_pml4_table_end,
                               (uint64_t)&phys_map_pdp_table,
                               (uint64_t)&phys_map_pdp_table_end,
                               (uint64_t)&phys_map_pd_table,
                               (uint64_t)&phys_map_pd_table_end,
                               (uint64_t)&high_pdp_table,
                               (uint64_t)&high_pdp_table_end,
                               (uint64_t)&high_pd_table,
                               (uint64_t)&high_pd_table_end,
                               mb_phys_addr,
                               mb_phys_addr + *(uint32_t *)mb_ptr,
                               initramfs_start,
                               initramfs_end};
        size_t res_count = sizeof(reserved) / (sizeof(uint64_t) * 2);

        reserve_ap_trampoline_span(mmap, reserved + 2, res_count - 1);

        BuddySizingStats sizing = measure_buddy_sizing(mmap, reserved, res_count);
        active_max_order = choose_max_order(sizing.bytes, sizing.largest_span);

        for (auto *e = mmap->entries; (uint8_t *)e < (uint8_t *)mmap + mmap->size;
             e = (multiboot_mmap_entry *)((uint8_t *)e + mmap->entry_size)) {
            if (system_bytes > UINT64_MAX - e->len) kpanic("PMM: system memory size overflows");
            system_bytes += e->len;
            if (e->type != MULTIBOOT_MEMORY_AVAILABLE) continue;
            uint64_t entry_end = 0;
            if (!range_end_checked(e->addr, e->len, &entry_end)) {
                kpanic("PMM: usable memory-map entry overflows");
            }
            add_direct_map_span(e->addr, entry_end);
            free_with_reservation(e->addr, entry_end, reserved, res_count);
        }

        // Refcount metadata is sparse and allocated lazily by alloc()/ref_page().
    }

    uint64_t get_ap_trampoline_page() { return ap_trampoline_page_phys; }

    size_t get_ap_trampoline_size() { return ap_trampoline_size; }

    size_t get_max_order() { return active_max_order; }

    void release_deferred_memory() {
        uint64_t flags;
        pmm_lock.acquire(flags);

        defer_unmapped_spans = false;
        for (size_t i = 0; i < deferred_span_count; i++) {
            add_span(deferred_spans[i].start, deferred_spans[i].end);
        }
        deferred_span_count = 0;

        pmm_lock.release(flags);
    }

    void ref_page(uint64_t phys) {
        uint64_t flags;
        pmm_lock.acquire(flags);
        uint32_t *slot = get_ref_slot_locked(phys, true);
        if (!slot) {
            pmm_lock.release(flags);
            kpanic("PMM: failed to allocate refcount metadata");
        }
        (*slot)++;
        pmm_lock.release(flags);
    }

    void unref_page(uint64_t phys) {
        uint64_t flags;
        pmm_lock.acquire(flags);
        uint32_t *slot = get_ref_slot_locked(phys, false);
        if (slot) {
            if (*slot > 0) (*slot)--;
            if (*slot == 0) {
                pmm_lock.release(flags);
                free(phys, PAGE_SIZE);
                return;
            }
        }
        pmm_lock.release(flags);
    }

    uint32_t get_ref(uint64_t phys) {
        uint64_t flags;
        pmm_lock.acquire(flags);
        uint32_t *slot = get_ref_slot_locked(phys, false);
        uint32_t value = slot ? *slot : 0;
        pmm_lock.release(flags);
        return value;
    }

    void free(uint64_t phys, size_t size) {
        if (!phys || size == 0) return;
        uint64_t flags;
        pmm_lock.acquire(flags);
        size_t order = size_to_order(size);
        if (order > active_max_order) {
            pmm_lock.release(flags);
            kpanic("PMM: free size exceeds active max order");
        }

        // Clear refcounts for every 4 KiB page covered by the allocation.
        // CoW/fork works at page granularity, so multi-page allocations such as
        // user stacks must not be treated as one untracked block. If only the
        // first page has a refcount, a fork()+exec() child can unref/free the
        // parent's still-mapped stack pages and corrupt the shell when wait()
        // returns.
        uint64_t ref_size = block_size(order);
        for (uint64_t off = 0; off < ref_size; off += PAGE_SIZE) {
            set_ref_locked(phys + off, 0);
        }

        uint64_t addr = phys;
        while (order < active_max_order) {
            uint64_t buddy = buddy_of(addr, order);
            if (!remove_block(buddy, order)) break;
            addr = (addr < buddy) ? addr : buddy;
            order++;
        }
        push_block(addr, order);
        pmm_lock.release(flags);
    }

    uint64_t alloc(size_t size) {
        if (size == 0) return 0;
        uint64_t flags;
        pmm_lock.acquire(flags);
        size_t order = size_to_order(size);
        if (order > active_max_order) {
            pmm_lock.release(flags);
            return 0;
        }
        for (size_t i = first_nonempty_order(order); i <= active_max_order; i = first_nonempty_order(i + 1)) {
            uint64_t block = pop_block(i);
            if (!block) continue;
            while (i > order) {
                i--;
                uint64_t buddy = block + block_size(i);
                push_block(buddy, i);
            }

            // Initialize sparse refcounts for every page in the allocated block.
            // The VM layer maps and unrefs individual 4 KiB pages, even when the
            // physical memory came from one larger buddy allocation.
            uint64_t alloc_size = block_size(order);
            if (!init_refcounts_locked(block, alloc_size)) {
                pmm_lock.release(flags);
                kpanic("PMM: failed to allocate refcount metadata");
            }

            pmm_lock.release(flags);
            return block;
        }
        pmm_lock.release(flags);
        return 0;
    }

    uint64_t alloc_constrained(size_t size, const AllocConstraints &constraints,
                               size_t *allocated_size) {
        if (allocated_size) *allocated_size = 0;
        if (size == 0) return 0;
        if (size > UINT64_MAX - (PAGE_SIZE - 1)) return 0;
        if (constraints.max_phys_exclusive <= constraints.min_phys) return 0;

        size_t order = size_to_order(size);
        if (order > active_max_order) return 0;

        uint64_t alloc_size = block_size(order);
        uint64_t align = constraints.align ? constraints.align : PAGE_SIZE;
        if (!is_power_of_two(align)) return 0;
        if (align < PAGE_SIZE) align = PAGE_SIZE;
        if (align < alloc_size) align = alloc_size;

        AllocConstraints normalized = constraints;
        normalized.align = align;

        uint64_t flags;
        pmm_lock.acquire(flags);

        for (size_t i = first_nonempty_order(order); i <= active_max_order; i = first_nonempty_order(i + 1)) {
            for (FreeBlock *block = free_lists[i]; block; block = block->next) {
                uint64_t block_phys = v2p(block);
                uint64_t block_end = 0;
                if (add_overflows(block_phys, block_size(i), &block_end)) {
                    pmm_lock.release(flags);
                    kpanic("PMM: free block address overflows");
                }
                uint64_t candidate = 0;

                if (!range_can_hold(block_phys, block_end, alloc_size, normalized, &candidate)) {
                    continue;
                }

                if (!remove_block(block_phys, i)) {
                    pmm_lock.release(flags);
                    kpanic("PMM: constrained free-list removal failed");
                }

                uint64_t candidate_end = 0;
                if (add_overflows(candidate, alloc_size, &candidate_end)) {
                    pmm_lock.release(flags);
                    kpanic("PMM: constrained allocation overflows");
                }

                push_range_locked(block_phys, candidate);
                push_range_locked(candidate_end, block_end);

                if (!init_refcounts_locked(candidate, alloc_size)) {
                    pmm_lock.release(flags);
                    kpanic("PMM: failed to allocate refcount metadata");
                }

                if (normalized.zero) memset(p2v(candidate), 0, alloc_size);
                if (allocated_size) *allocated_size = alloc_size;

                pmm_lock.release(flags);
                return candidate;
            }
        }

        pmm_lock.release(flags);
        return 0;
    }

    void get_debug_info(DebugInfo *info) {
        if (!info) return;

        uint64_t flags;
        pmm_lock.acquire(flags);

        memset(info, 0, sizeof(*info));
        info->managed_bytes = managed_bytes;
        info->free_bytes = free_bytes;
        info->system_bytes = system_bytes;
        info->physical_limit_bytes = physical_limit_bytes;
        info->deferred_span_count = deferred_span_count;

        info->max_order = active_max_order;
        for (size_t order = 0; order <= active_max_order; order++) {
            info->free_blocks[order] = free_block_counts[order];
            info->free_bytes_by_order[order] = info->free_blocks[order] * block_size(order);
        }

        pmm_lock.release(flags);
    }

    size_t get_total_bytes() { return managed_bytes; }
    size_t get_free_bytes() { return free_bytes; }
    size_t get_system_bytes() { return system_bytes; }
    size_t get_physical_limit_bytes() { return physical_limit_bytes; }

    void for_each_direct_map_span(DirectMapSpanCallback cb, void *ctx) {
        if (!cb) return;
        for (size_t i = 0; i < direct_map_span_count; i++) {
            cb(direct_map_spans[i].start, direct_map_spans[i].end, ctx);
        }
    }

}  // namespace pmm