#include "acpi.h"
#include "console.h"
#include "file/device.h"
#include "file/elf.h"
#include "file/initramfs.h"
#include "file/module_loader.h"
#include "file/vfs.h"
#include "framebuffer.h"
#include "gdt.h"
#include "idt.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "multiboot2.h"
#include "security/random.h"
#include "security/stack_protector.h"
#include "mod/fs.h"
#include "smp/apic.h"
#include "smp/ioapic.h"
#include "smp/scheduler.h"
#include "smp/smp.h"
#include "symbol/ksym.h"
#include "util.h"
#include "vga.h"

extern "C" void kmain(uint64_t mb_phys_addr) __attribute__((used, no_stack_protector));

extern "C" void kmain(uint64_t mb_phys_addr) {
    stack_protector::init(mb_phys_addr);
    {
        uint8_t *mb_info = (uint8_t *)(uintptr_t)mb_phys_addr;
        multiboot_tag_framebuffer *fb_tag = nullptr;

        for (uint8_t *tag = mb_info + 8; tag < mb_info + *(uint32_t *)mb_info;
             tag += ((*(uint32_t *)(tag + 4) + 7) & ~7)) {
            uint32_t type = *(uint32_t *)tag;

            if (type == 0) break;  // End tag

            if (type == 8) {  // Framebuffer tag
                fb_tag = (multiboot_tag_framebuffer *)tag;
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
    console::printf("[ OK ] PMM bootstrap initialized.\n");

    vmm::init();
    framebuffer::remap_wc();
    console::printf("[ OK ] VMM initialized.\n");

    pmm::release_deferred_memory();

    uint64_t total_kb = (uint64_t)pmm::get_total_bytes() / 1024;
    uint64_t free_kb = (uint64_t)pmm::get_free_bytes() / 1024;
    uint64_t used_kb = total_kb - free_kb;

    console::printf("[ OK ] PMM high memory released.\n");
    console::printf("       Memory: %d KiB / %d KiB used\n", used_kb, total_kb);
    console::printf("       Free:   %d KiB\n", free_kb);

    vfs::init();
    init_virt_fs();
    console::printf("[ OK ] VFS initialized.\n");

    initramfs::init(mb_phys_addr);
    console::printf("[ OK ] Initramfs initialized.\n");

    acpi::init(mb_phys_addr);
    console::printf("[ OK ] ACPI initialized.\n");

    apic::init();
    console::printf("[ OK ] APIC initialized.\n");
    ioapic::init();
    console::printf("[ OK ] IOAPIC initialized.\n");

    smp::init_bsp();

    scheduler::init_core();

    random::init();
    console::printf("[ OK ] Random generator initialized.\n");

    module_loader::init();

    module_loader::load_module("/lib/modules/kbd_core.ko");
    module_loader::load_module("/lib/modules/ps2_kbd.ko");

    module_loader::load_module("/lib/modules/pci_bus.ko");
    module_loader::load_module("/lib/modules/pci_bridge.ko");
    module_loader::load_module("/lib/modules/ahci.ko");
    module_loader::load_module("/lib/modules/ext2.ko");
    module_loader::load_module("/lib/modules/vfat.ko");

    vfs::vfs_node *mnt = vfs::finddir(vfs::get_root(), "mnt");
    if (!mnt) mnt = vfs::create_node("mnt", vfs::VfsType::VFS_DIRECTORY, vfs::get_root());
    if (mnt && !vfs::finddir(mnt, "ext2")) vfs::create_node("ext2", vfs::VfsType::VFS_DIRECTORY, mnt);
    if (mnt && !vfs::finddir(mnt, "fat12")) vfs::create_node("fat12", vfs::VfsType::VFS_DIRECTORY, mnt);
    if (mnt && !vfs::finddir(mnt, "fat16")) vfs::create_node("fat16", vfs::VfsType::VFS_DIRECTORY, mnt);
    if (mnt && !vfs::finddir(mnt, "fat32")) vfs::create_node("fat32", vfs::VfsType::VFS_DIRECTORY, mnt);
    fs_mount("/dev/sdf", "/mnt/ext2", "ext2", "rw");
    fs_mount("/dev/sda", "/mnt/fat12", "vfat", "rw");
    fs_mount("/dev/sdb", "/mnt/fat16", "vfat", "rw");
    fs_mount("/dev/sdc", "/mnt/fat32", "vfat", "rw");

    __asm__ volatile("sti");
    smp::init_aps();
    console::printf("[ OK ] SMP and scheduler initialized with %d cores.\n", smp::get_core_count());

    auto info = elf::load("/bin/init");
    if (info.pml4 != 0) {
        scheduler::spawn(scheduler::task_type::USER, (void (*)())info.entry, info.pml4,
                         info.heap_start, &info);
    }

    for (;;) { asm volatile("hlt"); }
}