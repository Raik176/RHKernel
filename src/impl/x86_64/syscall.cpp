#include "console.h"
#include "file/fd.h"
#include "file/vfs.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "mod/fs.h"
#include "security/random.h"
#include "smp/scheduler.h"
#include "smp/smp.h"
#include "string.h"
#include "util.h"

#define IA32_EFER 0xC0000080
#define IA32_STAR 0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_FMASK 0xC0000084

#define CLONE_VM 0x1
#define CLONE_FILES 0x2

enum SyscallNumbers {
    SYSCALL_WRITE = 0,
    SYSCALL_OPEN,
    SYSCALL_READ,
    SYSCALL_CLOSE,
    SYSCALL_YIELD,
    SYSCALL_SLEEP,
    SYSCALL_EXIT,
    SYSCALL_WAIT,
    SYSCALL_DUP2,
    SYSCALL_CLONE,
    SYSCALL_FORK,
    SYSCALL_EXEC,
    SYSCALL_GETPID,
    SYSCALL_MMAP,
    SYSCALL_MUNMAP,
    SYSCALL_BRK,
    SYSCALL_CREATE,
    SYSCALL_UNLINK,
    SYSCALL_RENAME,
    SYSCALL_READDIR,
    SYSCALL_CHDIR,
    SYSCALL_GETCWD,
    SYSCALL_FSCTL
};

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

static_assert(gdt::selectors::KDATA_SEL == gdt::selectors::KCODE_SEL + 8,
              "SYSCALL requires kernel SS selector to follow kernel CS");
static_assert(gdt::selectors::UDATA64_SEL == ((gdt::selectors::UCODE32_SEL & ~3) + 8) + 3,
              "SYSRET requires user data selector to match IA32_STAR user base + 8");
static_assert(gdt::selectors::UCODE64_SEL == ((gdt::selectors::UCODE32_SEL & ~3) + 16) + 3,
              "SYSRET requires user code selector to match IA32_STAR user base + 16");

static inline uint64_t page_align_down(uint64_t value) { return value & ~(pmm::PAGE_SIZE - 1); }
static inline uint64_t page_align_up(uint64_t value) {
    return (value + pmm::PAGE_SIZE - 1) & ~(pmm::PAGE_SIZE - 1);
}

