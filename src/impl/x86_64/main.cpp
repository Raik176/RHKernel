#include "acpi.h"
#include "boot_options.h"
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
#include "memory/heap.h"
#include "memory/vmm.h"
#include "multiboot2.h"
#include "security/random.h"
#include "security/stack_protector.h"
#include "mod/fs.h"
#include "smp/apic.h"
#include "smp/ioapic.h"
#include "smp/scheduler.h"
#include "smp/smp.h"
#include "smp/workqueue.h"
#include "string.h"
#include "symbol/ksym.h"
#include "util.h"
#include "vga.h"



static void fatal_boot(const char *msg) {
    KFATAL(msg ? msg : "boot failed");
}

static int mount_configured_root() {
    const boot_options::options &opts = boot_options::get();
    if (!opts.valid) fatal_boot(opts.error);
    if (opts.root == boot_options::root_kind::none) {
        console::printf("[ OK ] Live root: initramfs kept mounted at /\n");
        return 0;
    }

    const char *kind = opts.root == boot_options::root_kind::disk ? "disk" : "part";
    char source[80];
    snprintf(source, sizeof(source), "/dev/%s/by-uuid/%s", kind, opts.root_uuid);

    vfs::vfs_node *root_dev = vfs::open(source);
    if (!root_dev) {
        console::printf("[ OK ] Live root: %s root UUID %s not found\n", kind, opts.root_uuid);
        return 0;
    }

    const char *mode = strcmp(opts.rootmode, "rw") == 0 ? "rw" : "ro";
    int r = opts.rootfs[0] ? fs_mount(source, "/", opts.rootfs, mode) : fs_mount_auto(source, "/", mode);
    if (r != 0) {
        console::printf("[FAIL] Root mount failed: source=%s rootfs=%s mode=%s error=%d\n",
                        source, opts.rootfs[0] ? opts.rootfs : "auto", mode, r);
        return r;
    }

    console::printf("[ OK ] Root mounted: source=%s rootfs=%s mode=%s\n",
                    source, opts.rootfs[0] ? opts.rootfs : "auto", mode);
    return 0;
}

static vfs::vfs_node *ensure_dir(vfs::vfs_node *parent, const char *name) {
    if (!parent || !name) return nullptr;
    vfs::vfs_node *node = vfs::finddir(parent, name);
    if (node) return node->type == vfs::VfsType::VFS_DIRECTORY ? node : nullptr;
    return vfs::create_node(name, vfs::VfsType::VFS_DIRECTORY, parent);
}

static int ensure_dir_path(const char *path) {
    if (!path || path[0] != '/') return -1;
    if (strcmp(path, "/") == 0) return 0;

    uint64_t len = strlen(path);
    char *component = (char *)heap::kmalloc(len + 1);
    if (!component) return -1;

    vfs::vfs_node *dir = vfs::get_root();
    const char *p = path + 1;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != '/') p++;
        uint64_t n = (uint64_t)(p - start);
        if (n == 0 || (n == 1 && start[0] == '.') ||
            (n == 2 && start[0] == '.' && start[1] == '.')) {
            heap::kfree(component);
            return -1;
        }

        memcpy(component, start, n);
        component[n] = 0;
        dir = ensure_dir(dir, component);
        if (!dir) {
            heap::kfree(component);
            return -1;
        }
    }

    heap::kfree(component);
    return 0;
}

static bool automnt_field_ok(const char *s, bool path) {
    if (!s || !*s) return false;
    if (path && s[0] != '/') return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7f) return false;
    }
    return true;
}

static bool automnt_has_glob(const char *s) {
    return s && (strchr(s, '*') || strchr(s, '?'));
}

static bool automnt_match_component(const char *pat, const char *name) {
    const char *star_pat = nullptr;
    const char *star_name = nullptr;

    while (*name) {
        if (*pat == '?' || *pat == *name) {
            pat++;
            name++;
        } else if (*pat == '*') {
            star_pat = ++pat;
            star_name = name;
        } else if (star_pat) {
            pat = star_pat;
            name = ++star_name;
        } else {
            return false;
        }
    }

    while (*pat == '*') pat++;
    return *pat == 0;
}

static constexpr uint64_t AUTOMNT_RETRY_TICKS = 420;
static constexpr uint64_t AUTOMNT_RETRY_SLEEP_TICKS = 2;
static constexpr uint64_t AUTOMNT_PENDING_SLEEP_TICKS = 25;

static int automnt_pending_count;

static int automnt_mount_one(const char *source, const char *target, const char *fstype,
                             const char *flags, bool mkdir_target) {
    if (mkdir_target && ensure_dir_path(target) != 0) return -1;
    if (strcmp(fstype, "auto") == 0 || strcmp(fstype, "-") == 0)
        return fs_mount_auto(source, target, flags);
    return fs_mount(source, target, fstype, flags);
}

