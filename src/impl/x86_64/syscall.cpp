#include "console.h"
#include "file/fd.h"
#include "file/device.h"
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
#define IA32_SYSENTER_CS 0x174
#define IA32_SYSENTER_ESP 0x175
#define IA32_SYSENTER_EIP 0x176

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
    SYSCALL_FSCTL,
    SYSCALL_PKEY_MPROTECT,
    SYSCALL_SEEK,
    SYSCALL_STAT,
    SYSCALL_FSTAT,
    SYSCALL_SET_FS_BASE,
    SYSCALL_GET_FS_BASE,
    SYSCALL_SET_GS_BASE,
    SYSCALL_GET_GS_BASE
};

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

#define SEEK_SET_K 0
#define SEEK_CUR_K 1
#define SEEK_END_K 2

#define OPEN_CREAT 0x40
#define OPEN_TRUNC 0x200
#define OPEN_ALLOWED_FLAGS (OPEN_CREAT | OPEN_TRUNC)

#define STAT_IFMT 0170000
#define STAT_IFIFO 0010000
#define STAT_IFCHR 0020000
#define STAT_IFDIR 0040000
#define STAT_IFBLK 0060000
#define STAT_IFREG 0100000
#define STAT_DEFAULT_FILE_MODE 0644
#define STAT_DEFAULT_DIR_MODE 0755
#define STAT_DEFAULT_DEV_MODE 0666
#define STAT_BLOCK_SIZE 1024

struct user_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct user_stat64 {
    int16_t st_dev;
    uint16_t st_ino;
    uint32_t st_mode;
    uint16_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint16_t st_rdev;
    int64_t st_size;
    user_timespec64 st_atim;
    user_timespec64 st_mtim;
    user_timespec64 st_ctim;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_spare4[2];
};

static_assert(sizeof(user_stat64) == 104, "newlib struct stat ABI changed");
static_assert(__builtin_offsetof(user_stat64, st_size) == 16, "newlib stat st_size offset changed");
static_assert(__builtin_offsetof(user_stat64, st_blksize) == 72, "newlib stat st_blksize offset changed");


static bool sysenter_available = false;

static inline uint32_t read_pkru() {
    uint32_t eax = 0, edx = 0;
    asm volatile("rdpkru" : "=a"(eax), "=d"(edx) : "c"(0));
    return eax;
}

static inline void write_pkru(uint32_t value) {
    asm volatile("wrpkru" : : "a"(value), "c"(0), "d"(0) : "memory");
}

static_assert(gdt::selectors::KDATA_SEL == gdt::selectors::KCODE_SEL + 8,
              "SYSCALL requires kernel SS selector to follow kernel CS");
static_assert(gdt::selectors::UDATA64_SEL == gdt::selectors::SYSRET_USER_BASE + 8 + 3,
              "SYSRET requires user data selector to match IA32_STAR user base + 8");
static_assert(gdt::selectors::UCODE64_SEL == gdt::selectors::SYSRET_USER_BASE + 16 + 3,
              "SYSRET requires user code selector to match IA32_STAR user base + 16");
static_assert(gdt::selectors::UCODE32_SEL == gdt::selectors::KCODE_SEL + 16 + 3,
              "SYSEXIT32 requires user code selector to be SYSENTER_CS + 16");
static_assert(gdt::selectors::UDATA32_SEL == gdt::selectors::KCODE_SEL + 24 + 3,
              "SYSEXIT32 requires user data selector to be SYSENTER_CS + 24");

static inline uint64_t page_align_down(uint64_t value) { return value & ~(pmm::PAGE_SIZE - 1); }
static inline bool page_align_up_checked(uint64_t value, uint64_t *out) {
    if (!out || value > UINT64_MAX - (pmm::PAGE_SIZE - 1)) return false;
    *out = (value + pmm::PAGE_SIZE - 1) & ~(pmm::PAGE_SIZE - 1);
    return true;
}

static inline uint64_t page_align_up(uint64_t value) {
    uint64_t out = 0;
    return page_align_up_checked(value, &out) ? out : 0;
}

static inline void cpuid_leaf(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
                              uint32_t *ecx, uint32_t *edx) {
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "a"(leaf), "c"(subleaf));
}