extern "C" {
void syscall_entry();

static inline bool smap_enabled() {
    smp::cpu_local *cpu = smp::get_cpu();
    if (!cpu || !cpu->cpu_features.smap) return false;

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if ((cr4 & (1ULL << 21)) == 0) {
        cpu->cpu_features.smap = false;
        return false;
    }

    return true;
}

static inline void user_access_begin() {
    if (smap_enabled()) asm volatile("stac" ::: "cc");
}

static inline void user_access_end() {
    if (smap_enabled()) asm volatile("clac" ::: "cc");
}

static scheduler::task *current_user_task() {
    smp::cpu_local *cpu = smp::get_cpu();
    if (!cpu || !cpu->current_task || cpu->current_task->type != scheduler::task_type::USER) {
        return nullptr;
    }
    return cpu->current_task;
}

static bool copy_from_user(void *dst, const void *src, uint64_t size) {
    if (size == 0) return true;
    scheduler::task *task = current_user_task();
    if (!task || !dst || !src) return false;
    if (!vmm::user_range_mapped((uint64_t)src, size, false, task->cr3)) return false;

    user_access_begin();
    memcpy(dst, src, size);
    user_access_end();
    return true;
}

static bool copy_to_user(void *dst, const void *src, uint64_t size) {
    if (size == 0) return true;
    scheduler::task *task = current_user_task();
    if (!task || !dst || !src) return false;
    if (!vmm::user_range_mapped((uint64_t)dst, size, true, task->cr3)) return false;

    user_access_begin();
    memcpy(dst, src, size);
    user_access_end();
    return true;
}

static char *copy_string_from_user(const char *src, uint64_t max_len) {
    scheduler::task *task = current_user_task();
    if (!task || !src || max_len == 0) return nullptr;

    char *dst = (char *)heap::kmalloc(max_len);
    if (!dst) return nullptr;

    for (uint64_t i = 0; i < max_len; i++) {
        if ((i & (pmm::PAGE_SIZE - 1)) == 0 &&
            !vmm::user_range_mapped((uint64_t)src + i, 1, false, task->cr3)) {
            heap::kfree(dst);
            return nullptr;
        }

        user_access_begin();
        char c = src[i];
        user_access_end();
        dst[i] = c;
        if (c == 0) return dst;
    }

    heap::kfree(dst);
    return nullptr;
}

static char *copy_path_from_user(const char *src) {
    scheduler::task *task = current_user_task();
    if (!task || !src) return nullptr;

    uint64_t cap = 128;
    char *dst = (char *)heap::kmalloc(cap);
    if (!dst) return nullptr;

    for (uint64_t i = 0;; i++) {
        if (i == cap) {
            if (cap > (UINT64_MAX / 2)) {
                heap::kfree(dst);
                return nullptr;
            }
            uint64_t new_cap = cap * 2;
            char *next = (char *)heap::kmalloc(new_cap);
            if (!next) {
                heap::kfree(dst);
                return nullptr;
            }
            memcpy(next, dst, cap);
            heap::kfree(dst);
            dst = next;
            cap = new_cap;
        }

        if ((i & (pmm::PAGE_SIZE - 1)) == 0 &&
            !vmm::user_range_mapped((uint64_t)src + i, 1, false, task->cr3)) {
            heap::kfree(dst);
            return nullptr;
        }

        user_access_begin();
        char c = src[i];
        user_access_end();
        dst[i] = c;
        if (c == 0) return dst;
    }
}

static bool append_bytes(char **buf, uint64_t *len, uint64_t *cap, const char *src, uint64_t n) {
    if (!buf || !len || !cap || !src) return false;
    if (n > UINT64_MAX - *len - 1) return false;
    uint64_t need = *len + n + 1;
    if (need > *cap) {
        uint64_t new_cap = *cap ? *cap : 16;
        while (new_cap < need) {
            if (new_cap > UINT64_MAX / 2) return false;
            new_cap *= 2;
        }
        char *next = (char *)heap::kmalloc(new_cap);
        if (!next) return false;
        if (*buf && *len) memcpy(next, *buf, *len);
        if (*buf) heap::kfree(*buf);
        *buf = next;
        *cap = new_cap;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    (*buf)[*len] = 0;
    return true;
}

static bool push_path_component(char **buf, uint64_t *len, uint64_t *cap,
                                const char *component, uint64_t comp_len) {
    if (comp_len == 0 || (comp_len == 1 && component[0] == '.')) return true;
    if (comp_len == 2 && component[0] == '.' && component[1] == '.') {
        if (*len > 1) {
            while (*len > 1 && (*buf)[*len - 1] != '/') (*len)--;
            if (*len > 1) (*len)--;
            (*buf)[*len] = 0;
        }
        return true;
    }
    if (*len > 1 && !append_bytes(buf, len, cap, "/", 1)) return false;
    return append_bytes(buf, len, cap, component, comp_len);
}

static char *canonicalize_path(const char *cwd_path, const char *path) {
    if (!path || path[0] == 0) return nullptr;

    char *out = nullptr;
    uint64_t len = 0, cap = 0;
    if (!append_bytes(&out, &len, &cap, "/", 1)) return nullptr;

    auto scan = [&](const char *s) -> bool {
        const char *p = s;
        while (*p) {
            while (*p == '/') p++;
            const char *component = p;
            uint64_t comp_len = 0;
            while (p[comp_len] && p[comp_len] != '/') comp_len++;
            if (!push_path_component(&out, &len, &cap, component, comp_len)) return false;
            p += comp_len;
        }
        return true;
    };

    bool ok = path[0] == '/' ? scan(path) : (scan(cwd_path && cwd_path[0] ? cwd_path : "/") && scan(path));
    if (!ok) {
        heap::kfree(out);
        return nullptr;
    }
    return out;
}

static void free_kernel_argv(char **argv) {
    if (!argv) return;
    for (uint64_t i = 0; argv[i]; i++) heap::kfree(argv[i]);
    heap::kfree(argv);
}

static char **copy_argv_from_user(char **user_argv) {
    if (!user_argv) return nullptr;

    scheduler::task *task = current_user_task();
    if (!task) return nullptr;

    constexpr uint64_t MAX_ARGC = 64;
    constexpr uint64_t MAX_ARG_LEN = 4096;
    char **argv = (char **)heap::kmalloc(sizeof(char *) * (MAX_ARGC + 1));
    if (!argv) return nullptr;
    memset(argv, 0, sizeof(char *) * (MAX_ARGC + 1));

    for (uint64_t argc = 0; argc < MAX_ARGC; argc++) {
        char *arg_ptr = nullptr;
        if (!copy_from_user(&arg_ptr, &user_argv[argc], sizeof(arg_ptr))) {
            free_kernel_argv(argv);
            return nullptr;
        }
        if (!arg_ptr) return argv;

        argv[argc] = copy_string_from_user(arg_ptr, MAX_ARG_LEN);
        if (!argv[argc]) {
            free_kernel_argv(argv);
            return nullptr;
        }
    }

    char *terminator = nullptr;
    if (!copy_from_user(&terminator, &user_argv[MAX_ARGC], sizeof(terminator)) || terminator) {
        free_kernel_argv(argv);
        return nullptr;
    }

    return argv;
}

int sys_open(const char *path, int flags) {
    scheduler::task *current = current_user_task();
    if (!current) return -1;
    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    vfs::vfs_node *cwd = current->cwd ? current->cwd : vfs::get_root();
    vfs::vfs_node *node = vfs::open_at(cwd, kpath);
    if (!node && (flags & 0x40)) node = vfs::create_at(cwd, kpath, vfs::VfsType::VFS_FILE);
    if (node && (flags & 0x200) && vfs::truncate(node, 0) != 0) node = nullptr;

    heap::kfree(kpath);
    if (!node) return -1;

    vfs::open_file *file = (vfs::open_file *)heap::kmalloc(sizeof(vfs::open_file));
    if (!file) return -1;
    memset(file, 0, sizeof(*file));
    file->node = node;
    file->offset = 0;
    file->ref_count = 1;
    spinlock_init(&file->lock);

    int fd = fd_manager::alloc_fd(current);
    if (fd < 0) {
        heap::kfree(file);
        return -1;
    }
    current->fd_table[fd] = file;
    return fd;
}

int sys_read(int fd, void *buf, uint64_t size) {
    if (size == 0) return 0;
    if (size > 0x7fffffffULL) return -1;

    scheduler::task *current = current_user_task();
    if (!current || !buf || !vmm::user_range_mapped((uint64_t)buf, size, true, current->cr3)) {
        return -1;
    }

    vfs::open_file *file = fd_manager::get_file(fd);
    if (!file) return -1;

    constexpr uint64_t CHUNK = 4096;
    uint8_t *kbuf = (uint8_t *)heap::kmalloc(CHUNK);
    if (!kbuf) return -1;

    uint64_t lock_flags;
    spinlock_acquire(&file->lock, &lock_flags);

    uint64_t total = 0;
    while (total < size) {
        uint64_t want = size - total;
        if (want > CHUNK) want = CHUNK;

        uint64_t got = vfs::read(file->node, file->offset, want, kbuf);
        if (got == 0) break;
        if (!copy_to_user((uint8_t *)buf + total, kbuf, got)) {
            spinlock_release(&file->lock, lock_flags);
            heap::kfree(kbuf);
            return -1;
        }

        file->offset += got;
        total += got;
        if (got < want) break;
    }

    spinlock_release(&file->lock, lock_flags);
    heap::kfree(kbuf);
    return (int)total;
}

int sys_write(int fd, const void *buf, uint64_t size) {
    if (size == 0) return 0;
    if (size > 0x7fffffffULL) return -1;

    scheduler::task *current = current_user_task();
    if (!current || !buf || !vmm::user_range_mapped((uint64_t)buf, size, false, current->cr3)) {
        return -1;
    }

    vfs::open_file *file = fd_manager::get_file(fd);
    if (!file) return -1;

    constexpr uint64_t CHUNK = 4096;
    uint8_t *kbuf = (uint8_t *)heap::kmalloc(CHUNK);
    if (!kbuf) return -1;

    uint64_t lock_flags;
    spinlock_acquire(&file->lock, &lock_flags);

    uint64_t total = 0;
    while (total < size) {
        uint64_t want = size - total;
        if (want > CHUNK) want = CHUNK;

        if (!copy_from_user(kbuf, (const uint8_t *)buf + total, want)) {
            spinlock_release(&file->lock, lock_flags);
            heap::kfree(kbuf);
            return -1;
        }

        uint64_t written = vfs::write(file->node, file->offset, want, kbuf);
        if (written == 0) break;

        file->offset += written;
        total += written;
        if (written < want) break;
    }

    spinlock_release(&file->lock, lock_flags);
    heap::kfree(kbuf);
    return (int)total;
}

int sys_close(int fd) { return fd_manager::close_fd(fd, smp::get_cpu()->current_task); }

int sys_create(const char *path) {
    scheduler::task *current = current_user_task();
    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;
    vfs::vfs_node *node = vfs::create_at(current ? current->cwd : vfs::get_root(), kpath, vfs::VfsType::VFS_FILE);
    heap::kfree(kpath);
    return node ? 0 : -1;
}

int sys_unlink(const char *path) {
    scheduler::task *current = current_user_task();
    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;
    int ret = vfs::unlink_at(current ? current->cwd : vfs::get_root(), kpath);
    heap::kfree(kpath);
    return ret;
}

int sys_rename(const char *old_path, const char *new_path) {
    scheduler::task *current = current_user_task();
    char *kold = copy_path_from_user(old_path);
    if (!kold) return -1;
    char *knew = copy_path_from_user(new_path);
    if (!knew) {
        heap::kfree(kold);
        return -1;
    }

    int ret = vfs::rename_at(current ? current->cwd : vfs::get_root(), kold, knew);
    heap::kfree(kold);
    heap::kfree(knew);
    return ret;
}

int sys_readdir(const char *path, uint64_t index, void *user_out) {
    scheduler::task *current = current_user_task();
    if (!current || !user_out ||
        !vmm::user_range_mapped((uint64_t)user_out, sizeof(vfs::vfs_dirent), true, current->cr3)) {
        return -1;
    }

    vfs::vfs_dirent user_dent;
    if (!copy_from_user(&user_dent, user_out, sizeof(user_dent))) return -1;

    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    vfs::vfs_node *dir = vfs::open_at(current->cwd ? current->cwd : vfs::get_root(), kpath);
    heap::kfree(kpath);
    if (!dir || dir->type != vfs::VfsType::VFS_DIRECTORY) return -1;

    vfs::vfs_dirent dent;
    memset(&dent, 0, sizeof(dent));
    if (vfs::readdir(dir, index, &dent) != 0) return -1;

    void *user_name = user_dent.name;
    uint64_t user_cap = user_dent.name_capacity;
    if ((!user_name && user_cap) || (user_name && !user_cap)) return -1;
    dent.name = (char *)user_name;
    dent.name_capacity = user_cap;

    if (!user_name || user_cap <= dent.name_len) {
        return copy_to_user(user_out, &dent, sizeof(dent)) ? -2 : -1;
    }

    if (dent.name_len > (1ULL << 20)) return -1;
    char *kname = (char *)heap::kmalloc(dent.name_len + 1);
    if (!kname) return -1;

    vfs::vfs_dirent copy_dent;
    memset(&copy_dent, 0, sizeof(copy_dent));
    copy_dent.name = kname;
    copy_dent.name_capacity = dent.name_len + 1;
    int rc = vfs::readdir(dir, index, &copy_dent);
    if (rc != 0) {
        heap::kfree(kname);
        return -1;
    }

    dent.inode = copy_dent.inode;
    dent.type = copy_dent.type;
    dent.name_len = copy_dent.name_len;
    dent.name = (char *)user_name;
    dent.name_capacity = user_cap;

    if (copy_dent.name_capacity <= copy_dent.name_len || user_cap <= copy_dent.name_len) {
        heap::kfree(kname);
        return copy_to_user(user_out, &dent, sizeof(dent)) ? -2 : -1;
    }

    if (!vmm::user_range_mapped((uint64_t)user_name, copy_dent.name_len + 1, true, current->cr3)) {
        heap::kfree(kname);
        return -1;
    }

    bool ok = copy_to_user(user_name, kname, copy_dent.name_len + 1) &&
              copy_to_user(user_out, &dent, sizeof(dent));
    heap::kfree(kname);
    return ok ? 0 : -1;
}



struct sys_fsctl_args {
    const char *source;
    const char *target;
    const char *fstype;
    const char *flags;
};

static char *copy_optional_string(const char *src, uint64_t max_len) {
    if (!src) return nullptr;
    return copy_string_from_user(src, max_len);
}

static char *canonical_user_path(const char *user_path) {
    scheduler::task *current = current_user_task();
    char *kpath = copy_path_from_user(user_path);
    if (!kpath) return nullptr;
    char *canon = canonicalize_path(current && current->cwd_path ? current->cwd_path : "/", kpath);
    heap::kfree(kpath);
    return canon;
}

int sys_fsctl(int op, void *user_args) {
    scheduler::task *current = current_user_task();
    if (!current || !user_args) return -1;

    sys_fsctl_args args;
    if (!copy_from_user(&args, user_args, sizeof(args))) return -1;

    if (op == FS_CTL_MOUNT) {
        char *source = canonical_user_path(args.source);
        char *target = canonical_user_path(args.target);
        char *fstype = copy_optional_string(args.fstype, 64);
        char *flags = copy_optional_string(args.flags, 256);
        if (!source || !target || (args.fstype && !fstype) || (args.flags && !flags)) {
            if (source) heap::kfree(source);
            if (target) heap::kfree(target);
            if (fstype) heap::kfree(fstype);
            if (flags) heap::kfree(flags);
            return -1;
        }
        int ret = (fstype && fstype[0]) ? fs_mount(source, target, fstype, flags ? flags : "")
                                        : fs_mount_auto(source, target, flags ? flags : "");
        heap::kfree(source);
        heap::kfree(target);
        if (fstype) heap::kfree(fstype);
        if (flags) heap::kfree(flags);
        return ret;
    }

    if (op == FS_CTL_UNMOUNT) {
        char *target = canonical_user_path(args.target ? args.target : args.source);
        if (!target) return -1;
        int ret = fs_unmount(target);
        heap::kfree(target);
        return ret;
    }

    if (op == FS_CTL_PROBE) {
        char *source = canonical_user_path(args.source);
        char *fstype = copy_optional_string(args.fstype, 64);
        if (!source || (args.fstype && !fstype)) {
            if (source) heap::kfree(source);
            if (fstype) heap::kfree(fstype);
            return FS_PROBE_ERR;
        }
        int ret = fs_probe(source, fstype && fstype[0] ? fstype : nullptr);
        heap::kfree(source);
        if (fstype) heap::kfree(fstype);
        return ret;
    }

    return -1;
}

int sys_chdir(const char *path) {
    scheduler::task *current = current_user_task();
    if (!current) return -1;

    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    char *new_path = canonicalize_path(current->cwd_path, kpath);
    if (!new_path) {
        heap::kfree(kpath);
        return -1;
    }

    vfs::vfs_node *node = vfs::open_at(current->cwd ? current->cwd : vfs::get_root(), kpath);
    heap::kfree(kpath);
    if (!node || node->type != vfs::VfsType::VFS_DIRECTORY) {
        heap::kfree(new_path);
        return -1;
    }

    if (current->cwd_path) heap::kfree(current->cwd_path);
    current->cwd = node;
    current->cwd_path = new_path;
    return 0;
}

int sys_getcwd(char *buf, uint64_t size) {
    scheduler::task *current = current_user_task();
    if (!current || !buf || size == 0) return -1;
    const char *path = current->cwd_path ? current->cwd_path : "/";
    uint64_t len = strlen(path) + 1;
    if (len > size) return -1;
    return copy_to_user(buf, path, len) ? 0 : -1;
}

static bool range_overlaps_vma(scheduler::task *task, scheduler::vm_area *skip,
                              uint64_t start, uint64_t end) {
    for (scheduler::vm_area *vma = task->vma_list; vma; vma = vma->next) {
        if (vma == skip) continue;
        if (start < vma->end && end > vma->start) return true;
    }
    return false;
}

static uint64_t choose_mmap_addr(scheduler::task *task, uint64_t size) {
    constexpr uint64_t USER_TOP = 0x0000800000000000ULL;
    uint64_t base = page_align_up(task->mmap_hint ? task->mmap_hint : scheduler::USER_MMAP_BASE_MIN);
    uint64_t limit = scheduler::USER_MMAP_BASE_MIN + scheduler::USER_MMAP_ASLR_WINDOW;
    if (limit > USER_TOP) limit = USER_TOP;

    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t cur = pass == 0 ? base : scheduler::USER_MMAP_BASE_MIN;
        while (cur >= scheduler::USER_MMAP_BASE_MIN && cur <= limit && size <= limit - cur) {
            uint64_t end = cur + size;
            scheduler::vm_area *hit = nullptr;
            for (scheduler::vm_area *vma = task->vma_list; vma; vma = vma->next) {
                if (cur < vma->end && end > vma->start) {
                    hit = vma;
                    break;
                }
            }
            if (!hit) return cur;
            cur = page_align_up(hit->end);
        }
    }
    return 0;
}

static vmm::PageFlags prot_to_page_flags(int prot) {
    vmm::PageFlags page_flags = vmm::PageFlags::User;
    if (prot & PROT_WRITE) page_flags |= vmm::PageFlags::Write;
    if (!(prot & PROT_EXEC)) page_flags |= vmm::PageFlags::NX;
    return page_flags;
}

static uint32_t prot_to_vma_flags(int prot) {
    uint32_t vma_flags = 0;
    if (prot & PROT_READ) vma_flags |= scheduler::VMA_READ;
    if (prot & PROT_WRITE) vma_flags |= scheduler::VMA_WRITE;
    if (prot & PROT_EXEC) vma_flags |= scheduler::VMA_EXEC;
    return vma_flags;
}

void *sys_mmap(void *addr, uint64_t length, int prot, int flags) {
    scheduler::task *current = smp::get_cpu()->current_task;
    if (!current || current->type != scheduler::task_type::USER) return (void *)-1;
    if (length == 0) return (void *)-1;
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) return (void *)-1;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return (void *)-1;
    if ((flags & MAP_ANONYMOUS) == 0) return (void *)-1;

    uint64_t size = page_align_up(length);
    if (size < length) return (void *)-1;

    uint64_t virt = 0;
    if ((flags & MAP_FIXED) != 0) {
        virt = page_align_down((uint64_t)addr);
        if (virt == 0 || virt != (uint64_t)addr) return (void *)-1;
    } else if (addr != nullptr) {
        virt = page_align_up((uint64_t)addr);
    } else {
        virt = choose_mmap_addr(current, size);
        if (virt == 0) return (void *)-1;
    }

    constexpr uint64_t USER_TOP = 0x0000800000000000ULL;
    if (virt == 0 || virt >= USER_TOP || virt + size < virt || virt + size > USER_TOP) {
        return (void *)-1;
    }
    if (!scheduler::vma_range_free(current, virt, virt + size)) return (void *)-1;

    vmm::PageFlags page_flags = prot_to_page_flags(prot);
    for (uint64_t off = 0; off < size; off += pmm::PAGE_SIZE) {
        uint64_t phys = pmm::alloc(pmm::PAGE_SIZE);
        if (!phys) {
            for (uint64_t cleanup = 0; cleanup < off; cleanup += pmm::PAGE_SIZE) {
                uint64_t cleanup_phys = vmm::get_mapping(virt + cleanup, current->cr3);
                if (cleanup_phys) pmm::unref_page(cleanup_phys);
                vmm::unmap_page(virt + cleanup, current->cr3);
            }
            return (void *)-1;
        }
        memset(p2v(phys), 0, pmm::PAGE_SIZE);
        vmm::map_page(virt + off, phys, page_flags, vmm::PageSize::Size4K, current->cr3);
    }

    if (!scheduler::add_vma(current, virt, virt + size, virt, prot_to_vma_flags(prot),
                            scheduler::vma_type::ANON)) {
        for (uint64_t off = 0; off < size; off += pmm::PAGE_SIZE) {
            uint64_t phys = vmm::get_mapping(virt + off, current->cr3);
            if (phys) pmm::unref_page(phys);
            vmm::unmap_page(virt + off, current->cr3);
        }
        return (void *)-1;
    }

    if ((flags & MAP_FIXED) == 0) current->mmap_hint = virt + size;
    return (void *)virt;
}

