#include "util.h"
#include "vga.h"
#include "console.h"
#include "pmm.h"
#include "vmm.h"
#include "idt.h"
#include "gdt.h"
#include "heap.h"
#include "multiboot2.h"

extern "C" void kmain(uint64_t mb_phys_addr) {
    {
        uint8_t* mb_info = (uint8_t*)(uintptr_t)mb_phys_addr;
        multiboot_tag_framebuffer* fb_tag = nullptr;

        for (uint8_t* tag = mb_info + 8; 
            tag < mb_info + *(uint32_t*)mb_info; 
            tag += ((*(uint32_t*)(tag + 4) + 7) & ~7)) {

            uint32_t type = *(uint32_t*)tag;

            if (type == 0) break; // End tag

            if (type == 8) { // Framebuffer tag
                fb_tag = (multiboot_tag_framebuffer*)tag;
                break;
            }
        }

        if (fb_tag) {
            console::init(console::Backend::FRAMEBUFFER, fb_tag);
            console::printf("[ OK ] Framebuffer initialized (%dx%d); Type=%d\n", fb_tag->width, fb_tag->height, fb_tag->framebuffer_type);
        } else {
            console::init(console::Backend::VGA, nullptr);
            console::printf("[ OK ] VGA Text initialized.\n");
        }
    }

    gdt::init();
    console::printf("[ OK ] GDT initialized.\n");
    idt::init();
    console::printf("[ OK ] IDT initialized.\n");

    pmm::init(mb_phys_addr);

    uint64_t total_kb = (uint64_t)pmm::get_total_bytes() / 1024;
    uint64_t free_kb  = (uint64_t)pmm::get_free_bytes() / 1024;
    uint64_t used_kb  = total_kb - free_kb;

    console::printf("[ OK ] PMM initialized.\n");
    console::printf("       Memory: %d KiB / %d KiB used\n", 
                    (int)used_kb, 
                    (int)(total_kb));
    console::printf("       Free:   %d KiB\n", (int)free_kb);

    vmm::init();
    console::printf("[ OK ] VMM initialized.\n");

    heap::init();
    console::printf("[ OK ] Heap initialized.\n");

    for(;;);
}