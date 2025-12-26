#include "acpi.h"
#include "console.h"
#include "file/elf.h"
#include "file/initramfs.h"
#include "file/vfs.h"
#include "gdt.h"
#include "idt.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "multiboot2.h"
#include "smp/apic.h"
#include "smp/scheduler.h"
#include "smp/smp.h"
#include "util.h"
#include "vga.h"

static void debug_dump_vfs(vfs::vfs_node* node, int depth) {
    while (node) {
        for (int i = 0; i < depth; i++) console::printf("  ");

        const char* prefix = "- ";
        if (node->type == vfs::VfsType::VFS_DIRECTORY)
            prefix = "D ";
        else if (node->type == vfs::VfsType::VFS_CHAR_DEVICE)
            prefix = "C ";

        console::printf("%s%s (%d bytes)\n", prefix, node->name, node->size);

        if (node->child) { debug_dump_vfs(node->child, depth + 1); }

        node = node->next;
    }
}

extern "C" void kmain(uint64_t mb_phys_addr) {
    {
        uint8_t* mb_info = (uint8_t*)(uintptr_t)mb_phys_addr;
        multiboot_tag_framebuffer* fb_tag = nullptr;

        for (uint8_t* tag = mb_info + 8; tag < mb_info + *(uint32_t*)mb_info;
             tag += ((*(uint32_t*)(tag + 4) + 7) & ~7)) {
            uint32_t type = *(uint32_t*)tag;

            if (type == 0) break;  // End tag

            if (type == 8) {  // Framebuffer tag
                fb_tag = (multiboot_tag_framebuffer*)tag;
                break;
            }
        }

        if (fb_tag) {
            console::init(console::Backend::FRAMEBUFFER, fb_tag);
            console::printf("[ OK ] Framebuffer initialized (%dx%d); Type=%d\n", fb_tag->width,
                            fb_tag->height, fb_tag->framebuffer_type);
        } else {
            console::init(console::Backend::VGA, nullptr);
            console::printf("[ OK ] VGA Text initialized.\n");
        }
    }

    gdt::init_early();
    console::printf("[ OK ] GDT initialized.\n");
    idt::init();
    console::printf("[ OK ] IDT initialized.\n");

    pmm::init(mb_phys_addr);

    uint64_t total_kb = (uint64_t)pmm::get_total_bytes() / 1024;
    uint64_t free_kb = (uint64_t)pmm::get_free_bytes() / 1024;
    uint64_t used_kb = total_kb - free_kb;

    console::printf("[ OK ] PMM initialized.\n");
    console::printf("       Memory: %d KiB / %d KiB used\n", (int)used_kb, (int)(total_kb));
    console::printf("       Free:   %d KiB\n", (int)free_kb);

    vmm::init();
    console::printf("[ OK ] VMM initialized.\n");

    vfs::init();
    console::printf("[ OK ] VFS initialized.\n");

    initramfs::init(mb_phys_addr);
    console::printf("[ OK ] Initramfs initialized.\n");

    console::printf("--- VFS Debug Tree ---\n");
    debug_dump_vfs(vfs::get_root(), 0);
    console::printf("----------------------\n");

    acpi::init(mb_phys_addr);
    console::printf("[ OK ] ACPI initialized.\n");

    apic::init();
    console::printf("[ OK ] APIC initialized.\n");

    smp::init_bsp();

    scheduler::init_core();

    __asm__ volatile("sti");

    smp::init_aps();
    console::printf("[ OK ] SMP and scheduler initialized with %d cores.\n", smp::get_core_count());

    auto info = elf::load("/bin/init");

    if (info.pml4 != 0) {
        scheduler::spawn(scheduler::task_type::USER, (void (*)())info.entry, info.pml4);
    }

    for (;;) { asm volatile("pause"); }
}