int sys_munmap(void *addr, uint64_t length) {
    scheduler::task *current = smp::get_cpu()->current_task;
    if (!current || current->type != scheduler::task_type::USER) return -1;
    if (!addr || length == 0) return -1;

    uint64_t virt = page_align_down((uint64_t)addr);
    uint64_t size = page_align_up(((uint64_t)addr - virt) + length);
    constexpr uint64_t USER_TOP = 0x0000800000000000ULL;
    if (size < length || virt == 0 || virt >= USER_TOP || size > USER_TOP - virt) return -1;

    uint64_t end = virt + size;
    scheduler::vm_area *area = scheduler::find_vma(current, virt);
    if (!area || area->type != scheduler::vma_type::ANON || end > area->end) return -1;

    uint64_t old_area_end = area->end;
    bool split = virt > area->start && end < area->end;

    for (uint64_t off = 0; off < size; off += pmm::PAGE_SIZE) {
        uint64_t phys = vmm::get_mapping(virt + off, current->cr3);
        if (phys) pmm::unref_page(phys);
        vmm::unmap_page(virt + off, current->cr3);
    }

    if (virt == area->start && end == area->end) {
        scheduler::remove_vma(current, area->start, area->end);
    } else if (virt == area->start) {
        area->start = end;
        if (area->committed_start < area->start) area->committed_start = area->start;
    } else {
        area->end = virt;
        if (split && !scheduler::add_vma(current, end, old_area_end, end,
                                         area->flags, area->type)) {
            return -1;
        }
    }
    return 0;
}

