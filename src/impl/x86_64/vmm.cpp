#include "vmm.h"

#include "console.h"
#include "pmm.h"
#include "string.h"
#include "util.h"

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

namespace vmm {
    static constexpr uint64_t PHYS_ADDR_MASK = 0x000FFFFFFFFFF000;

    static uint64_t current_pml4_phys = 0;

    static inline uint64_t* get_table_ptr(uint64_t phys_addr) {
        return reinterpret_cast<uint64_t*>(p2v(phys_addr & PHYS_ADDR_MASK));
    }

    static uint64_t* get_next_table(uint64_t* v_table, uint64_t index, bool allocate) {
        uint64_t entry = v_table[index];

        if (entry & static_cast<uint64_t>(PageFlags::Present)) { return get_table_ptr(entry); }

        if (!allocate) return nullptr;

        uint64_t new_table_phys = pmm::alloc(pmm::PAGE_SIZE);
        if (!new_table_phys) return nullptr;

        memset(p2v(new_table_phys), 0, pmm::PAGE_SIZE);

        v_table[index] = (new_table_phys & PHYS_ADDR_MASK) |
                         static_cast<uint64_t>(PageFlags::Present | PageFlags::Write);

        return get_table_ptr(new_table_phys);
    }

    void init() {
        current_pml4_phys = pmm::alloc(pmm::PAGE_SIZE);
        memset(p2v(current_pml4_phys), 0, pmm::PAGE_SIZE);

        auto map_range = [&](uint8_t* start, uint8_t* end, PageFlags flags) {
            for (uint64_t v = (uint64_t)start; v < (uint64_t)end; v += pmm::PAGE_SIZE) {
                uint64_t p = v - KERNEL_VIRT_OFFSET;
                map_page(v, p, flags);
            }
        };

        map_range(_text_start, _text_end, PageFlags::None);
        map_range(_rodata_start, _rodata_end, PageFlags::NX);
        map_range(_data_start, _data_end, PageFlags::Write | PageFlags::NX);
        map_range(_bss_start, _bss_end, PageFlags::Write | PageFlags::NX);

        for (uint64_t p = 0; p < pmm::get_system_bytes(); p += 2 * 1024 * 1024) {  // 2MiB steps
            uint64_t v = p + PHYS_MAP_BASE;

            uint64_t pml4_idx = (v >> 39) & 0x1FF;
            uint64_t pdpt_idx = (v >> 30) & 0x1FF;
            uint64_t pd_idx = (v >> 21) & 0x1FF;

            uint64_t* pml4 = get_table_ptr(current_pml4_phys);
            uint64_t* pdpt = get_next_table(pml4, pml4_idx, true);
            uint64_t* pd = get_next_table(pdpt, pdpt_idx, true);

            pd[pd_idx] =
                (p & PHYS_ADDR_MASK) |
                static_cast<uint64_t>(PageFlags::Present | PageFlags::Write | PageFlags::Huge);
        }

        for (uint64_t p = 0; p < 0x100000; p += pmm::PAGE_SIZE) {
            map_page(p, p, PageFlags::Present | PageFlags::Write);
        }

        uint64_t new_cr3 = current_pml4_phys;
        asm volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
    }

    void map_page(uint64_t virt, uint64_t phys, PageFlags flags) {
        uint64_t pml4_idx = (virt >> 39) & 0x1FF;
        uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
        uint64_t pd_idx = (virt >> 21) & 0x1FF;
        uint64_t pt_idx = (virt >> 12) & 0x1FF;

        uint64_t* pml4 = get_table_ptr(current_pml4_phys);
        uint64_t* pdpt = get_next_table(pml4, pml4_idx, true);
        uint64_t* pd = get_next_table(pdpt, pdpt_idx, true);
        uint64_t* pt = get_next_table(pd, pd_idx, true);

        pt[pt_idx] = (phys & PHYS_ADDR_MASK) | static_cast<uint64_t>(flags | PageFlags::Present);
    }

    uint64_t get_kernel_pagemap() {
        return current_pml4_phys;
    }
}  // namespace vmm