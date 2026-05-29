#include "memory/vmm.h"

#include "console.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "smp/scheduler.h"
#include "smp/smp.h"
#include "string.h"

extern "C" {
extern uint8_t _kernel_phys_start[];
extern uint8_t _kernel_phys_end[];

extern uint8_t _text_start[];
extern uint8_t _text_end[];

extern uint8_t _rodata_start[];
extern uint8_t _rodata_end[];

extern uint8_t _data_start[];
extern uint8_t _data_end[];

extern uint8_t _bss_start[];
extern uint8_t _bss_end[];
extern uint8_t higher_stack_guard[];
}

namespace vmm {
    static_assert(pmm::PAGE_SIZE == 4096, "VMM assumes 4 KiB base pages");
    static_assert((pmm::PAGE_SIZE / sizeof(uint64_t)) == 512,
                  "Page tables must contain exactly 512 entries");
    static_assert(sizeof(uint64_t) * 8 >= 52,
                  "Physical address calculations assume at least 52-bit addresses");
    static_assert((KSTACK_BASE_4L & (pmm::PAGE_SIZE - 1)) == 0, "KSTACK_BASE_4L must be page-aligned");
    static_assert((KSTACK_SIZE_4L & (pmm::PAGE_SIZE - 1)) == 0, "KSTACK_SIZE_4L must be page-aligned");
    static_assert((KSTACK_BASE_5L & (pmm::PAGE_SIZE - 1)) == 0, "KSTACK_BASE_5L must be page-aligned");
    static_assert((KSTACK_SIZE_5L & (pmm::PAGE_SIZE - 1)) == 0, "KSTACK_SIZE_5L must be page-aligned");
    static_assert(KSTACK_BASE_4L + KSTACK_SIZE_4L <= MMIO_BASE_4L, "4-level stacks overlap MMIO");
    static_assert(KSTACK_BASE_5L + KSTACK_SIZE_5L <= MMIO_BASE_5L, "5-level stacks overlap MMIO");
    static_assert(512 == (1 << 9), "Page table indexing assumes 9-bit levels");

    static bool supports_2mb_pages = false;
    static bool supports_1gb_pages = false;
    static bool supports_nx = false;
    static bool supports_pat = false;
    static bool supports_wc = false;
    static uint64_t phys_addr_mask = 0;
    static uint64_t mapped_direct_map_bytes = 0;
    static uint64_t allocated_page_table_bytes = 0;
    static uint8_t paging_levels = 4;
    static uint64_t active_user_top = 0x0000800000000000ULL;
    static uint64_t active_kstack_base = KSTACK_BASE_4L;
    static uint64_t active_kstack_size = KSTACK_SIZE_4L;
    static uint64_t active_mmio_base = MMIO_BASE_4L;
    static uint64_t active_mmio_size = MMIO_SIZE_4L;

    static constexpr uint64_t SOFTWARE_PTE_MASK =
        static_cast<uint64_t>(PageFlags::User) |
        static_cast<uint64_t>(PageFlags::Write) |
        static_cast<uint64_t>(PageFlags::WriteThrough) |
        static_cast<uint64_t>(PageFlags::NoCache) |
        static_cast<uint64_t>(PageFlags::DemandZero) |
        static_cast<uint64_t>(PageFlags::Guard) |
        static_cast<uint64_t>(PageFlags::PKeyMask) |
        static_cast<uint64_t>(PageFlags::NX);

    static uint64_t current_pml4_phys = 0;

    static uint64_t kernel_section_phys(uint64_t virt) {
        return (uint64_t)_kernel_phys_start + (virt - (uint64_t)_text_start);
    }

    static uint64_t alloc_page_table() {
        uint64_t phys = pmm::alloc(pmm::PAGE_SIZE);
        if (phys) allocated_page_table_bytes += pmm::PAGE_SIZE;
        return phys;
    }

    static void free_page_table(uint64_t phys) {
        if (!phys) return;
        if (allocated_page_table_bytes < pmm::PAGE_SIZE) kpanic("VMM: page-table accounting underflow");
        allocated_page_table_bytes -= pmm::PAGE_SIZE;
        pmm::free(phys, pmm::PAGE_SIZE);
    }

    static VirtualRangeAllocator *mmio_allocator = nullptr;
    static VirtualRangeAllocator *kstack_allocator = nullptr;
    static spinlock_t tlb_shootdown_lock;

    static inline void invlpg(uint64_t virt) {
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }

    void flush_tlb(uint64_t pagemap, uint64_t virt, uint64_t pages) {
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));

        if (pagemap == 0 || pagemap == cr3) {
            if (pages == 1) {
                invlpg(virt);
            } else {
                asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
            }
        }

        uint64_t cores = smp::get_core_count();
        if (cores <= 1) return;

        uint64_t flags;
        tlb_shootdown_lock.acquire(flags);

        smp::cpu_local *cpu = smp::get_cpu();
        uint64_t self = cpu ? cpu->cpu_index : UINT64_MAX;
        uint64_t pending = 0;
        uint32_t page_count = pages > UINT32_MAX ? 0 : (uint32_t)pages;
        if (pages > 256) page_count = 0;


        for (uint64_t i = 0; i < cores; i++) {
            if (i == self) continue;
            smp::cpu_local *target = smp::get_cpu_by_index(i);
            if (!target) continue;

            if (pagemap != 0) {
                scheduler::task *remote = target->current_task;
                if (!remote || remote->cr3 != pagemap) continue;
            }

            if (!target->tlb_shootdown_mail.handled) {
                tlb_shootdown_lock.release(flags);
                kpanic("VMM: stale TLB shootdown mail");
            }
            if (!smp::send_tlb_shootdown_mail((int64_t)i, &target->tlb_shootdown_mail, pagemap,
                                             virt, page_count)) {
                tlb_shootdown_lock.release(flags);
                kpanic("VMM: failed to enqueue TLB shootdown");
            }
            pending++;
        }

        if (pending != 0) {
            smp::flush_mail(smp::MAIL_RECEIVER_OTHERS);

            for (uint64_t i = 0; i < cores; i++) {
                if (i == self) continue;
                smp::cpu_local *target = smp::get_cpu_by_index(i);
                if (!target) continue;
                while (!target->tlb_shootdown_mail.handled) asm volatile("pause");
            }
        }

        tlb_shootdown_lock.release(flags);
    }

    static inline uint64_t *get_table_ptr(uint64_t phys_addr) {
        return reinterpret_cast<uint64_t *>(p2v(phys_addr & get_phys_addr_mask()));
    }

    static uint64_t *get_next_table(uint64_t *v_table, uint64_t index, bool allocate) {
        uint64_t entry = v_table[index];

        if (entry & static_cast<uint64_t>(PageFlags::Present)) {
            if (entry & static_cast<uint64_t>(PageFlags::Huge)) { return nullptr; }
            return get_table_ptr(entry);
        }

        if (!allocate) return nullptr;

        uint64_t new_table_phys = alloc_page_table();
        if (!new_table_phys) return nullptr;

        memset(p2v(new_table_phys), 0, pmm::PAGE_SIZE);

        v_table[index] =
            (new_table_phys & get_phys_addr_mask()) |
            static_cast<uint64_t>(PageFlags::Present | PageFlags::Write | PageFlags::User);

        return get_table_ptr(new_table_phys);
    }



    static inline uint64_t level_shift(uint32_t level) {
        return 12 + 9ULL * (paging_levels - 1 - level);
    }

    static inline uint64_t level_index(uint64_t virt, uint32_t level) {
        return (virt >> level_shift(level)) & 0x1FFULL;
    }

    static uint64_t *walk_to_level(uint64_t root_phys, uint64_t virt, uint32_t target_level,
                                   bool allocate) {
        uint64_t *table = get_table_ptr(root_phys);
        for (uint32_t level = 0; level < target_level; level++) {
            table = get_next_table(table, level_index(virt, level), allocate);
            if (!table) return nullptr;
        }
        return table;
    }

    static inline bool user_address(uint64_t virt) { return virt < active_user_top; }

    uint64_t get_phys_addr_mask() { return phys_addr_mask; }

    bool nx_supported() { return supports_nx; }

    bool pat_supported() { return supports_pat; }

    bool write_combining_supported() { return supports_wc; }

    bool page_1g_supported() { return supports_1gb_pages; }

    bool five_level_paging_enabled() { return paging_levels == 5; }

    uint64_t paging_level_count() { return paging_levels; }

    uint64_t virtual_address_bits() { return five_level_paging_enabled() ? 57 : 48; }

    bool demand_zero_supported() { return true; }

    bool guard_page_supported() { return true; }

    uint64_t user_top() { return active_user_top; }

    uint64_t user_stack_top() { return active_user_top - pmm::PAGE_SIZE; }

    uint64_t user_mmap_base_min() { return five_level_paging_enabled() ? 0x0000200000000000ULL : 0x0000200000000000ULL; }

    uint64_t user_mmap_aslr_window() {
        return five_level_paging_enabled() ? 0x0008000000000000ULL : 0x0000100000000000ULL;
    }

    uint64_t direct_map_bytes() { return mapped_direct_map_bytes; }

    uint64_t page_table_bytes() { return allocated_page_table_bytes; }

    static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t &eax, uint32_t &ebx,
                             uint32_t &ecx, uint32_t &edx) {
        asm volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(leaf), "c"(subleaf));
    }

    // assumes heap is ready after end of vmm init, which it really should be.
    void init() {
        paging_runtime_cache_values();

        paging_levels = paging_mode_la57_value() ? 5 : 4;
        active_user_top = paging_user_top_value();
        active_kstack_base = five_level_paging_enabled() ? KSTACK_BASE_5L : KSTACK_BASE_4L;
        active_kstack_size = five_level_paging_enabled() ? KSTACK_SIZE_5L : KSTACK_SIZE_4L;
        active_mmio_base = five_level_paging_enabled() ? MMIO_BASE_5L : MMIO_BASE_4L;
        active_mmio_size = five_level_paging_enabled() ? MMIO_SIZE_5L : MMIO_SIZE_4L;

        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

        cpuid(0x00000000, 0, eax, ebx, ecx, edx);
        uint32_t max_basic_leaf = eax;

        if (max_basic_leaf >= 0x00000001) {
            cpuid(0x00000001, 0, eax, ebx, ecx, edx);
            supports_2mb_pages = edx & (1 << 3);
            supports_pat = edx & (1 << 16);
            supports_wc = supports_pat;
        }

        cpuid(0x80000000, 0, eax, ebx, ecx, edx);
        uint32_t max_extended_leaf = eax;

        if (max_extended_leaf >= 0x80000001) {
            cpuid(0x80000001, 0, eax, ebx, ecx, edx);
            supports_nx = edx & (1 << 20);
            supports_1gb_pages = edx & (1 << 26);
        }

        if (max_extended_leaf >= 0x80000008) {
            cpuid(0x80000008, 0, eax, ebx, ecx, edx);
            uint32_t phys_bits = eax & 0xFF;
            if (phys_bits == 0 || phys_bits > 52) phys_bits = 52;
            phys_addr_mask = ((1ULL << phys_bits) - 1) & ~0xFFFULL;
        } else {
            phys_addr_mask = 0x000FFFFFFFFFF000ULL;
        }

        current_pml4_phys = alloc_page_table();
        memset(p2v(current_pml4_phys), 0, pmm::PAGE_SIZE);

        map_range((uint64_t)_text_start, kernel_section_phys((uint64_t)_text_start),
                  (uint64_t)_text_end - (uint64_t)_text_start, PageFlags::Global);
        map_range((uint64_t)_rodata_start, kernel_section_phys((uint64_t)_rodata_start),
                  (uint64_t)_rodata_end - (uint64_t)_rodata_start,
                  PageFlags::NX | PageFlags::Global);
        map_range((uint64_t)_data_start, kernel_section_phys((uint64_t)_data_start),
                  (uint64_t)_data_end - (uint64_t)_data_start,
                  PageFlags::Write | PageFlags::NX | PageFlags::Global);
        for (uint64_t virt = (uint64_t)_bss_start; virt < (uint64_t)_bss_end;
             virt += pmm::PAGE_SIZE) {
            if (virt == (uint64_t)higher_stack_guard) continue;
            map_page(virt, kernel_section_phys(virt),
                     PageFlags::Write | PageFlags::NX | PageFlags::Global, PageSize::Size4K);
        }

        constexpr uint64_t LOW_DIRECT_MAP_BYTES = 4ULL * 1024 * 1024 * 1024;
        if (LOW_DIRECT_MAP_BYTES > paging_phys_direct_map_size_value()) {
            kpanic("VMM: physical direct map window exhausted");
        }

        mapped_direct_map_bytes = LOW_DIRECT_MAP_BYTES;
        map_range(paging_phys_map_base_value(), 0, LOW_DIRECT_MAP_BYTES, PageFlags::Write | PageFlags::NX);

        struct DirectMapCtx {
            uint64_t mapped_bytes;
        } direct_ctx = {LOW_DIRECT_MAP_BYTES};

        pmm::for_each_direct_map_span([](uint64_t start, uint64_t end, void *ptr) {
            DirectMapCtx *ctx = (DirectMapCtx *)ptr;
            if (end <= LOW_DIRECT_MAP_BYTES) return;
            if (start < LOW_DIRECT_MAP_BYTES) start = LOW_DIRECT_MAP_BYTES;
            if (end > paging_phys_direct_map_size_value()) {
                kpanic("VMM: physical direct map window exhausted");
            }
            map_range(paging_phys_map_base_value() + start, start, end - start,
                      PageFlags::Write | PageFlags::NX);
            ctx->mapped_bytes += end - start;
        }, &direct_ctx);
        mapped_direct_map_bytes = direct_ctx.mapped_bytes;
        map_range(0, 0, 0x100000, PageFlags::Write);

        asm volatile("mov %0, %%cr3" : : "r"(current_pml4_phys) : "memory");

        kstack_allocator = new VirtualRangeAllocator(active_kstack_base, active_kstack_size);
        mmio_allocator = new VirtualRangeAllocator(active_mmio_base, active_mmio_size);
    }

    static uint64_t make_leaf_flags(PageFlags flags, PageSize size) {
        uint64_t hw_flags = static_cast<uint64_t>(flags | PageFlags::Present);
        hw_flags &= ~static_cast<uint64_t>(PageFlags::WriteCombining);

        if (size != PageSize::Size4K) hw_flags |= static_cast<uint64_t>(PageFlags::Huge);
        if (!supports_nx) hw_flags &= ~static_cast<uint64_t>(PageFlags::NX);

        if ((flags & PageFlags::WriteCombining) != PageFlags::None && supports_wc) {
            hw_flags &= ~(static_cast<uint64_t>(PageFlags::WriteThrough) |
                          static_cast<uint64_t>(PageFlags::NoCache));
            hw_flags |= size == PageSize::Size4K ? (1ULL << 7) : (1ULL << 12);
        }

        return hw_flags;
    }

    static void map_page_internal(uint64_t virt, uint64_t phys, PageFlags flags, PageSize size,
                                  uint64_t pagemap, bool do_flush) {
        uint32_t leaf_level = paging_levels - 1;
        if (size == PageSize::Size1G) leaf_level = paging_levels - 3;
        if (size == PageSize::Size2M) leaf_level = paging_levels - 2;

        uint64_t *table = walk_to_level(pagemap, virt, leaf_level, true);
        if (!table) kpanic("VMM: failed to allocate page table");

        if (size == PageSize::Size1G && !supports_1gb_pages) kpanic("VMM: 1GB pages unsupported");
        if (size == PageSize::Size2M && !supports_2mb_pages) kpanic("VMM: 2MB pages unsupported");

        table[level_index(virt, leaf_level)] = (phys & get_phys_addr_mask()) | make_leaf_flags(flags, size);
        if (do_flush) flush_tlb(pagemap, virt, 1);
    }

    void map_page(uint64_t virt, uint64_t phys, PageFlags flags, PageSize size, uint64_t pagemap) {
        map_page_internal(virt, phys, flags, size, pagemap, true);
    }

    void map_range(uint64_t virt, uint64_t phys, uint64_t size, PageFlags flags, uint64_t pagemap) {
        if (size == 0) return;
        if ((virt & (pmm::PAGE_SIZE - 1)) || (phys & (pmm::PAGE_SIZE - 1))) {
            kpanic("VMM: map_range called with unaligned address");
        }
        if (size > UINT64_MAX - (pmm::PAGE_SIZE - 1)) kpanic("VMM: map_range overflow");

        uint64_t rounded_size = align_up(size, pmm::PAGE_SIZE);
        if (virt > UINT64_MAX - rounded_size || phys > UINT64_MAX - rounded_size) {
            kpanic("VMM: map_range overflow");
        }

        uint64_t curr_virt = virt;
        uint64_t curr_phys = phys;
        uint64_t remaining = rounded_size;
        bool user_mapping = (flags & PageFlags::User) != PageFlags::None;

        while (remaining != 0) {
            PageSize page_size = PageSize::Size4K;
            uint64_t step = pmm::PAGE_SIZE;

            if (!user_mapping && supports_1gb_pages && remaining >= 0x40000000ULL &&
                (curr_virt & 0x3FFFFFFFULL) == 0 && (curr_phys & 0x3FFFFFFFULL) == 0) {
                page_size = PageSize::Size1G;
                step = 0x40000000ULL;
            } else if (!user_mapping && supports_2mb_pages && remaining >= 0x200000ULL &&
                       (curr_virt & 0x1FFFFFULL) == 0 && (curr_phys & 0x1FFFFFULL) == 0) {
                page_size = PageSize::Size2M;
                step = 0x200000ULL;
            }

            map_page_internal(curr_virt, curr_phys, flags, page_size, pagemap, false);
            curr_virt += step;
            curr_phys += step;
            remaining -= step;
        }

        flush_tlb(pagemap, virt, rounded_size / pmm::PAGE_SIZE);
    }

    static uint64_t make_software_pte(PageFlags flags, PageFlags state) {
        uint64_t entry = static_cast<uint64_t>(flags | state) & SOFTWARE_PTE_MASK;
        entry &= ~static_cast<uint64_t>(PageFlags::Present);
        return entry;
    }

    static void map_software_page(uint64_t virt, PageFlags flags, PageFlags state,
                                  uint64_t pagemap, bool do_flush) {
        if (virt & (pmm::PAGE_SIZE - 1)) kpanic("VMM: software page is unaligned");

        uint64_t *table = walk_to_level(pagemap, virt, paging_levels - 1, true);
        if (!table) kpanic("VMM: failed to allocate page table");
        table[level_index(virt, paging_levels - 1)] = make_software_pte(flags, state);
        if (do_flush) flush_tlb(pagemap, virt, 1);
    }

    void map_demand_zero_range(uint64_t virt, uint64_t size, PageFlags flags, uint64_t pagemap) {
        if (size == 0) return;
        if (virt & (pmm::PAGE_SIZE - 1)) kpanic("VMM: demand-zero range is unaligned");
        if (size > UINT64_MAX - (pmm::PAGE_SIZE - 1)) kpanic("VMM: demand-zero range overflow");

        uint64_t rounded_size = align_up(size, pmm::PAGE_SIZE);
        if (virt > UINT64_MAX - rounded_size) kpanic("VMM: demand-zero range overflow");

        for (uint64_t off = 0; off < rounded_size; off += pmm::PAGE_SIZE) {
            map_software_page(virt + off, flags, PageFlags::DemandZero, pagemap, false);
        }
        flush_tlb(pagemap, virt, rounded_size / pmm::PAGE_SIZE);
    }

    void map_guard_page(uint64_t virt, PageFlags flags, uint64_t pagemap) {
        map_software_page(virt, flags, PageFlags::Guard, pagemap, true);
    }

    static void unmap_page_internal(uint64_t virt, uint64_t pagemap, bool do_flush) {
        if (virt % pmm::PAGE_SIZE != 0) {
            kpanic("VMM: unmap_page called with unaligned virtual address.");
        }

        uint64_t *table = get_table_ptr(pagemap);
        for (uint32_t level = 0; level < paging_levels; level++) {
            uint64_t index = level_index(virt, level);
            uint64_t entry = table[index];
            bool leaf = level + 1 == paging_levels;
            if (!(entry & static_cast<uint64_t>(PageFlags::Present))) {
                if (leaf && entry != 0) {
                    table[index] = 0;
                    goto flush;
                }
                return;
            }
            bool huge_1g = level + 3 == paging_levels &&
                           (entry & static_cast<uint64_t>(PageFlags::Huge));
            bool huge_2m = level + 2 == paging_levels &&
                           (entry & static_cast<uint64_t>(PageFlags::Huge));

            if (huge_1g) {
                if (virt % (1024ULL * 1024 * 1024) != 0) {
                    kpanic("VMM: Attempted to unmap part of a 1GB huge page.");
                }
                table[index] = 0;
                goto flush;
            }
            if (huge_2m) {
                if (virt % (2 * 1024 * 1024) != 0) {
                    kpanic("VMM: Attempted to unmap part of a 2MB huge page.");
                }
                table[index] = 0;
                goto flush;
            }
            if (leaf) {
                table[index] = 0;
                goto flush;
            }
            if (entry & static_cast<uint64_t>(PageFlags::Huge)) return;
            table = get_table_ptr(entry);
        }

    flush:
        if (do_flush) flush_tlb(pagemap, virt, 1);
    }

    void unmap_page(uint64_t virt, uint64_t pagemap) { unmap_page_internal(virt, pagemap, true); }

    void unmap_range(uint64_t virt, uint64_t size, uint64_t pagemap) {
        if (size == 0) return;
        if (virt > UINT64_MAX - size) kpanic("VMM: unmap_range overflow");

        uint64_t start = virt & ~0xFFFULL;
        uint64_t last = virt + size - 1;
        uint64_t end = (last & ~0xFFFULL) + pmm::PAGE_SIZE;
        if (end < start) kpanic("VMM: unmap_range overflow");

        for (uintptr_t curr = start; curr < end; curr += pmm::PAGE_SIZE) {
            unmap_page_internal(curr, pagemap, false);
        }

        if (end > start) flush_tlb(pagemap, start, (end - start) / pmm::PAGE_SIZE);
    }

    uint64_t create_user_address_space() {
        uint64_t root_phys = alloc_page_table();
        if (!root_phys) return 0;

        uint64_t *new_root = (uint64_t *)p2v(root_phys);
        uint64_t *kernel_root = (uint64_t *)p2v(current_pml4_phys);

        memset(new_root, 0, pmm::PAGE_SIZE);
        for (int i = 256; i < 512; i++) { new_root[i] = kernel_root[i]; }

        map_range(0, 0, 0x100000, PageFlags::Write, root_phys);
        return root_phys;
    }

    static PageFlags clone_leaf_flags(uint64_t entry) {
        uint64_t keep = static_cast<uint64_t>(PageFlags::Write) |
                        static_cast<uint64_t>(PageFlags::User) |
                        static_cast<uint64_t>(PageFlags::WriteThrough) |
                        static_cast<uint64_t>(PageFlags::NoCache) |
                        static_cast<uint64_t>(PageFlags::WriteCombining) |
                        static_cast<uint64_t>(PageFlags::CoW) |
                        static_cast<uint64_t>(PageFlags::PKeyMask) |
                        static_cast<uint64_t>(PageFlags::NX);
        return static_cast<PageFlags>(entry & keep);
    }

    static bool clone_user_table(uint64_t *old_table, uint32_t level, uint64_t prefix,
                                 uint64_t new_root) {
        constexpr uint64_t PRESENT = static_cast<uint64_t>(PageFlags::Present);
        constexpr uint64_t USER = static_cast<uint64_t>(PageFlags::User);
        constexpr uint64_t WRITE = static_cast<uint64_t>(PageFlags::Write);
        constexpr uint64_t HUGE = static_cast<uint64_t>(PageFlags::Huge);
        constexpr uint64_t COW = static_cast<uint64_t>(PageFlags::CoW);

        uint64_t shift = level_shift(level);
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t virt = prefix | (i << shift);
            if (!user_address(virt)) continue;

            uint64_t *leaf = &old_table[i];
            uint64_t entry = *leaf;
            bool pte = level + 1 == paging_levels;
            if (!(entry & PRESENT)) {
                if (pte && (entry & USER) &&
                    (entry & (static_cast<uint64_t>(PageFlags::DemandZero) |
                              static_cast<uint64_t>(PageFlags::Guard)))) {
                    map_software_page(virt, static_cast<PageFlags>(entry),
                                      (entry & static_cast<uint64_t>(PageFlags::DemandZero))
                                          ? PageFlags::DemandZero
                                          : PageFlags::Guard,
                                      new_root, false);
                }
                continue;
            }
            if ((entry & USER) == 0) continue;

            bool huge_1g = level + 3 == paging_levels && (entry & HUGE);
            bool huge_2m = level + 2 == paging_levels && (entry & HUGE);

            if (huge_1g || huge_2m || pte) {
                if (entry & WRITE) {
                    entry = (entry & ~WRITE) | COW;
                    *leaf = entry;
                }
                uint64_t phys = entry & get_phys_addr_mask();
                pmm::ref_page(phys);
                PageSize size = huge_1g ? PageSize::Size1G : (huge_2m ? PageSize::Size2M : PageSize::Size4K);
                map_page(virt, phys, clone_leaf_flags(entry), size, new_root);
                continue;
            }

            if (entry & HUGE) return false;
            if (!clone_user_table(get_table_ptr(entry), level + 1, virt, new_root)) return false;
        }
        return true;
    }

    uint64_t clone_address_space(uint64_t old_pml4_phys) {
        uint64_t new_root = create_user_address_space();
        if (!new_root) return 0;

        if (!clone_user_table((uint64_t *)p2v(old_pml4_phys), 0, 0, new_root)) {
            destroy_user_address_space(new_root);
            return 0;
        }

        flush_tlb(old_pml4_phys, 0, 0);
        return new_root;
    }

    static uint64_t *get_pte_ptr(uint64_t virt, uint64_t pml4_phys) {
        uint64_t *pt = walk_to_level(pml4_phys, virt, paging_levels - 1, false);
        if (!pt) return nullptr;
        return &pt[level_index(virt, paging_levels - 1)];
    }

    static bool stack_vma_allows_fault(scheduler::task *task, uint64_t page, bool write);

    uint64_t get_mapping(uint64_t virt, uint64_t pml4_phys) {
        constexpr uint64_t PRESENT = static_cast<uint64_t>(PageFlags::Present);
        constexpr uint64_t HUGE = static_cast<uint64_t>(PageFlags::Huge);

        uint64_t *table = get_table_ptr(pml4_phys);
        for (uint32_t level = 0; level < paging_levels; level++) {
            uint64_t entry = table[level_index(virt, level)];
            if (!(entry & PRESENT)) return 0;

            bool huge_1g = level + 3 == paging_levels && (entry & HUGE);
            bool huge_2m = level + 2 == paging_levels && (entry & HUGE);
            bool pte = level + 1 == paging_levels;
            if (huge_1g || huge_2m || pte) {
                uint64_t page_size = pte ? pmm::PAGE_SIZE :
                                     (huge_1g ? 1024ULL * 1024 * 1024 : 2ULL * 1024 * 1024);
                uint64_t phys = (entry & get_phys_addr_mask()) + (virt & (page_size - 1));
                return phys & ~(pmm::PAGE_SIZE - 1);
            }
            if (entry & HUGE) return 0;
            table = get_table_ptr(entry);
        }
        return 0;
    }

    static bool user_page_accessible(uint64_t virt, bool write, uint64_t pml4_phys) {
        constexpr uint64_t PRESENT = static_cast<uint64_t>(PageFlags::Present);
        constexpr uint64_t USER = static_cast<uint64_t>(PageFlags::User);
        constexpr uint64_t WRITE = static_cast<uint64_t>(PageFlags::Write);
        constexpr uint64_t HUGE = static_cast<uint64_t>(PageFlags::Huge);
        constexpr uint64_t COW = static_cast<uint64_t>(PageFlags::CoW);
        constexpr uint64_t DEMAND_ZERO = static_cast<uint64_t>(PageFlags::DemandZero);
        constexpr uint64_t GUARD = static_cast<uint64_t>(PageFlags::Guard);

        if (!user_address(virt)) return false;

        uint64_t *table = get_table_ptr(pml4_phys);
        for (uint32_t level = 0; level < paging_levels; level++) {
            uint64_t entry = table[level_index(virt, level)];
            bool pte = level + 1 == paging_levels;
            if (!(entry & PRESENT)) {
                if (pte && (entry & USER) && !(entry & GUARD) && (entry & DEMAND_ZERO)) {
                    return !write || (entry & WRITE);
                }
                smp::cpu_local *cpu = smp::get_cpu();
                scheduler::task *current = cpu ? cpu->current_task : nullptr;
                if (current && current->cr3 == pml4_phys && stack_vma_allows_fault(current, virt, write)) {
                    return true;
                }
                return false;
            }
            if ((entry & USER) == 0) return false;
            if (write && !(entry & WRITE) && !(pte && (entry & COW))) return false;

            bool huge = (level + 3 == paging_levels || level + 2 == paging_levels) && (entry & HUGE);
            if (huge || pte) return true;
            if (entry & HUGE) return false;
            table = get_table_ptr(entry);
        }
        return false;
    }

    bool user_range_mapped(uint64_t virt, uint64_t size, bool write, uint64_t pml4_phys) {
        if (size == 0) return true;
        if (virt == 0 || !user_address(virt)) return false;
        if (size > active_user_top - virt) return false;

        uint64_t start = virt & ~(pmm::PAGE_SIZE - 1);
        uint64_t last = (virt + size - 1) & ~(pmm::PAGE_SIZE - 1);

        for (uint64_t page = start;; page += pmm::PAGE_SIZE) {
            if (!user_page_accessible(page, write, pml4_phys)) return false;
            if (page == last) break;
        }
        return true;
    }

    bool set_user_pkey_range(uint64_t virt, uint64_t size, uint8_t pkey, uint64_t pml4_phys) {
        constexpr uint64_t PKEY_MASK = static_cast<uint64_t>(PageFlags::PKeyMask);
        constexpr uint64_t PRESENT = static_cast<uint64_t>(PageFlags::Present);
        constexpr uint64_t USER = static_cast<uint64_t>(PageFlags::User);
        constexpr uint64_t DEMAND_ZERO = static_cast<uint64_t>(PageFlags::DemandZero);
        constexpr uint64_t GUARD = static_cast<uint64_t>(PageFlags::Guard);
        if (pkey >= 16 || size == 0 || virt == 0 || !user_address(virt)) return false;
        if ((virt & (pmm::PAGE_SIZE - 1)) != 0 || (size & (pmm::PAGE_SIZE - 1)) != 0) return false;
        if (size > active_user_top - virt) return false;

        uint64_t end = virt + size;
        for (uint64_t page = virt; page < end; page += pmm::PAGE_SIZE) {
            uint64_t *pte = get_pte_ptr(page, pml4_phys);
            if (!pte || ((*pte & USER) == 0)) return false;
            if (((*pte & PRESENT) == 0) && (((*pte & DEMAND_ZERO) == 0) || (*pte & GUARD))) return false;
        }

        for (uint64_t page = virt; page < end; page += pmm::PAGE_SIZE) {
            uint64_t *pte = get_pte_ptr(page, pml4_phys);
            *pte = (*pte & ~PKEY_MASK) | ((uint64_t)pkey << 59);
        }
        flush_tlb(pml4_phys, virt, size / pmm::PAGE_SIZE);
        return true;
    }

    static bool stack_vma_allows_fault(scheduler::task *task, uint64_t page, bool write) {
        if (!task || !task->stack_vma) return false;
        scheduler::vm_area *vma = task->stack_vma;
        if (vma->type != scheduler::vma_type::STACK) return false;
        if (page < vma->committed_start || page >= vma->end) return false;
        if ((vma->flags & scheduler::VMA_READ) == 0) return false;
        if (write && (vma->flags & scheduler::VMA_WRITE) == 0) return false;
        return true;
    }

    static bool handle_stack_fault(uint64_t fault_addr, uint64_t error_code) {
        constexpr uint64_t PF_PRESENT = 1ULL << 0;
        constexpr uint64_t PF_WRITE = 1ULL << 1;
        constexpr uint64_t PF_INSTR = 1ULL << 4;
        if (error_code & PF_PRESENT) return false;
        if (error_code & PF_INSTR) return false;

        smp::cpu_local *cpu = smp::get_cpu();
        if (!cpu || !cpu->current_task) return false;

        scheduler::task *current = cpu->current_task;
        uint64_t page = fault_addr & ~(pmm::PAGE_SIZE - 1);
        if (!stack_vma_allows_fault(current, page, (error_code & PF_WRITE) != 0)) return false;

        uint64_t *pte = get_pte_ptr(page, current->cr3);
        if (pte && (*pte & static_cast<uint64_t>(PageFlags::Present))) return false;
        if (pte && (*pte & static_cast<uint64_t>(PageFlags::Guard))) return false;

        uint64_t phys = pmm::alloc(pmm::PAGE_SIZE);
        if (!phys) return false;
        memset(p2v(phys), 0, pmm::PAGE_SIZE);
        map_page(page, phys, PageFlags::Write | PageFlags::User | PageFlags::NX,
                 PageSize::Size4K, current->cr3);
        return true;
    }

    static bool handle_demand_zero_fault(uint64_t fault_addr, uint64_t error_code) {
        constexpr uint64_t PF_PRESENT = 1ULL << 0;
        constexpr uint64_t PF_WRITE = 1ULL << 1;
        constexpr uint64_t PF_USER = 1ULL << 2;
        constexpr uint64_t PF_INSTR = 1ULL << 4;
        constexpr uint64_t USER = static_cast<uint64_t>(PageFlags::User);
        constexpr uint64_t WRITE = static_cast<uint64_t>(PageFlags::Write);
        constexpr uint64_t DEMAND_ZERO = static_cast<uint64_t>(PageFlags::DemandZero);
        constexpr uint64_t GUARD = static_cast<uint64_t>(PageFlags::Guard);
        constexpr uint64_t NX = static_cast<uint64_t>(PageFlags::NX);

        if (error_code & PF_PRESENT) return false;

        smp::cpu_local *cpu = smp::get_cpu();
        if (!cpu || !cpu->current_task) return false;

        scheduler::task *current = cpu->current_task;
        uint64_t *pte = get_pte_ptr(fault_addr, current->cr3);
        if (!pte || !(*pte & DEMAND_ZERO) || (*pte & GUARD)) return false;
        if ((error_code & PF_USER) && !(*pte & USER)) return false;
        if ((error_code & PF_WRITE) && !(*pte & WRITE)) return false;
        if ((error_code & PF_INSTR) && (*pte & NX)) return false;

        uint64_t phys = pmm::alloc(pmm::PAGE_SIZE);
        if (!phys) return false;
        memset(p2v(phys), 0, pmm::PAGE_SIZE);

        uint64_t keep = (*pte & SOFTWARE_PTE_MASK) & ~DEMAND_ZERO;
        *pte = (phys & get_phys_addr_mask()) | keep | static_cast<uint64_t>(PageFlags::Present);
        flush_tlb(current->cr3, fault_addr & ~(pmm::PAGE_SIZE - 1), 1);
        return true;
    }

    bool fault_address_is_guard(uint64_t fault_addr, uint64_t pagemap) {
        if (pagemap == 0) {
            smp::cpu_local *cpu = smp::get_cpu();
            if (!cpu || !cpu->current_task) return false;
            pagemap = cpu->current_task->cr3;
        }

        uint64_t *pte = get_pte_ptr(fault_addr, pagemap);
        return pte && (*pte & static_cast<uint64_t>(PageFlags::Guard));
    }

    bool handle_fault(uint64_t fault_addr, uint64_t error_code, regs *) {
        if (handle_demand_zero_fault(fault_addr, error_code)) return true;
        if (handle_stack_fault(fault_addr, error_code)) return true;

        bool is_write = error_code & (1 << 1);
        if (!is_write) return false;

        scheduler::task *current = smp::get_cpu()->current_task;
        uint64_t *pte = get_pte_ptr(fault_addr, current->cr3);

        if (!pte) return false;

        constexpr uint64_t USER = static_cast<uint64_t>(PageFlags::User);
        constexpr uint64_t WRITE = static_cast<uint64_t>(PageFlags::Write);
        constexpr uint64_t COW = static_cast<uint64_t>(PageFlags::CoW);
        constexpr uint64_t KEEP_FLAGS = 0xFFFULL |
                                        static_cast<uint64_t>(PageFlags::PKeyMask) |
                                        static_cast<uint64_t>(PageFlags::NX);

        bool cow = (*pte & COW) != 0;
        if (!cow) {
            scheduler::vm_area *vma = scheduler::find_vma(current, fault_addr);
            if (!vma || fault_addr >= vma->end || (vma->flags & scheduler::VMA_WRITE) == 0) {
                return false;
            }
            if ((*pte & USER) == 0 || (*pte & WRITE) != 0) return false;
        }

        uint64_t old_phys = *pte & get_phys_addr_mask();
        if (pmm::get_ref(old_phys) == 1) {
            *pte = (*pte & ~COW) | WRITE;
        } else {
            uint64_t new_phys = pmm::alloc(pmm::PAGE_SIZE);
            if (!new_phys) return false;
            memcpy(p2v(new_phys), p2v(old_phys), pmm::PAGE_SIZE);
            pmm::unref_page(old_phys);
            *pte = (new_phys & get_phys_addr_mask()) | ((*pte & KEEP_FLAGS) & ~COW) | WRITE;
        }

        flush_tlb(current->cr3, fault_addr & ~(pmm::PAGE_SIZE - 1), 1);
        return true;
    }

    static void destroy_user_table(uint64_t table_phys, uint32_t level, uint64_t prefix) {
        uint64_t *table = (uint64_t *)p2v(table_phys & get_phys_addr_mask());
        constexpr uint64_t PRESENT = static_cast<uint64_t>(PageFlags::Present);
        constexpr uint64_t USER = static_cast<uint64_t>(PageFlags::User);
        constexpr uint64_t HUGE = static_cast<uint64_t>(PageFlags::Huge);

        uint64_t shift = level_shift(level);
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t virt = prefix | (i << shift);
            if (!user_address(virt)) continue;

            uint64_t entry = table[i];
            if ((entry & PRESENT) == 0 || (entry & USER) == 0) continue;

            bool huge = (level + 3 == paging_levels || level + 2 == paging_levels) && (entry & HUGE);
            bool pte = level + 1 == paging_levels;
            if (huge || pte) {
                pmm::unref_page(entry & get_phys_addr_mask());
                continue;
            }
            if (entry & HUGE) continue;
            destroy_user_table(entry & get_phys_addr_mask(), level + 1, virt);
        }

        free_page_table(table_phys);
    }

    void destroy_user_address_space(uint64_t pml4_phys) {
        destroy_user_table(pml4_phys, 0, 0);
    }

    uint64_t get_kernel_pagemap() { return current_pml4_phys; }

    VirtualRangeAllocator::VirtualRangeAllocator(uint64_t base, uint64_t size)
        : m_base(base), m_total_size(size) {
        spinlock_init(&m_lock);
        m_head = static_cast<Segment *>(heap::kmalloc(sizeof(Segment)));
        m_head->start = base;
        m_head->size = size;
        m_head->is_free = true;
        m_head->next = nullptr;
        m_head->prev = nullptr;
    }

    VirtualRangeAllocator::~VirtualRangeAllocator() {
        uint64_t flags;
        m_lock.acquire(flags);

        Segment *curr = m_head;
        while (curr) {
            Segment *next = curr->next;
            heap::kfree(curr);
            curr = next;
        }

        m_lock.release(flags);
    }

    uint64_t VirtualRangeAllocator::allocate(uint64_t size) {
        if (size == 0 || size > UINT64_MAX - (pmm::PAGE_SIZE - 1)) return 0;
        size = (size + pmm::PAGE_SIZE - 1) & ~(pmm::PAGE_SIZE - 1);

        uint64_t flags;
        m_lock.acquire(flags);

        Segment *curr = m_head;
        while (curr) {
            if (curr->is_free && curr->size >= size) {
                // Split the segment if it's larger than requested
                if (curr->size > size) {
                    Segment *new_seg = static_cast<Segment *>(heap::kmalloc(sizeof(Segment)));
                    if (!new_seg) {
                        m_lock.release(flags);
                        return 0;
                    }

                    new_seg->start = curr->start + size;
                    new_seg->size = curr->size - size;
                    new_seg->is_free = true;

                    new_seg->prev = curr;
                    new_seg->next = curr->next;

                    if (curr->next) curr->next->prev = new_seg;
                    curr->next = new_seg;
                    curr->size = size;
                }

                curr->is_free = false;
                uint64_t addr = curr->start;

                m_lock.release(flags);
                return addr;
            }
            curr = curr->next;
        }

        m_lock.release(flags);
        return 0;
    }

    void VirtualRangeAllocator::free(uint64_t virt_addr) {
        uint64_t flags;
        m_lock.acquire(flags);

        Segment *curr = m_head;
        while (curr) {
            if (curr->start == virt_addr) {
                curr->is_free = true;
                coalesce(curr);
                break;
            }
            curr = curr->next;
        }

        m_lock.release(flags);
    }

    void VirtualRangeAllocator::coalesce(Segment *seg) {
        // Merge with next segment if it is free
        if (seg->next && seg->next->is_free) {
            Segment *next_seg = seg->next;
            seg->size += next_seg->size;
            seg->next = next_seg->next;
            if (next_seg->next) next_seg->next->prev = seg;
            heap::kfree(next_seg);
        }

        // Merge with previous segment if it is free
        if (seg->prev && seg->prev->is_free) {
            Segment *prev_seg = seg->prev;
            prev_seg->size += seg->size;
            prev_seg->next = seg->next;
            if (seg->next) seg->next->prev = prev_seg;
            heap::kfree(seg);
        }
    }

    bool kernel_stack_range(void *stack_base, uint64_t usable_size, uint64_t *low_guard,
                            uint64_t *top) {
        if (!stack_base || usable_size == 0 || (usable_size & (pmm::PAGE_SIZE - 1)) != 0) {
            return false;
        }
        uint64_t base = reinterpret_cast<uint64_t>(stack_base);
        if ((base & (pmm::PAGE_SIZE - 1)) != 0) return false;
        if (base < active_kstack_base + pmm::PAGE_SIZE) return false;
        if (base + usable_size < base) return false;
        uint64_t high_guard = base + usable_size;
        if (high_guard + pmm::PAGE_SIZE < high_guard) return false;
        if (high_guard + pmm::PAGE_SIZE > active_kstack_base + active_kstack_size) return false;
        if (low_guard) *low_guard = base - pmm::PAGE_SIZE;
        if (top) *top = high_guard;
        return true;
    }

    void *alloc_kernel_stack(uint64_t usable_size) {
        if (!kstack_allocator || usable_size == 0 || (usable_size & (pmm::PAGE_SIZE - 1)) != 0) {
            return nullptr;
        }

        uint64_t total_size = usable_size + 2 * pmm::PAGE_SIZE;
        if (total_size < usable_size) return nullptr;

        uint64_t region = kstack_allocator->allocate(total_size);
        if (!region) return nullptr;

        uint64_t phys = pmm::alloc(usable_size);
        if (!phys) {
            kstack_allocator->free(region);
            return nullptr;
        }

        uint64_t base = region + pmm::PAGE_SIZE;
        map_range(base, phys, usable_size, PageFlags::Write | PageFlags::NX | PageFlags::Global);
        memset(reinterpret_cast<void *>(base), 0, usable_size);
        return reinterpret_cast<void *>(base);
    }

    void free_kernel_stack(void *stack_base, uint64_t usable_size) {
        uint64_t low_guard = 0;
        uint64_t top = 0;
        if (!kernel_stack_range(stack_base, usable_size, &low_guard, &top)) {
            kpanic("VMM: invalid kernel stack free");
        }

        uint64_t base = reinterpret_cast<uint64_t>(stack_base);
        uint64_t first_phys = get_mapping(base);
        if (!first_phys) kpanic("VMM: kernel stack missing mapping");

        for (uint64_t off = 0; off < usable_size; off += pmm::PAGE_SIZE) {
            uint64_t phys = get_mapping(base + off);
            if (!phys || phys != first_phys + off) kpanic("VMM: corrupt kernel stack mapping");
            unmap_page(base + off);
        }

        pmm::free(first_phys, usable_size);
        kstack_allocator->free(low_guard);
    }

    static void *mmio_map_with_flags(uint64_t phys_addr, uint64_t size, PageFlags flags) {
        if (!mmio_allocator || size == 0) return nullptr;

        uint64_t offset = phys_addr & (pmm::PAGE_SIZE - 1);
        uint64_t phys_page = phys_addr & ~(pmm::PAGE_SIZE - 1);
        if (size > UINT64_MAX - offset || size + offset > UINT64_MAX - (pmm::PAGE_SIZE - 1)) return nullptr;
        uint64_t map_size = (size + offset + pmm::PAGE_SIZE - 1) & ~(pmm::PAGE_SIZE - 1);

        uint64_t virt = mmio_allocator->allocate(map_size);
        if (!virt) {
            console::printf("[ VMM ] Ran out of space to map mmio!\n");
            return nullptr;
        }

        map_range(virt, phys_page, map_size, flags | PageFlags::Write | PageFlags::NX);
        return (void *)(virt + offset);
    }

    void *mmio_map(uint64_t phys_addr, uint64_t size) {
        return mmio_map_with_flags(phys_addr, size, PageFlags::NoCache | PageFlags::WriteThrough);
    }

    void *mmio_map_wc(uint64_t phys_addr, uint64_t size) {
        PageFlags flags = supports_wc ? PageFlags::WriteCombining
                                      : PageFlags::NoCache | PageFlags::WriteThrough;
        return mmio_map_with_flags(phys_addr, size, flags);
    }

    void mmio_unmap(void *virt_addr, uint64_t size) {
        if (!mmio_allocator || !virt_addr || size == 0) return;

        uint64_t virt = reinterpret_cast<uint64_t>(virt_addr);
        uint64_t offset = virt & (pmm::PAGE_SIZE - 1);
        uint64_t virt_page = virt & ~(pmm::PAGE_SIZE - 1);
        if (size > UINT64_MAX - offset || size + offset > UINT64_MAX - (pmm::PAGE_SIZE - 1)) return;
        uint64_t map_size = (size + offset + pmm::PAGE_SIZE - 1) & ~(pmm::PAGE_SIZE - 1);

        unmap_range(virt_page, map_size);
        mmio_allocator->free(virt_page);
    }
}  // namespace vmm