uint64_t sys_brk(void *addr) {
    scheduler::task *current = smp::get_cpu()->current_task;
    if (!current || current->type != scheduler::task_type::USER || !current->heap_vma) return 0;

    uint64_t requested = (uint64_t)addr;
    scheduler::vm_area *heap_vma = current->heap_vma;
    uint64_t heap_start = heap_vma->start;
    if (requested == 0) return current->program_break;
    if (requested >= 0x0000800000000000ULL) return current->program_break;
    if (requested < heap_start) return current->program_break;

    if (current->stack_vma && requested > current->stack_vma->start) return current->program_break;

    uint64_t old_break = current->program_break;
    uint64_t old_mapped_end = page_align_up(old_break);
    uint64_t new_mapped_end = page_align_up(requested);

    if (new_mapped_end > old_mapped_end) {
        if (range_overlaps_vma(current, heap_vma, heap_vma->end, new_mapped_end)) {
            return old_break;
        }
        for (uint64_t page = old_mapped_end; page < new_mapped_end; page += pmm::PAGE_SIZE) {
            uint64_t phys = pmm::alloc(pmm::PAGE_SIZE);
            if (!phys) {
                for (uint64_t cleanup = old_mapped_end; cleanup < page; cleanup += pmm::PAGE_SIZE) {
                    uint64_t cleanup_phys = vmm::get_mapping(cleanup, current->cr3);
                    if (cleanup_phys) pmm::unref_page(cleanup_phys);
                    vmm::unmap_page(cleanup, current->cr3);
                }
                return old_break;
            }
            memset(p2v(phys), 0, pmm::PAGE_SIZE);
            vmm::map_page(page, phys, vmm::PageFlags::User | vmm::PageFlags::Write |
                                      vmm::PageFlags::NX,
                          vmm::PageSize::Size4K, current->cr3);
        }
        if (new_mapped_end > heap_vma->end) heap_vma->end = new_mapped_end;
    }

    if (new_mapped_end < old_mapped_end) {
        for (uint64_t page = new_mapped_end; page < old_mapped_end; page += pmm::PAGE_SIZE) {
            uint64_t phys = vmm::get_mapping(page, current->cr3);
            if (phys) pmm::unref_page(phys);
            vmm::unmap_page(page, current->cr3);
        }
        heap_vma->end = new_mapped_end > heap_start ? new_mapped_end : heap_start + pmm::PAGE_SIZE;
    }

    current->program_break = requested;
    return current->program_break;
}