extern "C" {
void syscall_entry();
void sysenter_entry();

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

static inline uint32_t user_access_begin() {
    smp::cpu_local *cpu = smp::get_cpu();
    uint32_t pkru = 0;
    if (cpu && cpu->cpu_features.pku) {
        pkru = read_pkru();
        if (pkru != 0) write_pkru(0);
    }
    if (smap_enabled()) asm volatile("stac" ::: "cc");
    return pkru;
}

static inline void user_access_end(uint32_t pkru) {
    if (smap_enabled()) asm volatile("clac" ::: "cc");
    smp::cpu_local *cpu = smp::get_cpu();
    if (cpu && cpu->cpu_features.pku && pkru != read_pkru()) write_pkru(pkru);
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

    uint32_t pkru = user_access_begin();
    memcpy(dst, src, size);
    user_access_end(pkru);
    return true;
}

static bool copy_to_user(void *dst, const void *src, uint64_t size) {
    if (size == 0) return true;
    scheduler::task *task = current_user_task();
    if (!task || !dst || !src) return false;
    if (!vmm::user_range_mapped((uint64_t)dst, size, true, task->cr3)) return false;

    uint32_t pkru = user_access_begin();
    memcpy(dst, src, size);
    user_access_end(pkru);
    return true;
}

static bool copy_user_cstr_page(scheduler::task *task, char *dst, const char *src,
                                 uint64_t offset, uint64_t max_len,
                                 uint64_t *copied, bool *terminated) {
    if (!task || !dst || !src || !copied || !terminated || offset >= max_len) return false;

    uint64_t user_addr = (uint64_t)src + offset;
    if (user_addr < (uint64_t)src) return false;

    uint64_t page_left = pmm::PAGE_SIZE - (user_addr & (pmm::PAGE_SIZE - 1));
    uint64_t chunk = max_len - offset;
    if (chunk > page_left) chunk = page_left;
    if (!vmm::user_range_mapped(user_addr, chunk, false, task->cr3)) return false;

    uint32_t pkru = user_access_begin();
    for (uint64_t i = 0; i < chunk; i++) {
        char c = src[offset + i];
        dst[offset + i] = c;
        if (c == 0) {
            user_access_end(pkru);
            *copied = i + 1;
            *terminated = true;
            return true;
        }
    }
    user_access_end(pkru);

    *copied = chunk;
    *terminated = false;
    return true;
}

static constexpr uint64_t MAX_USER_PATH = 4096;
static constexpr uint64_t MAX_CANONICAL_PATH = 4096;

static char *copy_string_from_user(const char *src, uint64_t max_len) {
    scheduler::task *task = current_user_task();
    if (!task || !src || max_len == 0) return nullptr;

    char *dst = (char *)heap::kmalloc(max_len);
    if (!dst) return nullptr;

    for (uint64_t i = 0; i < max_len;) {
        uint64_t copied = 0;
        bool terminated = false;
        if (!copy_user_cstr_page(task, dst, src, i, max_len, &copied, &terminated)) {
            heap::kfree(dst);
            return nullptr;
        }
        if (terminated) return dst;
        i += copied;
    }

    heap::kfree(dst);
    return nullptr;
}

static bool ensure_path_copy_capacity(char **dst, uint64_t *cap, uint64_t used, uint64_t need) {
    if (!dst || !*dst || !cap || *cap == 0 || need > 4096 || used > need) return false;
    if (need <= *cap) return true;

    uint64_t new_cap = *cap;
    while (new_cap < need) {
        if (new_cap >= 4096) return false;
        new_cap *= 2;
        if (new_cap > 4096) new_cap = 4096;
    }

    char *next = (char *)heap::kmalloc(new_cap);
    if (!next) return false;
    if (used) memcpy(next, *dst, used);
    heap::kfree(*dst);
    *dst = next;
    *cap = new_cap;
    return true;
}

static char *copy_path_from_user(const char *src) {
    scheduler::task *task = current_user_task();
    if (!task || !src) return nullptr;

    constexpr uint64_t MAX_PATH = MAX_USER_PATH;
    uint64_t cap = 128;
    char *dst = (char *)heap::kmalloc(cap);
    if (!dst) return nullptr;

    for (uint64_t i = 0; i < MAX_PATH;) {
        uint64_t user_addr = (uint64_t)src + i;
        if (user_addr < (uint64_t)src) {
            heap::kfree(dst);
            return nullptr;
        }

        uint64_t page_left = pmm::PAGE_SIZE - (user_addr & (pmm::PAGE_SIZE - 1));
        uint64_t chunk = MAX_PATH - i;
        if (chunk > page_left) chunk = page_left;
        if (!ensure_path_copy_capacity(&dst, &cap, i, i + chunk)) {
            heap::kfree(dst);
            return nullptr;
        }

        uint64_t copied = 0;
        bool terminated = false;
        if (!copy_user_cstr_page(task, dst, src, i, MAX_PATH, &copied, &terminated)) {
            heap::kfree(dst);
            return nullptr;
        }
        if (terminated) return dst;
        i += copied;
    }

    heap::kfree(dst);
    return nullptr;
}

static constexpr uint64_t SYSCALL_IOBUF_SIZE = 64ULL * 1024;

static uint8_t *syscall_iobuf(scheduler::task *task) {
    if (!task) return nullptr;
    if (!task->syscall_iobuf) task->syscall_iobuf = heap::kmalloc(SYSCALL_IOBUF_SIZE);
    return (uint8_t *)task->syscall_iobuf;
}

static bool append_bytes(char **buf, uint64_t *len, uint64_t *cap, const char *src, uint64_t n) {
    if (!buf || !len || !cap || !src || *len > MAX_CANONICAL_PATH) return false;
    if (n > MAX_CANONICAL_PATH || *len > MAX_CANONICAL_PATH - n) return false;
    if (n > UINT64_MAX - *len - 1) return false;
    uint64_t need = *len + n + 1;
    if (need > *cap) {
        uint64_t new_cap = *cap ? *cap : 16;
        while (new_cap < need) {
            if (new_cap >= MAX_CANONICAL_PATH + 1 || new_cap > UINT64_MAX / 2) return false;
            new_cap *= 2;
            if (new_cap > MAX_CANONICAL_PATH + 1) new_cap = MAX_CANONICAL_PATH + 1;
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
    if (!buf || !len || !cap || !component || comp_len > MAX_USER_PATH) return false;
    if (comp_len == 0 || (comp_len == 1 && component[0] == '.')) return true;
    if (comp_len == 2 && component[0] == '.' && component[1] == '.') {
        if (*len > 1) {
            while (*len > 1 && (*buf)[*len - 1] != '/') (*len)--;
            if (*len > 1 && (*buf)[*len - 1] == '/') (*len)--;
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
    char **argv = (char **)heap::kcalloc(MAX_ARGC + 1, sizeof(char *));
    if (!argv) return nullptr;

    for (uint64_t argc = 0; argc < MAX_ARGC; argc++) {
        char *arg_ptr = nullptr;
        if (task->abi == scheduler::task_abi::USER32) {
            uint32_t raw = 0;
            if (!copy_from_user(&raw, (uint32_t *)user_argv + argc, sizeof(raw))) {
                free_kernel_argv(argv);
                return nullptr;
            }
            arg_ptr = (char *)(uintptr_t)raw;
        } else if (!copy_from_user(&arg_ptr, &user_argv[argc], sizeof(arg_ptr))) {
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

    if (task->abi == scheduler::task_abi::USER32) {
        uint32_t terminator = 0;
        if (!copy_from_user(&terminator, (uint32_t *)user_argv + MAX_ARGC, sizeof(terminator)) ||
            terminator) {
            free_kernel_argv(argv);
            return nullptr;
        }
    } else {
        char *terminator = nullptr;
        if (!copy_from_user(&terminator, &user_argv[MAX_ARGC], sizeof(terminator)) || terminator) {
            free_kernel_argv(argv);
            return nullptr;
        }
    }

    return argv;
}

int sys_open(const char *path, int flags) {
    scheduler::task *current = current_user_task();
    if (!current || (flags & ~OPEN_ALLOWED_FLAGS) != 0) return -1;
    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    vfs::vfs_node *cwd = current->cwd ? current->cwd : vfs::get_root();
    vfs::vfs_node *node = vfs::open_at(cwd, kpath);
    bool created = false;
    if (!node && (flags & OPEN_CREAT)) {
        node = vfs::create_at(cwd, kpath, vfs::VfsType::VFS_FILE);
        created = node != nullptr;
    }
    if (node && (flags & OPEN_TRUNC) && (!created || node->size != 0) && vfs::truncate(node, 0) != 0) node = nullptr;

    heap::kfree(kpath);
    if (!node) return -1;
    if (!vfs::get_node(node)) return -1;

    int fd = fd_manager::alloc_fd(current);
    if (fd < 0) {
        vfs::put_node(node);
        return -1;
    }

    vfs::open_file *file = (vfs::open_file *)heap::kzalloc(sizeof(vfs::open_file));
    if (!file) {
        vfs::put_node(node);
        fd_manager::release_reserved_fd(fd, current);
        return -1;
    }
    file->node = node;
    file->offset = 0;
    file->ref_count = 1;
    file->device_ctx = nullptr;
    spinlock_init(&file->lock);
    if (devfs_open_file(node, file) != 0) {
        vfs::put_node(node);
        heap::kfree(file);
        fd_manager::release_reserved_fd(fd, current);
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

    constexpr uint64_t CHUNK = SYSCALL_IOBUF_SIZE;
    uint8_t *kbuf = syscall_iobuf(current);
    if (!kbuf) return -1;

    uint64_t lock_flags;
    spinlock_acquire(&file->lock, &lock_flags);

    uint64_t total = 0;
    while (total < size) {
        uint64_t want = size - total;
        if (want > CHUNK) want = CHUNK;

        bool is_dev = file->node && file->node->type == vfs::VfsType::VFS_CHAR_DEVICE &&
                      devfs_is_device_node(file->node);
        uint64_t got = is_dev ? devfs_read_file(file, 0, want, kbuf)
                              : vfs::read(file->node, file->offset, want, kbuf);
        if (got == 0) break;
        if (!copy_to_user((uint8_t *)buf + total, kbuf, got)) {
            spinlock_release(&file->lock, lock_flags);
            return -1;
        }

        if (!is_dev) file->offset += got;
        total += got;
        if (got < want) break;
    }

    spinlock_release(&file->lock, lock_flags);
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

    constexpr uint64_t CHUNK = SYSCALL_IOBUF_SIZE;
    uint8_t *kbuf = syscall_iobuf(current);
    if (!kbuf) return -1;

    uint64_t lock_flags;
    spinlock_acquire(&file->lock, &lock_flags);

    uint64_t total = 0;
    while (total < size) {
        uint64_t want = size - total;
        if (want > CHUNK) want = CHUNK;

        if (!copy_from_user(kbuf, (const uint8_t *)buf + total, want)) {
            spinlock_release(&file->lock, lock_flags);
            return -1;
        }

        bool is_dev = file->node && file->node->type == vfs::VfsType::VFS_CHAR_DEVICE &&
                      devfs_is_device_node(file->node);
        uint64_t offset = is_dev ? 0 : file->offset;
        uint64_t written = is_dev ? devfs_write_file(file, offset, want, kbuf)
                                  : vfs::write(file->node, offset, want, kbuf);
        if (written == 0) break;

        if (!is_dev) file->offset += written;
        total += written;
        if (written < want) break;
    }

    spinlock_release(&file->lock, lock_flags);
    return (int)total;
}

int sys_close(int fd) {
    scheduler::task *current = current_user_task();
    return current ? fd_manager::close_fd(fd, current) : -1;
}

static uint32_t stat_type_bits(vfs::VfsType type) {
    switch (type) {
        case vfs::VfsType::VFS_DIRECTORY: return STAT_IFDIR;
        case vfs::VfsType::VFS_CHAR_DEVICE: return STAT_IFCHR;
        case vfs::VfsType::VFS_BLOCK_DEVICE: return STAT_IFBLK;
        case vfs::VfsType::VFS_FILE: return STAT_IFREG;
    }
    return 0;
}

static uint32_t stat_mode_for(vfs::vfs_node *node) {
    uint32_t type = stat_type_bits(node->type);
    if (!type) return 0;
    uint32_t perm = STAT_DEFAULT_FILE_MODE;
    if (node->type == vfs::VfsType::VFS_DIRECTORY) perm = STAT_DEFAULT_DIR_MODE;
    if (node->type == vfs::VfsType::VFS_CHAR_DEVICE || node->type == vfs::VfsType::VFS_BLOCK_DEVICE)
        perm = STAT_DEFAULT_DEV_MODE;
    return type | perm;
}

static bool fill_stat(vfs::vfs_node *node, user_stat64 *out) {
    if (!vfs::valid_node(node) || !out) return false;
    uint32_t mode = stat_mode_for(node);
    if ((mode & STAT_IFMT) == 0) return false;

    memset(out, 0, sizeof(*out));
    out->st_ino = (uint16_t)(node->inode & 0xffffu);
    out->st_mode = mode;
    out->st_nlink = node->unlinked ? 0 : (node->type == vfs::VfsType::VFS_DIRECTORY ? 2 : 1);
    out->st_rdev = (int16_t)(node->inode & 0x7fffu);
    out->st_size = node->size > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)node->size;
    out->st_blksize = STAT_BLOCK_SIZE;
    out->st_blocks = (out->st_size + 511) / 512;
    return true;
}

int sys_fstat(int fd, void *user_out) {
    scheduler::task *current = current_user_task();
    if (!current || !user_out) return -1;
    if (!vmm::user_range_mapped((uint64_t)user_out, sizeof(user_stat64), true, current->cr3)) return -1;

    vfs::open_file *file = fd_manager::get_file(fd, current);
    if (!file) return -1;

    uint64_t flags;
    spinlock_acquire(&file->lock, &flags);
    user_stat64 st;
    bool ok = fill_stat(file->node, &st);
    spinlock_release(&file->lock, flags);
    return ok && copy_to_user(user_out, &st, sizeof(st)) ? 0 : -1;
}

int sys_stat(const char *path, void *user_out) {
    scheduler::task *current = current_user_task();
    if (!current || !user_out) return -1;
    if (!vmm::user_range_mapped((uint64_t)user_out, sizeof(user_stat64), true, current->cr3)) return -1;

    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    vfs::vfs_node *node = vfs::open_at(current->cwd ? current->cwd : vfs::get_root(), kpath);
    heap::kfree(kpath);
    user_stat64 st;
    return fill_stat(node, &st) && copy_to_user(user_out, &st, sizeof(st)) ? 0 : -1;
}

int64_t sys_seek(int fd, int64_t offset, int whence) {
    scheduler::task *current = current_user_task();
    if (!current) return -1;

    vfs::open_file *file = fd_manager::get_file(fd, current);
    if (!file) return -1;

    uint64_t flags;
    spinlock_acquire(&file->lock, &flags);

    vfs::vfs_node *node = file->node;
    if (!vfs::valid_node(node) || node->type == vfs::VfsType::VFS_CHAR_DEVICE) {
        spinlock_release(&file->lock, flags);
        return -1;
    }

    uint64_t base = 0;
    if (whence == SEEK_SET_K) {
        base = 0;
    } else if (whence == SEEK_CUR_K) {
        base = file->offset;
    } else if (whence == SEEK_END_K) {
        base = node->size;
    } else {
        spinlock_release(&file->lock, flags);
        return -1;
    }

    uint64_t next = 0;
    if (offset >= 0) {
        uint64_t add = (uint64_t)offset;
        if (add > UINT64_MAX - base || base + add > (uint64_t)INT64_MAX) {
            spinlock_release(&file->lock, flags);
            return -1;
        }
        next = base + add;
    } else {
        uint64_t sub = (uint64_t)(-(offset + 1)) + 1;
        if (sub > base) {
            spinlock_release(&file->lock, flags);
            return -1;
        }
        next = base - sub;
    }

    file->offset = next;
    spinlock_release(&file->lock, flags);
    return (int64_t)next;
}


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
    if ((!user_dent.name && user_dent.name_capacity) || (user_dent.name && !user_dent.name_capacity)) return -1;

    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    vfs::vfs_node *dir = vfs::open_at(current->cwd ? current->cwd : vfs::get_root(), kpath);
    heap::kfree(kpath);
    if (!dir || dir->type != vfs::VfsType::VFS_DIRECTORY) return -1;

    char small_name[256];
    vfs::vfs_dirent dent;
    memset(&dent, 0, sizeof(dent));
    dent.name = small_name;
    dent.name_capacity = sizeof(small_name);
    int rc = vfs::readdir(dir, index, &dent);
    if (rc != 0) return -1;

    char *kname = small_name;
    if (dent.name_len >= sizeof(small_name)) {
        if (dent.name_len == UINT64_MAX) return -1;
        uint64_t long_len = dent.name_len;
        kname = (char *)heap::kmalloc(long_len + 1);
        if (!kname) return -1;
        memset(&dent, 0, sizeof(dent));
        dent.name = kname;
        dent.name_capacity = long_len + 1;
        rc = vfs::readdir(dir, index, &dent);
        if (rc != 0 || dent.name_len != long_len) {
            heap::kfree(kname);
            return -1;
        }
    }

    vfs::vfs_dirent out = dent;
    out.name = (char *)user_dent.name;
    out.name_capacity = user_dent.name_capacity;

    if (!user_dent.name || user_dent.name_capacity <= dent.name_len) {
        if (kname != small_name) heap::kfree(kname);
        return copy_to_user(user_out, &out, sizeof(out)) ? 0 : -1;
    }

    if (!vmm::user_range_mapped((uint64_t)user_dent.name, dent.name_len + 1, true, current->cr3)) {
        if (kname != small_name) heap::kfree(kname);
        return -1;
    }

    bool ok = copy_to_user(user_dent.name, kname, dent.name_len + 1) &&
              copy_to_user(user_out, &out, sizeof(out));
    if (kname != small_name) heap::kfree(kname);
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
    if (!node || node->type != vfs::VfsType::VFS_DIRECTORY || !vfs::get_node(node)) {
        heap::kfree(new_path);
        return -1;
    }

    if (current->cwd) vfs::put_node(current->cwd);
    if (current->cwd_path) heap::kfree(current->cwd_path);
    current->cwd = node;
    current->cwd_path = new_path;
    current->cwd_path_len = strlen(new_path);
    return 0;
}

int sys_getcwd(char *buf, uint64_t size) {
    scheduler::task *current = current_user_task();
    if (!current || !buf || size == 0) return -1;
    const char *path = current->cwd_path ? current->cwd_path : "/";
    uint64_t len = (current->cwd_path ? current->cwd_path_len : 1) + 1;
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

static uint64_t user_top_for(scheduler::task *task) {
    return task && task->abi == scheduler::task_abi::USER32 ? scheduler::USER32_TOP
                                                            : vmm::user_top();
}

static uint64_t choose_mmap_addr(scheduler::task *task, uint64_t size) {
    uint64_t user_top = user_top_for(task);
    uint64_t mmap_min = task->abi == scheduler::task_abi::USER32 ? scheduler::USER32_MMAP_BASE_MIN
                                                                  : vmm::user_mmap_base_min();
    uint64_t mmap_window = task->abi == scheduler::task_abi::USER32 ? scheduler::USER32_MMAP_ASLR_WINDOW
                                                                     : vmm::user_mmap_aslr_window();
    uint64_t base = page_align_up(task->mmap_hint ? task->mmap_hint : mmap_min);
    uint64_t limit = mmap_min + mmap_window;
    if (limit > user_top) limit = user_top;

    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t cur = pass == 0 ? base : mmap_min;
        while (cur >= mmap_min && cur <= limit && size <= limit - cur) {
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

void *sys_mmap(void *addr, uint64_t length, int prot, int flags, int fd, uint64_t offset) {
    scheduler::task *current = current_user_task();
    if (!current) return (void *)-1;
    if (length == 0) return (void *)-1;
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) return (void *)-1;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return (void *)-1;
    if ((flags & ~(MAP_FIXED | MAP_ANONYMOUS | MAP_SHARED | MAP_PRIVATE)) != 0) return (void *)-1;
    bool anon = (flags & MAP_ANONYMOUS) != 0;
    bool shared = (flags & MAP_SHARED) != 0;
    bool priv = (flags & MAP_PRIVATE) != 0;
    if (shared && priv) return (void *)-1;
    if (anon && fd != -1) return (void *)-1;
    if (!anon && !shared) return (void *)-1;
    if (anon && offset != 0) return (void *)-1;
    if (!anon && (offset & (pmm::PAGE_SIZE - 1))) return (void *)-1;

    uint64_t size = 0;
    if (!page_align_up_checked(length, &size)) return (void *)-1;

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

    uint64_t user_top = user_top_for(current);
    if (virt == 0 || virt >= user_top || virt + size < virt || virt + size > user_top) {
        return (void *)-1;
    }
    if (!scheduler::vma_range_free(current, virt, virt + size)) return (void *)-1;

    vmm::PageFlags page_flags = prot_to_page_flags(prot);
    scheduler::vma_type type = scheduler::vma_type::ANON;

    if (anon) {
        vmm::map_demand_zero_range(virt, size, page_flags, current->cr3);
    } else {
        vfs::open_file *file = fd_manager::get_file(fd, current);
        if (!file) return (void *)-1;
        device_mmap_result mm = {};
        if (devfs_mmap_file(file, offset, size, &mm) != 0) return (void *)-1;
        if (mm.size < size || (mm.phys & (pmm::PAGE_SIZE - 1))) return (void *)-1;
        if (mm.flags & DEVICE_MMAP_WRITE_COMBINING) page_flags |= vmm::PageFlags::WriteCombining;
        if (mm.flags & DEVICE_MMAP_NO_CACHE) page_flags |= vmm::PageFlags::NoCache;
        vmm::map_range(virt, mm.phys, size, page_flags, current->cr3);
        type = scheduler::vma_type::DEVICE;
    }

    if (!scheduler::add_vma(current, virt, virt + size, virt, prot_to_vma_flags(prot), type)) {
        vmm::unmap_range(virt, size, current->cr3);
        return (void *)-1;
    }

    if ((flags & MAP_FIXED) == 0) current->mmap_hint = virt + size;
    return (void *)virt;
}

int sys_munmap(void *addr, uint64_t length) {
    scheduler::task *current = current_user_task();
    if (!current) return -1;
    if (!addr || length == 0) return -1;

    uint64_t virt = page_align_down((uint64_t)addr);
    uint64_t span = ((uint64_t)addr - virt);
    if (length > UINT64_MAX - span) return -1;
    uint64_t size = 0;
    if (!page_align_up_checked(span + length, &size)) return -1;
    uint64_t user_top = user_top_for(current);
    if (virt == 0 || virt >= user_top || size > user_top - virt) return -1;

    uint64_t end = virt + size;
    scheduler::vm_area *area = scheduler::find_vma(current, virt);
    if (!area || (area->type != scheduler::vma_type::ANON && area->type != scheduler::vma_type::DEVICE) || end > area->end) return -1;

    uint64_t old_area_end = area->end;
    bool split = virt > area->start && end < area->end;
    scheduler::vm_area *tail = nullptr;
    if (split) {
        tail = scheduler::add_vma(current, end, old_area_end, end, area->flags, area->type);
        if (!tail) return -1;
    }

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
        (void)tail;
    }
    return 0;
}

uint64_t sys_brk(void *addr) {
    scheduler::task *current = current_user_task();
    if (!current || !current->heap_vma) return 0;

    uint64_t requested = (uint64_t)addr;
    scheduler::vm_area *heap_vma = current->heap_vma;
    uint64_t heap_start = heap_vma->start;
    if (requested == 0) return current->program_break;
    if (requested >= user_top_for(current)) return current->program_break;
    if (requested < heap_start) return current->program_break;

    if (current->stack_vma && requested > current->stack_vma->start) return current->program_break;

    uint64_t old_break = current->program_break;
    uint64_t old_mapped_end = 0;
    uint64_t new_mapped_end = 0;
    if (!page_align_up_checked(old_break, &old_mapped_end) ||
        !page_align_up_checked(requested, &new_mapped_end)) {
        return old_break;
    }

    if (new_mapped_end > old_mapped_end) {
        if (range_overlaps_vma(current, heap_vma, heap_vma->end, new_mapped_end)) {
            return old_break;
        }
        vmm::map_demand_zero_range(old_mapped_end, new_mapped_end - old_mapped_end,
                                   vmm::PageFlags::User | vmm::PageFlags::Write |
                                       vmm::PageFlags::NX,
                                   current->cr3);
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


static int sys_set_fs_base(uint64_t base) {
    scheduler::task *current = current_user_task();
    if (!current) return -1;
    if (base >= user_top_for(current)) return -1;
    current->fs_base = base;
    apic::wrmsr(0xC0000100, base);
    return 0;
}

static uint64_t sys_get_fs_base() {
    scheduler::task *current = current_user_task();
    if (!current) return (uint64_t)-1;
    current->fs_base = apic::rdmsr(0xC0000100);
    return current->fs_base;
}

static int sys_set_gs_base(uint64_t base) {
    scheduler::task *current = current_user_task();
    if (!current) return -1;
    if (base >= user_top_for(current)) return -1;
    current->gs_base = base;
    apic::wrmsr(0xC0000102, base);
    return 0;
}

static uint64_t sys_get_gs_base() {
    scheduler::task *current = current_user_task();
    if (!current) return (uint64_t)-1;
    current->gs_base = apic::rdmsr(0xC0000102);
    return current->gs_base;
}

int sys_pkey_mprotect(void *addr, uint64_t length, int prot, int pkey) {
    scheduler::task *current = current_user_task();
    smp::cpu_local *cpu = smp::get_cpu();
    if (!cpu || !cpu->cpu_features.pku || !current) return -1;
    if (!addr || length == 0 || pkey < 0 || pkey >= 16) return -1;
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) return -1;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return -1;

    uint64_t virt = page_align_down((uint64_t)addr);
    uint64_t span = ((uint64_t)addr - virt);
    if (length > UINT64_MAX - span) return -1;
    uint64_t size = 0;
    if (!page_align_up_checked(length + span, &size)) return -1;
    if (virt == 0 || virt + size < virt || virt + size > user_top_for(current)) return -1;

    scheduler::vm_area *vma = scheduler::find_vma_exact(current, virt, virt + size);
    if (!vma || vma->committed_start > virt) return -1;

    vmm::PageFlags page_flags = prot_to_page_flags(prot);
    if (!vmm::set_user_pkey_range(virt, size, (uint8_t)pkey, current->cr3)) return -1;
    vma->flags = prot_to_vma_flags(prot);
    vma->pkey = (uint8_t)pkey;

    for (uint64_t page = virt; page < virt + size; page += pmm::PAGE_SIZE) {
        uint64_t phys = vmm::get_mapping(page, current->cr3);
        if (!phys) continue;
        vmm::map_page(page, phys, page_flags | (vmm::PageFlags)((uint64_t)pkey << 59),
                      vmm::PageSize::Size4K, current->cr3);
    }
    return 0;
}

int sys_dup2(int oldfd, int newfd) {
    scheduler::task *current = current_user_task();
    if (!current) return -1;

    auto *file = fd_manager::get_file(oldfd, current);
    if (!file) return -1;
    if (newfd < 0 || newfd >= 512) return -1;
    if (oldfd == newfd) return newfd;

    if (!fd_manager::expand_table(newfd + 1, current)) return -1;

    if (current->fd_table[newfd] != nullptr && fd_manager::close_fd(newfd, current) != 0) return -1;

    uint64_t flags;
    spinlock_acquire(&file->lock, &flags);
    file->ref_count++;
    spinlock_release(&file->lock, flags);

    current->fd_table[newfd] = file;
    if (!fd_manager::reserve_fd(newfd, current)) {
        current->fd_table[newfd] = nullptr;
        uint64_t rollback_flags;
        spinlock_acquire(&file->lock, &rollback_flags);
        if (file->ref_count > 0) file->ref_count--;
        spinlock_release(&file->lock, rollback_flags);
        return -1;
    }
    return newfd;
}

static uint64_t finish_user_syscall(struct regs *r, uint64_t result) {
    r->rax = result;
    if (scheduler::complete_fault_return_if_pending(r)) return r->rax;
    scheduler::apply_pending_kill(r);
    return r->rax;
}

uint64_t syscall_handler(struct regs *r) {
    scheduler::task *task = current_user_task();
    if (!task || !r) return (uint64_t)-1;

    scheduler::apply_pending_kill(r);

    bool compat = task->abi == scheduler::task_abi::USER32;
    if (r->int_no == 128 && !compat) return finish_user_syscall(r, (uint64_t)-1);
    if (r->int_no == 0x81 && !compat) {
        r->cs = gdt::selectors::UCODE64_SEL;
        r->ss = gdt::selectors::UDATA64_SEL;
        return finish_user_syscall(r, (uint64_t)-1);
    }
    if (r->int_no == 0 && compat) {
        r->cs = gdt::selectors::UCODE32_SEL;
        r->ss = gdt::selectors::UDATA32_SEL;
        return finish_user_syscall(r, (uint64_t)-1);
    }

    bool fast32 = compat && r->int_no == 0x81;
    uint64_t syscall = compat ? (uint32_t)r->rax : r->rax;
    uint64_t arg1 = compat ? (uint32_t)r->rbx : r->rdi;
    uint64_t arg2 = fast32 ? (uint32_t)r->rsi : (compat ? (uint32_t)r->rcx : r->rsi);
    uint64_t arg3 = fast32 ? (uint32_t)r->rdi : (compat ? (uint32_t)r->rdx : r->rdx);
    uint64_t arg4 = fast32 ? (uint32_t)r->rbp : (compat ? (uint32_t)r->rsi : r->r10);
    uint64_t arg5 = compat ? 0 : r->r8;
    uint64_t arg6 = compat ? 0 : r->r9;
    uint64_t result = (uint64_t)-1;

    switch (syscall) {
        case SYSCALL_WRITE:
            result = sys_write((int)arg1, (const void *)arg2, (uint64_t)arg3);
            break;
        case SYSCALL_OPEN:
            result = sys_open((const char *)arg1, (int)arg2);
            break;
        case SYSCALL_READ:
            result = sys_read((int)arg1, (void *)arg2, (uint64_t)arg3);
            break;
        case SYSCALL_CLOSE:
            result = sys_close((int)arg1);
            break;
        case SYSCALL_DUP2:
            result = sys_dup2((int)arg1, (int)arg2);
            break;
        case SYSCALL_YIELD:
            scheduler::yield();
            result = 0;
            break;
        case SYSCALL_SLEEP:
            scheduler::sleep(arg1);
            result = 0;
            break;
        case SYSCALL_EXIT:
            scheduler::exit((int)arg1);
            result = 0;
            break;
        case SYSCALL_CLONE:
            result = (uint64_t)scheduler::clone(arg1, (void *)arg2, r);
            break;
        case SYSCALL_FORK:
            result = (uint64_t)scheduler::clone(0, nullptr, r);
            break;
        case SYSCALL_EXEC: {
            char *kpath = copy_path_from_user((const char *)arg1);
            if (!kpath) break;

            char **kargv = copy_argv_from_user((char **)arg2);
            if (arg2 && !kargv) {
                heap::kfree(kpath);
                break;
            }

            scheduler::task *current = current_user_task();
            char *exec_path = nullptr;
            if (strchr(kpath, '/')) {
                exec_path = canonicalize_path(current ? current->cwd_path : "/", kpath);
            } else {
                exec_path = scheduler::resolve_launch_exec(current, kpath);
            }
            heap::kfree(kpath);
            if (!exec_path) {
                free_kernel_argv(kargv);
                break;
            }

            result = (uint64_t)scheduler::exec(exec_path, kargv, r);
            heap::kfree(exec_path);
            free_kernel_argv(kargv);
            break;
        }
        case SYSCALL_WAIT: {
            int status = 0;
            int *status_ptr = (int *)arg1;
            if (status_ptr) {
                scheduler::task *current = current_user_task();
                if (!current || !vmm::user_range_mapped((uint64_t)status_ptr, sizeof(int), true,
                                                        current->cr3))
                    break;
            }

            int ret = scheduler::wait(status_ptr ? &status : nullptr);
            if (ret >= 0 && status_ptr && !copy_to_user(status_ptr, &status, sizeof(status))) break;
            result = (uint64_t)ret;
            break;
        }
        case SYSCALL_GETPID:
            result = task->id;
            break;
        case SYSCALL_MMAP:
            result = (uint64_t)sys_mmap((void *)arg1, (uint64_t)arg2, (int)arg3, (int)arg4, (int)arg5, (uint64_t)arg6);
            break;
        case SYSCALL_MUNMAP:
            result = (uint64_t)sys_munmap((void *)arg1, (uint64_t)arg2);
            break;
        case SYSCALL_BRK:
            result = sys_brk((void *)arg1);
            break;
        case SYSCALL_CREATE:
            result = sys_create((const char *)arg1);
            break;
        case SYSCALL_UNLINK:
            result = sys_unlink((const char *)arg1);
            break;
        case SYSCALL_RENAME:
            result = sys_rename((const char *)arg1, (const char *)arg2);
            break;
        case SYSCALL_READDIR:
            result = sys_readdir((const char *)arg1, (uint64_t)arg2, (void *)arg3);
            break;
        case SYSCALL_CHDIR:
            result = sys_chdir((const char *)arg1);
            break;
        case SYSCALL_GETCWD:
            result = sys_getcwd((char *)arg1, (uint64_t)arg2);
            break;
        case SYSCALL_FSCTL:
            result = sys_fsctl((int)arg1, (void *)arg2);
            break;
        case SYSCALL_PKEY_MPROTECT:
            result = (uint64_t)sys_pkey_mprotect((void *)arg1, (uint64_t)arg2, (int)arg3, (int)arg4);
            break;
        case SYSCALL_SEEK:
            result = (uint64_t)sys_seek((int)arg1, (int64_t)arg2, (int)arg3);
            break;
        case SYSCALL_STAT:
            result = (uint64_t)sys_stat((const char *)arg1, (void *)arg2);
            break;
        case SYSCALL_FSTAT:
            result = (uint64_t)sys_fstat((int)arg1, (void *)arg2);
            break;
        case SYSCALL_SET_FS_BASE:
            result = (uint64_t)sys_set_fs_base(arg1);
            break;
        case SYSCALL_GET_FS_BASE:
            result = sys_get_fs_base();
            break;
        case SYSCALL_SET_GS_BASE:
            result = (uint64_t)sys_set_gs_base(arg1);
            break;
        case SYSCALL_GET_GS_BASE:
            result = sys_get_gs_base();
            break;
        default:
            break;
    }
    return finish_user_syscall(r, result);
}

void enable_syscalls() {
    uint32_t efer_low, efer_high;
    asm volatile("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(IA32_EFER));
    efer_low |= 1;
    asm volatile("wrmsr" : : "a"(efer_low), "d"(efer_high), "c"(IA32_EFER));

    uint64_t addr = (uint64_t)syscall_entry;
    asm volatile("wrmsr" : : "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)), "c"(IA32_LSTAR));

    uint32_t eax, ebx, ecx, edx;
    cpuid_leaf(1, 0, &eax, &ebx, &ecx, &edx);
    sysenter_available = (edx & (1u << 11)) != 0;
    if (sysenter_available) {
        uint64_t sysenter_addr = (uint64_t)sysenter_entry;
        asm volatile("wrmsr" : : "a"((uint32_t)gdt::selectors::KCODE_SEL), "d"(0),
                     "c"(IA32_SYSENTER_CS));
        asm volatile("wrmsr" : : "a"((uint32_t)sysenter_addr),
                     "d"((uint32_t)(sysenter_addr >> 32)), "c"(IA32_SYSENTER_EIP));
    }

    uint64_t kernel_base = gdt::selectors::KCODE_SEL;
    uint64_t user_base = gdt::selectors::SYSRET_USER_BASE;

    uint64_t star = (kernel_base << 32) | (user_base << 48);

    asm volatile("wrmsr" : : "a"(0), "d"((uint32_t)(star >> 32)), "c"(IA32_STAR));
    asm volatile("wrmsr" : : "a"(0x200), "d"(0), "c"(IA32_FMASK));
}
}
extern "C" void syscall_set_kernel_stack(uint64_t stack_top) {
    if (!sysenter_available) return;
    asm volatile("wrmsr" : : "a"((uint32_t)stack_top), "d"((uint32_t)(stack_top >> 32)),
                 "c"(IA32_SYSENTER_ESP));
}