static int automnt_mount_glob(const char *source, const char *target, const char *fstype,
                              const char *flags, bool mkdir_target) {
    const char *slash = source;
    for (const char *p = source; *p; ++p) {
        if (*p == '/') slash = p;
    }
    for (const char *p = source; p < slash; ++p) {
        if (*p == '*' || *p == '?') return -1;
    }

    uint64_t dir_len = (uint64_t)(slash - source);
    const char *pat = slash + 1;
    if (!*pat) return -1;

    char *dir_path = (char *)heap::kmalloc(dir_len + 2);
    if (!dir_path) return -1;
    if (dir_len == 0) {
        dir_path[0] = '/';
        dir_path[1] = 0;
    } else {
        memcpy(dir_path, source, dir_len);
        dir_path[dir_len] = 0;
    }

    vfs::vfs_node *dir = vfs::open(dir_path);
    if (!dir || dir->type != vfs::VfsType::VFS_DIRECTORY) {
        heap::kfree(dir_path);
        return -2;
    }

    char *name = (char *)heap::kmalloc(128);
    if (!name) {
        heap::kfree(dir_path);
        return -1;
    }
    uint64_t name_cap = 128;
    int matched = 0;
    int mounted = -2;

    for (uint64_t i = 0;; ++i) {
        vfs::vfs_dirent ent;
        memset(&ent, 0, sizeof(ent));
        ent.name = name;
        ent.name_capacity = name_cap;
        int r = vfs::readdir(dir, i, &ent);
        if (r != 0) break;
        if (ent.name_len + 1 > name_cap) {
            char *new_name = (char *)heap::krealloc(name, ent.name_len + 1);
            if (!new_name) {
                mounted = -1;
                break;
            }
            name = new_name;
            name_cap = ent.name_len + 1;
            i--;
            continue;
        }
        if (!ent.name || !automnt_match_component(pat, ent.name)) continue;
        matched = 1;

        uint64_t src_len = strlen(dir_path) + 1 + ent.name_len;
        char *candidate = (char *)heap::kmalloc(src_len + 1);
        if (!candidate) {
            mounted = -1;
            break;
        }
        if (strcmp(dir_path, "/") == 0)
            snprintf(candidate, src_len + 1, "/%s", ent.name);
        else
            snprintf(candidate, src_len + 1, "%s/%s", dir_path, ent.name);

        if (automnt_mount_one(candidate, target, fstype, flags, mkdir_target) == 0) {
            mounted = 0;
            heap::kfree(candidate);
            break;
        }
        heap::kfree(candidate);
    }

    if (!matched) mounted = -2;
    heap::kfree(name);
    heap::kfree(dir_path);
    return mounted;
}

static int automnt_parse_options(const char *opts, bool *mkdir_target, bool *nofail) {
    *mkdir_target = false;
    *nofail = false;
    if (!opts || strcmp(opts, "-") == 0) return 0;

    const char *p = opts;
    while (*p) {
        const char *start = p;
        while (*p && *p != ',') p++;
        uint64_t n = (uint64_t)(p - start);
        if (n == 5 && strncmp(start, "mkdir", 5) == 0) {
            *mkdir_target = true;
        } else if (n == 6 && strncmp(start, "nofail", 6) == 0) {
            *nofail = true;
        } else {
            return -1;
        }
        if (*p == ',') {
            p++;
            if (!*p) return -1;
        }
    }
    return 0;
}

static int automnt_run_entry(char *line, uint64_t line_no) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;

    char *fields[6];
    int count = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        if (count == 6) return -1;
        fields[count++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
        if (*p) *p++ = 0;
    }

    if (count == 0) return 0;
    if (count != 4 && count != 5) {
        console::printf("automnt:%d: expected 4 or 5 fields\n", line_no);
        return -1;
    }

    const char *source = fields[0];
    const char *target = fields[1];
    const char *fstype = fields[2];
    const char *flags = fields[3];
    const char *opts = count == 5 ? fields[4] : "-";

    if (!automnt_field_ok(source, true) || !automnt_field_ok(target, true) ||
        !automnt_field_ok(fstype, false) || !automnt_field_ok(flags, false) ||
        !automnt_field_ok(opts, false)) {
        console::printf("automnt:%d: invalid field\n", line_no);
        return -1;
    }
    if (strcmp(flags, "ro") != 0 && strcmp(flags, "rw") != 0) {
        console::printf("automnt:%d: flags must be ro or rw\n", line_no);
        return -1;
    }

    bool mkdir_target = false;
    bool nofail = false;
    if (automnt_parse_options(opts, &mkdir_target, &nofail) != 0) {
        console::printf("automnt:%d: invalid options\n", line_no);
        return -1;
    }

    uint64_t start = apic::get_global_ticks();
    for (;;) {
        int r;
        if (automnt_has_glob(source)) {
            r = automnt_mount_glob(source, target, fstype, flags, mkdir_target);
        } else {
            vfs::vfs_node *node = vfs::open(source);
            r = node ? automnt_mount_one(source, target, fstype, flags, mkdir_target) : -2;
        }

        if (r == 0) return 0;

        uint64_t now = apic::get_global_ticks();
        if (r != -2 || now - start >= AUTOMNT_RETRY_TICKS) {
            if (nofail && r == -2) {
                automnt_pending_count++;
                return 0;
            }
            console::printf("automnt:%d: mount %s on %s failed: %d\n", line_no, source, target, r);
            return nofail ? 0 : -1;
        }

        scheduler::sleep(AUTOMNT_RETRY_SLEEP_TICKS);
    }
}

