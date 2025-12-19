#include "multiboot2.h"
#include "util.h"
#include "vga.h"
#include "console.h"

#define PT_PRESENT         (1ULL << 0)
#define PT_WRITABLE        (1ULL << 1)

extern uint8_t _kernel_phys_start[];
extern uint8_t _kernel_phys_end[];

uint64_t next_free_page;

void map_page(uint64_t* pml4_virt, uint64_t virt, uint64_t phys) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t* pml4 = pml4_virt;

    // PML4 -> PDPT
    if (!(pml4[pml4_idx] & PT_PRESENT)) {
        pml4[pml4_idx] = next_free_page | PT_PRESENT | PT_WRITABLE;
        next_free_page += PAGE_SIZE;
    }
    
    uint64_t* pdpt = (uint64_t*)p2v(pml4[pml4_idx] & ~0xFFF);
    // PDPT -> PD
    if (!(pdpt[pdpt_idx] & PT_PRESENT)) {
        pdpt[pdpt_idx] = next_free_page | PT_PRESENT | PT_WRITABLE;
        next_free_page += PAGE_SIZE;
    }

    uint64_t* pd = (uint64_t*)p2v(pdpt[pdpt_idx] & ~0xFFF);
    // PD -> PT
    if (!(pd[pd_idx] & PT_PRESENT)) {
        pd[pd_idx] = next_free_page | PT_PRESENT | PT_WRITABLE;
        next_free_page += PAGE_SIZE;
    }

    uint64_t* pt = (uint64_t*)p2v(pd[pd_idx] & ~0xFFF);
    pt[pt_idx] = phys | PT_PRESENT | PT_WRITABLE;
}

extern "C" void kmain(uint64_t mb_phys_addr) {
    struct multiboot_tag_mmap *mmap_tag = 0;
    struct multiboot_tag *tag;

    uint8_t* mb_ptr = (uint8_t*)p2v(mb_phys_addr);
    
    for (tag = (struct multiboot_tag *)(mb_ptr + 8);
         tag->type != 0;
         tag = (struct multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7))) {
        if (tag->type == 6) {
            mmap_tag = (struct multiboot_tag_mmap *)tag;
            break;
        }
    }

    next_free_page = (uint64_t)_kernel_phys_end;

    uint64_t pml4_phys = next_free_page;
    next_free_page += PAGE_SIZE;
    uint64_t* pml4_virt = (uint64_t*)p2v(pml4_phys);

    for(int i = 0; i < 512; i++) pml4_virt[i] = 0;

    uint32_t entry_count = (mmap_tag->size - sizeof(*mmap_tag)) / mmap_tag->entry_size;
    for (uint32_t i = 0; i < entry_count; i++) {
        if (mmap_tag->entries[i].type == 1) {
            uint64_t addr = mmap_tag->entries[i].addr;
            uint64_t length = mmap_tag->entries[i].len;

            for (uint64_t offset = 0; offset < length; offset += PAGE_SIZE) {
                uint64_t phys = addr + offset;
                map_page(pml4_virt, KERNEL_VIRT_OFFSET + phys, phys);
            }
        }
    }

    map_page(pml4_virt, VGA_VIRT, 0xB8000);

    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys));

    console::init(console::Backend::VGA);
    console::write("[ OK ] VGA Text initialized");
    
    for(;;);
}