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
}

// TODO: implement TLB shootdown
namespace vmm {
    static_assert(pmm::PAGE_SIZE == 4096, "VMM assumes 4 KiB base pages");
    static_assert((pmm::PAGE_SIZE / sizeof(uint64_t)) == 512,
                  "Page tables must contain exactly 512 entries");
    static_assert(sizeof(uint64_t) * 8 >= 52,
                  "Physical address calculations assume at least 52-bit addresses");
    static_assert(512 == (1 << 9), "Page table indexing assumes 9-bit levels");

    static bool supports_2mb_pages = false;
    static bool supports_1gb_pages = false;
    static uint64_t phys_addr_mask = 0;

    static uint64_t current_pml4_phys = 0;

    static VirtualRangeAllocator *mmio_allocator = nullptr;

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

    // assumes heap is ready after end of vmm init, which it really should be.
    void init() {
        uint32_t eax, ebx, ecx, edx;

        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x01));
        supports_2mb_pages = edx & (1 << 3);

        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
        supports_1gb_pages = edx & (1 << 26);

        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000008));
        phys_addr_mask = ((1ULL << (eax & 0xFF)) - 1) & ~0xFFFULL;

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
        map_range((uint64_t)_bss_start, (uint64_t)_bss_start - KERNEL_VIRT_OFFSET,
                  (uint64_t)_bss_end - (uint64_t)_bss_start,
                  PageFlags::Write | PageFlags::NX | PageFlags::Global);

        map_range(PHYS_MAP_BASE, 0, pmm::get_system_bytes(), PageFlags::Write);
        map_range(0, 0, 0x100000, PageFlags::Write);

        asm volatile("mov %0, %%cr3" : : "r"(current_pml4_phys) : "memory");

        mmio_allocator = new VirtualRangeAllocator(MMIO_BASE, MMIO_SIZE);
    }

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
                map_page(curr_v, curr_p, flags, PageSize::Size1G, pml4);
                mapped += GIB;
            } else if (supports_2mb_pages && remaining >= MIB && (curr_v % MIB == 0) &&
                       (curr_p % MIB == 0)) {
                map_page(curr_v, curr_p, flags, PageSize::Size2M, pml4);
                mapped += MIB;
            } else {
                map_page(curr_v, curr_p, flags, PageSize::Size4K, pml4);
                mapped += KIB;
            }
        }
    }

    void map_page(uint64_t virt, uint64_t phys, PageFlags flags, PageSize size, uint64_t pagemap) {
        uint64_t pml4_idx = (virt >> 39) & 0x1FF;
        uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
        uint64_t pd_idx = (virt >> 21) & 0x1FF;
        uint64_t pt_idx = (virt >> 12) & 0x1FF;

        uint64_t *pml4 = get_table_ptr(pagemap);
        uint64_t *pdpt = get_next_table(pml4, pml4_idx, true);

        if (size == PageSize::Size1G) {
            if (!supports_1gb_pages) kpanic("VMM: 1GB pages unsupported");
            pdpt[pdpt_idx] = (phys & get_phys_addr_mask()) |
                             static_cast<uint64_t>(flags | PageFlags::Present | PageFlags::Huge);
            goto flush;
        }

        {
            uint64_t *pd = get_next_table(pdpt, pdpt_idx, true);
            if (size == PageSize::Size2M) {
                if (!supports_2mb_pages) kpanic("VMM: 2MB pages unsupported");
                pd[pd_idx] = (phys & get_phys_addr_mask()) |
                             static_cast<uint64_t>(flags | PageFlags::Present | PageFlags::Huge);
                goto flush;
            }

            uint64_t *pt = get_next_table(pd, pd_idx, true);
            pt[pt_idx] =
                (phys & get_phys_addr_mask()) | static_cast<uint64_t>(flags | PageFlags::Present);
        }

    flush:
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }

    void unmap_page(uint64_t virt, uint64_t pagemap) {
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
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }

    void unmap_range(uint64_t virt, uint64_t size, uint64_t pagemap) {
        uint64_t start = virt & ~0xFFFULL;
        uint64_t end = (virt + size + 4095) & ~0xFFFULL;

        for (uintptr_t curr = start; curr < end; curr += pmm::PAGE_SIZE) {
            unmap_page(curr, pagemap);
        }
    }

    uint64_t create_user_address_space() {
        uint64_t pml4_phys = pmm::alloc(pmm::PAGE_SIZE);
        uint64_t *new_pml4 = (uint64_t *)p2v(pml4_phys);
        uint64_t *kernel_pml4 = (uint64_t *)p2v(current_pml4_phys);

        memset(new_pml4, 0, pmm::PAGE_SIZE);

        for (int i = 256; i < 512; i++) { new_pml4[i] = kernel_pml4[i]; }

        return pml4_phys;
    }

    uint64_t clone_address_space(uint64_t old_pml4_phys) {
        uint64_t new_pml4_phys = create_user_address_space();
        uint64_t *old_pml4 = (uint64_t *)p2v(old_pml4_phys);
        uint64_t *new_pml4 = (uint64_t *)p2v(new_pml4_phys);

        // Only clone the user half (0-255)
        for (int i = 0; i < 256; i++) {
            if (old_pml4[i] & (uint64_t)PageFlags::Present) {
                uint64_t *old_pdpt = (uint64_t *)p2v(old_pml4[i] & get_phys_addr_mask());
                uint64_t new_pdpt_phys = pmm::alloc(pmm::PAGE_SIZE);
                uint64_t *new_pdpt = (uint64_t *)p2v(new_pdpt_phys);
                memset(new_pdpt, 0, pmm::PAGE_SIZE);
                new_pml4[i] = new_pdpt_phys | (old_pml4[i] & 0xFFF);

                for (int j = 0; j < 512; j++) {
                    if (old_pdpt[j] & (uint64_t)PageFlags::Present) {
                        uint64_t *old_pd = (uint64_t *)p2v(old_pdpt[j] & get_phys_addr_mask());
                        uint64_t new_pd_phys = pmm::alloc(pmm::PAGE_SIZE);
                        uint64_t *new_pd = (uint64_t *)p2v(new_pd_phys);
                        memset(new_pd, 0, pmm::PAGE_SIZE);
                        new_pdpt[j] = new_pd_phys | (old_pdpt[j] & 0xFFF);

                        for (int k = 0; k < 512; k++) {
                            if (old_pd[k] & (uint64_t)PageFlags::Present) {
                                uint64_t *old_pt =
                                    (uint64_t *)p2v(old_pd[k] & get_phys_addr_mask());
                                uint64_t new_pt_phys = pmm::alloc(pmm::PAGE_SIZE);
                                uint64_t *new_pt = (uint64_t *)p2v(new_pt_phys);
                                memset(new_pt, 0, pmm::PAGE_SIZE);
                                new_pd[k] = new_pt_phys | (old_pd[k] & 0xFFF);

                                for (int l = 0; l < 512; l++) {
                                    if (old_pt[l] & (uint64_t)PageFlags::Present) {
                                        // CoW: If page is Write, remove Write, leave User/Present
                                        if (old_pt[l] & (uint64_t)PageFlags::Write) {
                                            old_pt[l] &= ~(uint64_t)PageFlags::Write;
                                            // We use bit 9 (Available) as our "Was Writable" flag
                                            old_pt[l] |= (uint64_t)PageFlags::CoW;
                                        }
                                        new_pt[l] = old_pt[l];
                                        pmm::ref_page(new_pt[l] & get_phys_addr_mask());
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
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

    bool handle_fault(uint64_t fault_addr, uint64_t error_code) {
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

            // Flush TLB for this address
            asm volatile("invlpg (%0)" : : "r"(fault_addr) : "memory");
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
                pmm::unref_page(child_phys);
                continue;
            }

            if (level > 0) {
                // Not a leaf yet (PML4=3, PDPT=2, PD=1), go deeper
                destroy_table_level(child_phys, level - 1);
            } else {
                // We are at the PT level (Level 0), the entry is a 4KB page
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
        // Initial segment representing the entire range
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

    void *mmio_map(uint64_t phys_addr, uint64_t size) {
        uint64_t virt = mmio_allocator->allocate(size);
        if (!virt) {
            console::printf("[ VMM ] Ran out of space to map mmio!");
            return nullptr;
        }

        map_range(virt, phys_addr, size,
                  PageFlags::NoCache | PageFlags::WriteThrough | PageFlags::Write | PageFlags::NX);

        return (void *)virt;
    }

    void mmio_unmap(void *virt_addr, uint64_t size) {
        uint64_t virt = reinterpret_cast<uint64_t>(virt_addr);

        unmap_range(virt, size);
        mmio_allocator->free(virt);
    }
}  // namespace vmm