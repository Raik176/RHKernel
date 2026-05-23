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
    static_assert((KSTACK_BASE & (pmm::PAGE_SIZE - 1)) == 0, "KSTACK_BASE must be page-aligned");
    static_assert((KSTACK_SIZE & (pmm::PAGE_SIZE - 1)) == 0, "KSTACK_SIZE must be page-aligned");
    static_assert(KSTACK_BASE + KSTACK_SIZE <= MMIO_BASE, "Kernel stacks must not overlap MMIO");
    static_assert(512 == (1 << 9), "Page table indexing assumes 9-bit levels");

    static bool supports_2mb_pages = false;
    static bool supports_1gb_pages = false;
    static bool supports_nx = false;
    static bool supports_pat = false;
    static bool supports_wc = false;
    static uint64_t phys_addr_mask = 0;
    static uint64_t mapped_direct_map_bytes = 0;

    static uint64_t current_pml4_phys = 0;

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
            smp::cpu_local *target = smp::get_cpu_by_index(i);
            if (target) target->tlb_shootdown_handled = true;
        }

        for (uint64_t i = 0; i < cores; i++) {
            if (i == self) continue;
            smp::cpu_local *target = smp::get_cpu_by_index(i);
            if (!target) continue;

            if (pagemap != 0) {
                scheduler::task *remote = target->current_task;
                if (!remote || remote->cr3 != pagemap) continue;
            }

            target->tlb_shootdown_handled = false;
            if (!smp::send_tlb_shootdown_mail((int64_t)i, &target->tlb_shootdown_mail, pagemap,
                                             virt, page_count, &target->tlb_shootdown_handled)) {
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
                while (!target->tlb_shootdown_handled) asm volatile("pause");
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

        uint64_t new_table_phys = pmm::alloc(pmm::PAGE_SIZE);
        if (!new_table_phys) return nullptr;

        memset(p2v(new_table_phys), 0, pmm::PAGE_SIZE);

        v_table[index] =
            (new_table_phys & get_phys_addr_mask()) |
            static_cast<uint64_t>(PageFlags::Present | PageFlags::Write | PageFlags::User);

        return get_table_ptr(new_table_phys);
    }

    uint64_t get_phys_addr_mask() { return phys_addr_mask; }

    bool nx_supported() { return supports_nx; }

    bool pat_supported() { return supports_pat; }

    bool write_combining_supported() { return supports_wc; }

    bool page_1g_supported() { return supports_1gb_pages; }

    uint64_t direct_map_bytes() { return mapped_direct_map_bytes; }

    static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t &eax, uint32_t &ebx,
                             uint32_t &ecx, uint32_t &edx) {
        asm volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(leaf), "c"(subleaf));
    }

    // assumes heap is ready after end of vmm init, which it really should be.
    void init() {
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

        current_pml4_phys = pmm::alloc(pmm::PAGE_SIZE);
        memset(p2v(current_pml4_phys), 0, pmm::PAGE_SIZE);

        map_range((uint64_t)_text_start, (uint64_t)_text_start - KERNEL_VIRT_OFFSET,
                  (uint64_t)_text_end - (uint64_t)_text_start, PageFlags::Global);
        map_range((uint64_t)_rodata_start, (uint64_t)_rodata_start - KERNEL_VIRT_OFFSET,
                  (uint64_t)_rodata_end - (uint64_t)_rodata_start,
                  PageFlags::NX | PageFlags::Global);
        map_range((uint64_t)_data_start, (uint64_t)_data_start - KERNEL_VIRT_OFFSET,
                  (uint64_t)_data_end - (uint64_t)_data_start,
                  PageFlags::Write | PageFlags::NX | PageFlags::Global);
        for (uint64_t virt = (uint64_t)_bss_start; virt < (uint64_t)_bss_end;
             virt += pmm::PAGE_SIZE) {
            if (virt == (uint64_t)higher_stack_guard) continue;
            map_page(virt, virt - KERNEL_VIRT_OFFSET,
                     PageFlags::Write | PageFlags::NX | PageFlags::Global, PageSize::Size4K);
        }

        // Keep the physical direct map large enough for both RAM and early MMIO.
        //
        // pmm::get_physical_limit_bytes() is derived from usable RAM entries. It can be
        // much smaller than device/MMIO physical addresses such as the QEMU framebuffer
        // at ~0xFD000000. console::init() stores the framebuffer as p2v(fb_phys), so
        // after switching to the final CR3 the framebuffer must still be covered by
        // the direct map. Otherwise the first printk after this point page-faults,
        // then the panic path recurses/triple-faults before anything useful is shown.
        constexpr uint64_t MIN_DIRECT_MAP_BYTES = 4ULL * 1024 * 1024 * 1024;
        uint64_t direct_map_bytes = pmm::get_physical_limit_bytes();
        if (direct_map_bytes < MIN_DIRECT_MAP_BYTES) direct_map_bytes = MIN_DIRECT_MAP_BYTES;
        if (direct_map_bytes > PHYS_DIRECT_MAP_SIZE) {
            kpanic("VMM: physical direct map window exhausted");
        }

        mapped_direct_map_bytes = direct_map_bytes;
        map_range(PHYS_MAP_BASE, 0, direct_map_bytes, PageFlags::Write | PageFlags::NX);
        map_range(0, 0, 0x100000, PageFlags::Write);

        asm volatile("mov %0, %%cr3" : : "r"(current_pml4_phys) : "memory");

        kstack_allocator = new VirtualRangeAllocator(KSTACK_BASE, KSTACK_SIZE);
        mmio_allocator = new VirtualRangeAllocator(MMIO_BASE, MMIO_SIZE);
    }

    static void map_page_internal(uint64_t virt, uint64_t phys, PageFlags flags, PageSize size,
                                  uint64_t pagemap, bool do_flush);

    void map_range(uint64_t virt, uint64_t phys, uint64_t size, PageFlags flags, uint64_t pml4) {
        const uint64_t GIB = 1024ULL * 1024 * 1024;
        const uint64_t MIB = 2ULL * 1024 * 1024;
        const uint64_t KIB = pmm::PAGE_SIZE;

        uint64_t mapped = 0;

        while (mapped < size) {
            uint64_t curr_v = virt + mapped;
            uint64_t curr_p = phys + mapped;
            uint64_t remaining = size - mapped;

            if (supports_1gb_pages && remaining >= GIB && (curr_v % GIB == 0) &&
                (curr_p % GIB == 0)) {
                map_page_internal(curr_v, curr_p, flags, PageSize::Size1G, pml4, false);
                mapped += GIB;
            } else if (supports_2mb_pages && remaining >= MIB && (curr_v % MIB == 0) &&
                       (curr_p % MIB == 0)) {
                map_page_internal(curr_v, curr_p, flags, PageSize::Size2M, pml4, false);
                mapped += MIB;
            } else {
                map_page_internal(curr_v, curr_p, flags, PageSize::Size4K, pml4, false);
                mapped += KIB;
            }
        }

        if (size != 0) flush_tlb(pml4, virt, ((size - 1) / pmm::PAGE_SIZE) + 1);
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
        uint64_t pml4_idx = (virt >> 39) & 0x1FF;
        uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
        uint64_t pd_idx = (virt >> 21) & 0x1FF;
        uint64_t pt_idx = (virt >> 12) & 0x1FF;

        uint64_t *pml4 = get_table_ptr(pagemap);
        uint64_t *pdpt = get_next_table(pml4, pml4_idx, true);

        if (size == PageSize::Size1G) {
            if (!supports_1gb_pages) kpanic("VMM: 1GB pages unsupported");
            uint64_t hw_flags = make_leaf_flags(flags, size);
            pdpt[pdpt_idx] = (phys & get_phys_addr_mask()) | hw_flags;
            goto flush;
        }

        {
            uint64_t *pd = get_next_table(pdpt, pdpt_idx, true);
            if (size == PageSize::Size2M) {
                if (!supports_2mb_pages) kpanic("VMM: 2MB pages unsupported");
                uint64_t hw_flags = make_leaf_flags(flags, size);
                pd[pd_idx] = (phys & get_phys_addr_mask()) | hw_flags;
                goto flush;
            }

            uint64_t *pt = get_next_table(pd, pd_idx, true);
            uint64_t hw_flags = make_leaf_flags(flags, size);
            pt[pt_idx] = (phys & get_phys_addr_mask()) | hw_flags;
        }

    flush:
        if (do_flush) flush_tlb(pagemap, virt, 1);
    }

    void map_page(uint64_t virt, uint64_t phys, PageFlags flags, PageSize size, uint64_t pagemap) {
        map_page_internal(virt, phys, flags, size, pagemap, true);
    }

    static void unmap_page_internal(uint64_t virt, uint64_t pagemap, bool do_flush) {
        if (virt % pmm::PAGE_SIZE != 0) {
            kpanic("VMM: unmap_page called with unaligned virtual address.");
        }

        uint64_t pml4_idx = (virt >> 39) & 0x1FF;
        uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
        uint64_t pd_idx = (virt >> 21) & 0x1FF;
        uint64_t pt_idx = (virt >> 12) & 0x1FF;

        uint64_t *pml4 = get_table_ptr(pagemap);
        uint64_t *pdpt = get_next_table(pml4, pml4_idx, false);
        if (!pdpt) return;

        if (pdpt[pdpt_idx] & static_cast<uint64_t>(PageFlags::Huge)) {
            if (virt % (1024ULL * 1024 * 1024) != 0) {
                kpanic("VMM: Attempted to unmap part of a 1GB huge page.");
            }
            pdpt[pdpt_idx] = 0;
            goto flush;
        }

        {
            uint64_t *pd = get_next_table(pdpt, pdpt_idx, false);
            if (!pd) return;

            if (pd[pd_idx] & static_cast<uint64_t>(PageFlags::Huge)) {
                if (virt % (2 * 1024 * 1024) != 0) {
                    kpanic("VMM: Attempted to unmap part of a 2MB huge page.");
                }
                pd[pd_idx] = 0;
                goto flush;
            }

            {
                uint64_t *pt = get_next_table(pd, pd_idx, false);
                if (!pt) return;
                pt[pt_idx] = 0;
            }
        }

    flush:
        if (do_flush) flush_tlb(pagemap, virt, 1);
    }

    void unmap_page(uint64_t virt, uint64_t pagemap) { unmap_page_internal(virt, pagemap, true); }

    void unmap_range(uint64_t virt, uint64_t size, uint64_t pagemap) {
        uint64_t start = virt & ~0xFFFULL;
        uint64_t end = (virt + size + 4095) & ~0xFFFULL;

        for (uintptr_t curr = start; curr < end; curr += pmm::PAGE_SIZE) {
            unmap_page_internal(curr, pagemap, false);
        }

        if (end > start) flush_tlb(pagemap, start, (end - start) / pmm::PAGE_SIZE);
    }

    uint64_t create_user_address_space() {
        uint64_t pml4_phys = pmm::alloc(pmm::PAGE_SIZE);
        if (!pml4_phys) return 0;

        uint64_t *new_pml4 = (uint64_t *)p2v(pml4_phys);
        uint64_t *kernel_pml4 = (uint64_t *)p2v(current_pml4_phys);

        memset(new_pml4, 0, pmm::PAGE_SIZE);

        for (int i = 256; i < 512; i++) { new_pml4[i] = kernel_pml4[i]; }

        map_range(0, 0, 0x100000, PageFlags::Write, pml4_phys);
        return pml4_phys;
    }

    static PageFlags clone_leaf_flags(uint64_t entry) {
        uint64_t keep = static_cast<uint64_t>(PageFlags::Write) |
                        static_cast<uint64_t>(PageFlags::User) |
                        static_cast<uint64_t>(PageFlags::WriteThrough) |
                        static_cast<uint64_t>(PageFlags::NoCache) |
                        static_cast<uint64_t>(PageFlags::WriteCombining) |
                        static_cast<uint64_t>(PageFlags::CoW) |
                        static_cast<uint64_t>(PageFlags::NX);
        return static_cast<PageFlags>(entry & keep);
    }

    uint64_t clone_address_space(uint64_t old_pml4_phys) {
        uint64_t new_pml4_phys = create_user_address_space();
        if (!new_pml4_phys) return 0;

        uint64_t *old_pml4 = (uint64_t *)p2v(old_pml4_phys);
        constexpr uint64_t PRESENT = static_cast<uint64_t>(PageFlags::Present);
        constexpr uint64_t USER = static_cast<uint64_t>(PageFlags::User);
        constexpr uint64_t WRITE = static_cast<uint64_t>(PageFlags::Write);
        constexpr uint64_t HUGE = static_cast<uint64_t>(PageFlags::Huge);
        constexpr uint64_t COW = static_cast<uint64_t>(PageFlags::CoW);

        for (uint64_t i = 0; i < 256; i++) {
            uint64_t pml4e = old_pml4[i];
            if ((pml4e & (PRESENT | USER)) != (PRESENT | USER)) continue;

            uint64_t *old_pdpt = (uint64_t *)p2v(pml4e & get_phys_addr_mask());
            for (uint64_t j = 0; j < 512; j++) {
                uint64_t *leaf = &old_pdpt[j];
                uint64_t entry = *leaf;
                uint64_t virt = (i << 39) | (j << 30);

                if (!(entry & PRESENT)) continue;
                if (entry & HUGE) {
                    if ((entry & USER) != USER) continue;
                    if (entry & WRITE) {
                        entry = (entry & ~WRITE) | COW;
                        *leaf = entry;
                    }
                    pmm::ref_page(entry & get_phys_addr_mask());
                    map_page(virt, entry & get_phys_addr_mask(), clone_leaf_flags(entry),
                             PageSize::Size1G, new_pml4_phys);
                    continue;
                }
                if ((entry & USER) != USER) continue;

                uint64_t *old_pd = (uint64_t *)p2v(entry & get_phys_addr_mask());
                for (uint64_t k = 0; k < 512; k++) {
                    leaf = &old_pd[k];
                    entry = *leaf;
                    virt = (i << 39) | (j << 30) | (k << 21);

                    if (!(entry & PRESENT)) continue;
                    if (entry & HUGE) {
                        if ((entry & USER) != USER) continue;
                        if (entry & WRITE) {
                            entry = (entry & ~WRITE) | COW;
                            *leaf = entry;
                        }
                        pmm::ref_page(entry & get_phys_addr_mask());
                        map_page(virt, entry & get_phys_addr_mask(), clone_leaf_flags(entry),
                                 PageSize::Size2M, new_pml4_phys);
                        continue;
                    }
                    if ((entry & USER) != USER) continue;

                    uint64_t *old_pt = (uint64_t *)p2v(entry & get_phys_addr_mask());
                    for (uint64_t l = 0; l < 512; l++) {
                        leaf = &old_pt[l];
                        entry = *leaf;
                        if ((entry & (PRESENT | USER)) != (PRESENT | USER)) continue;

                        if (entry & WRITE) {
                            entry = (entry & ~WRITE) | COW;
                            *leaf = entry;
                        }

                        uint64_t phys = entry & get_phys_addr_mask();
                        pmm::ref_page(phys);
                        virt = (i << 39) | (j << 30) | (k << 21) | (l << 12);
                        map_page(virt, phys, clone_leaf_flags(entry), PageSize::Size4K,
                                 new_pml4_phys);
                    }
                }
            }
        }

        flush_tlb(old_pml4_phys, 0, 0);
        return new_pml4_phys;
    }

    static uint64_t *get_pte_ptr(uint64_t virt, uint64_t pml4_phys) {
        uint64_t pml4_idx = (virt >> 39) & 0x1FF;
        uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
        uint64_t pd_idx = (virt >> 21) & 0x1FF;
        uint64_t pt_idx = (virt >> 12) & 0x1FF;

        uint64_t *pml4 = (uint64_t *)p2v(pml4_phys);
        uint64_t *pdpt = get_next_table(pml4, pml4_idx, false);
        if (!pdpt) return nullptr;

        uint64_t *pd = get_next_table(pdpt, pdpt_idx, false);
        if (!pd) return nullptr;

        uint64_t *pt = get_next_table(pd, pd_idx, false);
        if (!pt) return nullptr;

        return &pt[pt_idx];
    }

    uint64_t get_mapping(uint64_t virt, uint64_t pml4_phys) {
        uint64_t *pte = get_pte_ptr(virt, pml4_phys);
        if (!pte || !(*pte & static_cast<uint64_t>(PageFlags::Present))) return 0;
        return *pte & get_phys_addr_mask();
    }

    static bool user_page_accessible(uint64_t virt, bool write, uint64_t pml4_phys) {
        constexpr uint64_t USER_TOP = 0x0000800000000000ULL;
        constexpr uint64_t PRESENT = static_cast<uint64_t>(PageFlags::Present);
        constexpr uint64_t USER = static_cast<uint64_t>(PageFlags::User);
        constexpr uint64_t WRITE = static_cast<uint64_t>(PageFlags::Write);
        constexpr uint64_t HUGE = static_cast<uint64_t>(PageFlags::Huge);

        if (virt >= USER_TOP) return false;

        uint64_t pml4_idx = (virt >> 39) & 0x1FF;
        uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
        uint64_t pd_idx = (virt >> 21) & 0x1FF;
        uint64_t pt_idx = (virt >> 12) & 0x1FF;

        uint64_t *pml4 = (uint64_t *)p2v(pml4_phys);
        uint64_t pml4e = pml4[pml4_idx];
        if ((pml4e & (PRESENT | USER)) != (PRESENT | USER)) return false;
        if (write && !(pml4e & WRITE)) return false;

        uint64_t *pdpt = get_table_ptr(pml4e);
        uint64_t pdpte = pdpt[pdpt_idx];
        if ((pdpte & (PRESENT | USER)) != (PRESENT | USER)) return false;
        if (write && !(pdpte & WRITE)) return false;
        if (pdpte & HUGE) return true;

        uint64_t *pd = get_table_ptr(pdpte);
        uint64_t pde = pd[pd_idx];
        if ((pde & (PRESENT | USER)) != (PRESENT | USER)) return false;
        if (write && !(pde & WRITE)) return false;
        if (pde & HUGE) return true;

        uint64_t *pt = get_table_ptr(pde);
        uint64_t pte = pt[pt_idx];
        if ((pte & (PRESENT | USER)) != (PRESENT | USER)) return false;
        if (write && !(pte & (WRITE | static_cast<uint64_t>(PageFlags::CoW)))) return false;
        return true;
    }

    bool user_range_mapped(uint64_t virt, uint64_t size, bool write, uint64_t pml4_phys) {
        constexpr uint64_t USER_TOP = 0x0000800000000000ULL;
        if (size == 0) return true;
        if (virt == 0 || virt >= USER_TOP) return false;
        if (size > USER_TOP - virt) return false;

        uint64_t start = virt & ~(pmm::PAGE_SIZE - 1);
        uint64_t last = (virt + size - 1) & ~(pmm::PAGE_SIZE - 1);

        for (uint64_t page = start;; page += pmm::PAGE_SIZE) {
            if (!user_page_accessible(page, write, pml4_phys)) return false;
            if (page == last) break;
        }
        return true;
    }

    static bool handle_user_stack_growth(uint64_t fault_addr, uint64_t error_code) {
        // Only grow on non-present faults. Permission faults inside the stack
        // window should still be reported as bugs.
        constexpr uint64_t PF_PRESENT = 1ULL << 0;
        if (error_code & PF_PRESENT) return false;

        smp::cpu_local *cpu = smp::get_cpu();
        if (!cpu || !cpu->current_task) return false;

        scheduler::task *current = cpu->current_task;
        if (current->type != scheduler::task_type::USER || !current->stack_vma) return false;

        scheduler::vm_area *stack = current->stack_vma;
        if ((stack->flags & scheduler::VMA_GROWSDOWN) == 0) return false;

        uint64_t fault_page = fault_addr & ~(pmm::PAGE_SIZE - 1);
        uint64_t committed_base = stack->committed_start;
        if (fault_page < stack->start || fault_page >= committed_base) return false;

        for (uint64_t page = fault_page; page < committed_base; page += pmm::PAGE_SIZE) {
            uint64_t phys = pmm::alloc(pmm::PAGE_SIZE);
            if (!phys) return false;
            memset(p2v(phys), 0, pmm::PAGE_SIZE);
            map_page(page, phys, PageFlags::Write | PageFlags::User | PageFlags::NX,
                     PageSize::Size4K, current->cr3);
        }

        stack->committed_start = fault_page;
        return true;
    }

    bool handle_fault(uint64_t fault_addr, uint64_t error_code, regs *) {
        if (handle_user_stack_growth(fault_addr, error_code)) { return true; }

        // Error code bit 1: 0 = Read, 1 = Write
        bool is_write = error_code & (1 << 1);
        if (!is_write) return false;

        scheduler::task *current = smp::get_cpu()->current_task;
        uint64_t *pte = get_pte_ptr(fault_addr, current->cr3);

        if (pte && (*pte & static_cast<uint64_t>(PageFlags::CoW))) {
            uint64_t old_phys = *pte & get_phys_addr_mask();

            // If we are the only one holding a reference, just promote it to writable
            if (pmm::get_ref(old_phys) == 1) {
                *pte &= ~static_cast<uint64_t>(PageFlags::CoW);
                *pte |= static_cast<uint64_t>(PageFlags::Write);
            } else {
                // Someone else is using this page, we must duplicate it
                uint64_t new_phys = pmm::alloc(pmm::PAGE_SIZE);
                memcpy(p2v(new_phys), p2v(old_phys), pmm::PAGE_SIZE);

                pmm::unref_page(old_phys);

                // Update PTE: New physical address, remove CoW bit, add Write bit
                *pte = (new_phys & get_phys_addr_mask()) |
                       (*pte & 0xFFF & ~static_cast<uint64_t>(PageFlags::CoW)) |
                       static_cast<uint64_t>(PageFlags::Write);
            }

            flush_tlb(current->cr3, fault_addr & ~(pmm::PAGE_SIZE - 1), 1);
            return true;
        }

        return false;
    }

    static void destroy_table_level(uint64_t table_phys, int level) {
        uint64_t *table = (uint64_t *)p2v(table_phys & get_phys_addr_mask());

        for (int i = 0; i < 512; i++) {
            uint64_t entry = table[i];
            if (!(entry & static_cast<uint64_t>(PageFlags::Present))) { continue; }

            uint64_t child_phys = entry & get_phys_addr_mask();

            // If it's a huge page (1GB in PDPT or 2MB in PD), it's a leaf
            if (entry & static_cast<uint64_t>(PageFlags::Huge)) {
                if (entry & static_cast<uint64_t>(PageFlags::User)) pmm::unref_page(child_phys);
                continue;
            }

            if (level > 0) {
                destroy_table_level(child_phys, level - 1);
            } else if (entry & static_cast<uint64_t>(PageFlags::User)) {
                pmm::unref_page(child_phys);
            }
        }

        // After freeing all children, free the table itself
        pmm::free(table_phys, pmm::PAGE_SIZE);
    }

    void destroy_user_address_space(uint64_t pml4_phys) {
        uint64_t *pml4 = (uint64_t *)p2v(pml4_phys & get_phys_addr_mask());

        // ONLY iterate through the lower 256 entries (User Space)
        for (int i = 0; i < 256; i++) {
            uint64_t entry = pml4[i];
            if (entry & static_cast<uint64_t>(PageFlags::Present)) {
                destroy_table_level(entry & get_phys_addr_mask(), 2);  // Start at PDPT level
            }
        }

        // Finally, free the PML4 frame itself
        pmm::free(pml4_phys, pmm::PAGE_SIZE);
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
        // Standard page alignment (4 KiB)
        size = (size + 0xFFF) & ~0xFFFULL;

        uint64_t flags;
        m_lock.acquire(flags);

        Segment *curr = m_head;
        while (curr) {
            if (curr->is_free && curr->size >= size) {
                // Split the segment if it's larger than requested
                if (curr->size > size) {
                    Segment *new_seg = static_cast<Segment *>(heap::kmalloc(sizeof(Segment)));

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
        if (base < KSTACK_BASE + pmm::PAGE_SIZE) return false;
        if (base + usable_size < base) return false;
        uint64_t high_guard = base + usable_size;
        if (high_guard + pmm::PAGE_SIZE < high_guard) return false;
        if (high_guard + pmm::PAGE_SIZE > KSTACK_BASE + KSTACK_SIZE) return false;
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
        uint64_t map_size = (size + offset + pmm::PAGE_SIZE - 1) & ~(pmm::PAGE_SIZE - 1);
        if (map_size < size) return nullptr;

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
        uint64_t map_size = (size + offset + pmm::PAGE_SIZE - 1) & ~(pmm::PAGE_SIZE - 1);
        if (map_size < size) return;

        unmap_range(virt_page, map_size);
        mmio_allocator->free(virt_page);
    }
}  // namespace vmm