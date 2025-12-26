#include "memory/vmm.h"

#include "console.h"
#include "memory/pmm.h"
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
    static bool supports_2mb_pages = false;
    static bool supports_1gb_pages = false;
    static uint64_t phys_addr_mask = 0;

    static uint64_t current_pml4_phys = 0;

    static inline uint64_t* get_table_ptr(uint64_t phys_addr) {
        return reinterpret_cast<uint64_t*>(p2v(phys_addr & get_phys_addr_mask()));
    }

    static uint64_t* get_next_table(uint64_t* v_table, uint64_t index, bool allocate) {
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
                  (uint64_t)_text_end - (uint64_t)_text_start, PageFlags::None);
        map_range((uint64_t)_rodata_start, (uint64_t)_rodata_start - KERNEL_VIRT_OFFSET,
                  (uint64_t)_rodata_end - (uint64_t)_rodata_start, PageFlags::NX);
        map_range((uint64_t)_data_start, (uint64_t)_data_start - KERNEL_VIRT_OFFSET,
                  (uint64_t)_data_end - (uint64_t)_data_start, PageFlags::Write | PageFlags::NX);
        map_range((uint64_t)_bss_start, (uint64_t)_bss_start - KERNEL_VIRT_OFFSET,
                  (uint64_t)_bss_end - (uint64_t)_bss_start, PageFlags::Write | PageFlags::NX);

        map_range(PHYS_MAP_BASE, 0, pmm::get_system_bytes(), PageFlags::Write);
        map_range(0, 0, 0x100000, PageFlags::Write);

        asm volatile("mov %0, %%cr3" : : "r"(current_pml4_phys) : "memory");
    }

    void map_range(uint64_t virt, uint64_t phys, uint64_t size, PageFlags flags, uint64_t pml4) {
        const uint64_t GIB = 1024ULL * 1024 * 1024;
        const uint64_t MIB = 2ULL * 1024 * 1024;
        const uint64_t KIB = 4096ULL;

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

        uint64_t* pml4 = get_table_ptr(pagemap);
        uint64_t* pdpt = get_next_table(pml4, pml4_idx, true);

        if (size == PageSize::Size1G) {
            if (!supports_1gb_pages) kpanic("VMM: 1GB pages unsupported");
            pdpt[pdpt_idx] = (phys & get_phys_addr_mask()) |
                             static_cast<uint64_t>(flags | PageFlags::Present | PageFlags::Huge);
            goto flush;
        }

        {
            uint64_t* pd = get_next_table(pdpt, pdpt_idx, true);
            if (size == PageSize::Size2M) {
                if (!supports_2mb_pages) kpanic("VMM: 2MB pages unsupported");
                pd[pd_idx] = (phys & get_phys_addr_mask()) |
                             static_cast<uint64_t>(flags | PageFlags::Present | PageFlags::Huge);
                goto flush;
            }

            uint64_t* pt = get_next_table(pd, pd_idx, true);
            pt[pt_idx] =
                (phys & get_phys_addr_mask()) | static_cast<uint64_t>(flags | PageFlags::Present);
        }

    flush:
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }

    void unmap_page(uint64_t virt, uint64_t pagemap) {
        if (virt % 4096 != 0) { kpanic("VMM: unmap_page called with unaligned virtual address."); }

        uint64_t pml4_idx = (virt >> 39) & 0x1FF;
        uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
        uint64_t pd_idx = (virt >> 21) & 0x1FF;
        uint64_t pt_idx = (virt >> 12) & 0x1FF;

        uint64_t* pml4 = get_table_ptr(pagemap);
        uint64_t* pdpt = get_next_table(pml4, pml4_idx, false);
        if (!pdpt) return;

        if (pdpt[pdpt_idx] & static_cast<uint64_t>(PageFlags::Huge)) {
            if (virt % (1024ULL * 1024 * 1024) != 0) {
                kpanic("VMM: Attempted to unmap part of a 1GB huge page.");
            }
            pdpt[pdpt_idx] = 0;
            goto flush;
        }

        {
            uint64_t* pd = get_next_table(pdpt, pdpt_idx, false);
            if (!pd) return;

            if (pd[pd_idx] & static_cast<uint64_t>(PageFlags::Huge)) {
                if (virt % (2 * 1024 * 1024) != 0) {
                    kpanic("VMM: Attempted to unmap part of a 2MB huge page.");
                }
                pd[pd_idx] = 0;
                goto flush;
            }

            {
                uint64_t* pt = get_next_table(pd, pd_idx, false);
                if (!pt) return;
                pt[pt_idx] = 0;
            }
        }

    flush:
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }

    uint64_t create_user_address_space() {
        uint64_t pml4_phys = pmm::alloc(pmm::PAGE_SIZE);
        uint64_t* new_pml4 = (uint64_t*)p2v(pml4_phys);
        uint64_t* kernel_pml4 = (uint64_t*)p2v(current_pml4_phys);

        memset(new_pml4, 0, pmm::PAGE_SIZE);

        for (int i = 256; i < 512; i++) { new_pml4[i] = kernel_pml4[i]; }

        return pml4_phys;
    }

    uint64_t get_kernel_pagemap() { return current_pml4_phys; }
}  // namespace vmm