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
extern uint8_t pml4_table[];
extern uint8_t pml4_table_end[];
extern uint8_t pdp_table[];
extern uint8_t pdp_table_end[];
extern uint8_t page_directory[];
extern uint8_t page_directory_end[];
extern uint8_t phys_map_pdp_table[];
extern uint8_t phys_map_pdp_table_end[];
extern uint8_t phys_map_pd_table[];
extern uint8_t phys_map_pd_table_end[];
extern uint8_t high_pdp_table[];
extern uint8_t high_pdp_table_end[];
extern uint8_t high_pd_table[];
extern uint8_t high_pd_table_end[];
}

namespace {
    static spinlock_t pmm_lock;

    size_t managed_bytes = 0;
    size_t free_bytes = 0;
    size_t system_bytes = 0;
    size_t physical_limit_bytes = 0;

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

    static FreeBlock *free_lists[MAX_ORDER + 1];

    size_t size_to_order(size_t size) {
        size = align_up(size, PAGE_SIZE);
        size_t pages = size / PAGE_SIZE;
        size_t order = 0;
        while ((1ULL << order) < pages) order++;
        return order;
    }

    static uint64_t block_size(size_t order) { return (1ULL << order) * PAGE_SIZE; }
    static uint64_t buddy_of(uint64_t addr, size_t order) { return addr ^ block_size(order); }

    static void push_block(uint64_t addr, size_t order) {
        FreeBlock *block = (FreeBlock *)p2v(addr);
        block->next = free_lists[order];
        free_lists[order] = block;
        free_bytes += block_size(order);
    }

    static uint64_t pop_block(size_t order) {
        FreeBlock *block = free_lists[order];
        if (!block) return 0;
        free_lists[order] = block->next;
        free_bytes -= block_size(order);
        return v2p(block);
    }

    static bool remove_block(uint64_t phys_addr, size_t order) {
        FreeBlock **cur = &free_lists[order];
        FreeBlock *target_virt = (FreeBlock *)p2v(phys_addr);
        while (*cur) {
            if (*cur == target_virt) {
                *cur = (*cur)->next;
                free_bytes -= block_size(order);
                return true;
            }
            cur = &(*cur)->next;
        }
        return false;
    }


    static void *alloc_metadata_page_locked() {
        for (size_t i = 0; i <= MAX_ORDER; i++) {
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

    static void defer_span_locked(uint64_t start, uint64_t end) {
        start = align_up(start, PAGE_SIZE);
        end = align_down(end, PAGE_SIZE);
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

        uint64_t curr = align_up(start, PAGE_SIZE);
        uint64_t last = align_down(end, PAGE_SIZE);

        while (curr < last) {
            uint64_t remaining = last - curr;
            size_t align_order = (curr == 0) ? MAX_ORDER : (size_t)__builtin_ctzll(curr >> 12);
            size_t size_order = 63 - __builtin_clzll(remaining >> 12);
            size_t order = align_order;
            if (order > size_order) order = size_order;
            if (order > MAX_ORDER) order = MAX_ORDER;

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
        deferred_span_count = 0;
        defer_unmapped_spans = true;

        for (size_t i = 0; i <= MAX_ORDER; i++) free_lists[i] = nullptr;
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
                    if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
                        if (e->addr + e->len > max_phys_addr) max_phys_addr = e->addr + e->len;
                    }
                }
            }
        }

        if (!mmap) return;

        physical_limit_bytes = align_up(max_phys_addr, PAGE_SIZE);
        if (physical_limit_bytes > PHYS_DIRECT_MAP_SIZE) {
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
                               (uint64_t)&pml4_table,
                               (uint64_t)&pml4_table_end,
                               (uint64_t)&pdp_table,
                               (uint64_t)&pdp_table_end,
                               (uint64_t)&page_directory,
                               (uint64_t)&page_directory_end,
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

        for (auto *e = mmap->entries; (uint8_t *)e < (uint8_t *)mmap + mmap->size;
             e = (multiboot_mmap_entry *)((uint8_t *)e + mmap->entry_size)) {
            system_bytes += e->len;
            if (e->type != MULTIBOOT_MEMORY_AVAILABLE) continue;
            free_with_reservation(e->addr, e->addr + e->len, reserved, res_count);
        }

        // Refcount metadata is sparse and allocated lazily by alloc()/ref_page().
    }

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
        if (order > MAX_ORDER) {
            pmm_lock.release(flags);
            kpanic("PMM: free size exceeds MAX_ORDER");
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
        while (order < MAX_ORDER) {
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
        if (order > MAX_ORDER) {
            pmm_lock.release(flags);
            return 0;
        }
        for (size_t i = order; i <= MAX_ORDER; i++) {
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
            for (uint64_t off = 0; off < alloc_size; off += PAGE_SIZE) {
                if (!set_ref_locked(block + off, 1)) {
                    pmm_lock.release(flags);
                    kpanic("PMM: failed to allocate refcount metadata");
                }
            }

            pmm_lock.release(flags);
            return block;
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

        for (size_t order = 0; order <= MAX_ORDER; order++) {
            for (FreeBlock *b = free_lists[order]; b; b = b->next) {
                info->free_blocks[order]++;
            }
            info->free_bytes_by_order[order] = info->free_blocks[order] * block_size(order);
        }

        pmm_lock.release(flags);
    }

    size_t get_total_bytes() { return managed_bytes; }
    size_t get_free_bytes() { return free_bytes; }
    size_t get_system_bytes() { return system_bytes; }
    size_t get_physical_limit_bytes() { return physical_limit_bytes; }

}  // namespace pmm