int sys_dup2(int oldfd, int newfd) {
    auto *current = smp::get_cpu()->current_task;

    // Get the file object to be duplicated
    auto *file = fd_manager::get_file(oldfd, current);
    if (!file) return -1;
    if (newfd < 0 || newfd >= 512) return -1;
    if (oldfd == newfd) return newfd;

    if (!fd_manager::expand_table(newfd + 1, current)) return -1;

    // If newfd is already open, close it first
    if (current->fd_table[newfd] != nullptr) { fd_manager::close_fd(newfd, current); }

    uint64_t flags;
    spinlock_acquire(&file->lock, &flags);
    file->ref_count++;
    spinlock_release(&file->lock, flags);

    current->fd_table[newfd] = file;
    return newfd;
}

uint64_t syscall_handler(struct regs *r) {
    uint64_t syscall = r->rax;
    uint64_t arg1 = r->rdi;
    uint64_t arg2 = r->rsi;
    uint64_t arg3 = r->rdx;

    switch (syscall) {
        case SYSCALL_WRITE:
            return sys_write((int)arg1, (const void *)arg2, (uint64_t)arg3);
        case SYSCALL_OPEN:
            return sys_open((const char *)arg1, (int)arg2);
        case SYSCALL_READ:
            return sys_read((int)arg1, (void *)arg2, (uint64_t)arg3);
        case SYSCALL_CLOSE:
            return sys_close((int)arg1);
        case SYSCALL_DUP2:
            return sys_dup2((int)arg1, (int)arg2);
        case SYSCALL_YIELD:
            scheduler::yield();
            return 0;
        case SYSCALL_SLEEP:
            scheduler::sleep(arg1);
            return 0;
        case SYSCALL_EXIT:
            scheduler::exit((int)arg1);
            return 0;
        case SYSCALL_CLONE:
            return (uint64_t)scheduler::clone(arg1, (void *)arg2, r);
        case SYSCALL_FORK:
            return (uint64_t)scheduler::clone(0, nullptr, r);
        case SYSCALL_EXEC: {
            char *kpath = copy_path_from_user((const char *)arg1);
            if (!kpath) return (uint64_t)-1;

            char **kargv = copy_argv_from_user((char **)arg2);
            if (arg2 && !kargv) {
                heap::kfree(kpath);
                return (uint64_t)-1;
            }

            scheduler::task *current = current_user_task();
            char *exec_path = canonicalize_path(current ? current->cwd_path : "/", kpath);
            heap::kfree(kpath);
            if (!exec_path) {
                free_kernel_argv(kargv);
                return (uint64_t)-1;
            }

            uint64_t ret = (uint64_t)scheduler::exec(exec_path, kargv, r);
            heap::kfree(exec_path);
            free_kernel_argv(kargv);
            return ret;
        }
        case SYSCALL_WAIT: {
            int status = 0;
            int *status_ptr = (int *)arg1;
            if (status_ptr) {
                scheduler::task *current = current_user_task();
                if (!current || !vmm::user_range_mapped((uint64_t)status_ptr, sizeof(int), true,
                                                        current->cr3))
                    return (uint64_t)-1;
            }

            int ret = scheduler::wait(status_ptr ? &status : nullptr);
            if (ret >= 0 && status_ptr && !copy_to_user(status_ptr, &status, sizeof(status))) {
                return (uint64_t)-1;
            }
            return (uint64_t)ret;
        }
        case SYSCALL_GETPID:
            return smp::get_cpu()->current_task->id;
        case SYSCALL_MMAP:
            return (uint64_t)sys_mmap((void *)arg1, (uint64_t)arg2, (int)arg3, (int)r->r10);
        case SYSCALL_MUNMAP:
            return (uint64_t)sys_munmap((void *)arg1, (uint64_t)arg2);
        case SYSCALL_BRK:
            return sys_brk((void *)arg1);
        case SYSCALL_CREATE:
            return sys_create((const char *)arg1);
        case SYSCALL_UNLINK:
            return sys_unlink((const char *)arg1);
        case SYSCALL_RENAME:
            return sys_rename((const char *)arg1, (const char *)arg2);
        case SYSCALL_READDIR:
            return sys_readdir((const char *)arg1, (uint64_t)arg2, (void *)arg3);
        case SYSCALL_CHDIR:
            return sys_chdir((const char *)arg1);
        case SYSCALL_GETCWD:
            return sys_getcwd((char *)arg1, (uint64_t)arg2);
        case SYSCALL_FSCTL:
            return sys_fsctl((int)arg1, (void *)arg2);
        default:
            return -1;
    }
}

void enable_syscalls() {
    uint32_t efer_low, efer_high;
    asm volatile("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(IA32_EFER));
    efer_low |= 1;
    asm volatile("wrmsr" : : "a"(efer_low), "d"(efer_high), "c"(IA32_EFER));

    uint64_t addr = (uint64_t)syscall_entry;
    asm volatile("wrmsr" : : "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)), "c"(IA32_LSTAR));

    uint64_t kernel_base = gdt::selectors::KCODE_SEL;
    uint64_t user_base = (gdt::selectors::UCODE32_SEL & ~3);

    uint64_t star = (kernel_base << 32) | (user_base << 48);

    asm volatile("wrmsr" : : "a"(0), "d"((uint32_t)(star >> 32)), "c"(IA32_STAR));
    asm volatile("wrmsr" : : "a"(0x200), "d"(0), "c"(IA32_FMASK));
}
}