static int automnt_append(char **buf, uint64_t *len, uint64_t *cap, char c) {
    if (*len + 1 >= *cap) {
        uint64_t new_cap = *cap ? *cap * 2 : 128;
        char *new_buf = (char *)heap::krealloc(*buf, new_cap);
        if (!new_buf) return -1;
        *buf = new_buf;
        *cap = new_cap;
    }
    (*buf)[(*len)++] = c;
    (*buf)[*len] = 0;
    return 0;
}

static int run_automnt_file(const char *path) {
    vfs::vfs_node *file = vfs::open(path);
    if (!file) {
        console::printf("[ OK ] %s not found; no automounts\n", path);
        return 0;
    }
    if (file->type != vfs::VfsType::VFS_FILE) return -1;

    char *line = nullptr;
    uint64_t line_len = 0;
    uint64_t line_cap = 0;
    uint64_t line_no = 1;
    uint64_t off = 0;
    int rc = 0;
    uint8_t chunk[256];

    for (;;) {
        uint64_t n = vfs::read(file, off, sizeof(chunk), chunk);
        if (n == 0) break;
        off += n;
        for (uint64_t i = 0; i < n; ++i) {
            unsigned char c = chunk[i];
            if (c == '\n') {
                if (automnt_append(&line, &line_len, &line_cap, 0) != 0 ||
                    automnt_run_entry(line, line_no) != 0) {
                    rc = -1;
                    goto out;
                }
                line_len = 0;
                if (line) line[0] = 0;
                line_no++;
                continue;
            }
            if ((c < 0x20 && c != '\t' && c != '\r') || c == 0x7f) {
                console::printf("automnt:%d: invalid byte\n", line_no);
                rc = -1;
                goto out;
            }
            if (automnt_append(&line, &line_len, &line_cap, (char)c) != 0) {
                rc = -1;
                goto out;
            }
        }
        if (n < sizeof(chunk)) break;
    }

    if (line_len) {
        if (automnt_append(&line, &line_len, &line_cap, 0) != 0 ||
            automnt_run_entry(line, line_no) != 0)
            rc = -1;
    }

out:
    if (line) heap::kfree(line);
    return rc;
}

static void automnt_task_main() {
    for (;;) {
        automnt_pending_count = 0;
        int r = run_automnt_file("/etc/automnt");
        if (r != 0) fatal_boot("/etc/automnt failed");
        if (automnt_pending_count == 0) break;
        scheduler::sleep(AUTOMNT_PENDING_SLEEP_TICKS);
    }
    scheduler::exit(0);
}

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

    boot_options::init(mb_phys_addr);

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
    workqueue::init();

    random::init();
    console::printf("[ OK ] Random generator initialized.\n");

    module_loader::init();

    module_loader::load_module("/lib/modules/kbd_core.ko");
    module_loader::load_module("/lib/modules/usb_core.ko");
    module_loader::load_module("/lib/modules/usb_hid_kbd.ko");
    module_loader::load_module("/lib/modules/usb_hub.ko");
    module_loader::load_module("/lib/modules/usb_mass_storage.ko");
    module_loader::load_module("/lib/modules/ps2_kbd.ko");

    module_loader::load_module("/lib/modules/pci_bus.ko");
    module_loader::load_module("/lib/modules/pci_bridge.ko");
    module_loader::load_module("/lib/modules/ahci.ko");
    module_loader::load_module("/lib/modules/virtio_blk.ko");
    module_loader::load_module("/lib/modules/ramloop.ko");
    module_loader::load_module("/lib/modules/ext2.ko");
    module_loader::load_module("/lib/modules/vfat.ko");
    module_loader::load_module("/lib/modules/tmpfs.ko");
    module_loader::load_module("/lib/modules/iso9660.ko");
    module_loader::load_module("/lib/modules/minixfs.ko");

    if (mount_configured_root() != 0) fatal_boot("root mount failed");

    auto info = elf::load("/bin/init");
    if (info.pml4 == 0) fatal_boot("/bin/init load failed");

    scheduler::task *init_task = scheduler::spawn_init((void (*)())info.entry, info.pml4,
                                                       info.heap_start, &info);
    if (!init_task) fatal_boot("init task creation failed");

    if (!workqueue::start()) fatal_boot("workqueue start failed");

    __asm__ volatile("sti" ::: "memory");
    smp::init_aps();
    console::printf("[ OK ] SMP and scheduler initialized with %d cores.\n", smp::get_core_count());

    module_loader::load_module("/lib/modules/ehci.ko");
    module_loader::load_module("/lib/modules/uhci.ko");
    module_loader::load_module("/lib/modules/ohci.ko");

    scheduler::task *automnt_task = scheduler::spawn(scheduler::task_type::KERNEL, automnt_task_main);
    if (!automnt_task) fatal_boot("automnt task creation failed");

    if (!scheduler::unfreeze(init_task)) fatal_boot("init task unfreeze failed");

    for (;;) { asm volatile("hlt"); }
}