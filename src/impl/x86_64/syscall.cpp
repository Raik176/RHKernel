#include "console.h"
#include "file/fd.h"
#include "file/vfs.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
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
    SYSCALL_READDIR
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

static inline void user_access_begin() {
    if (smp::get_cpu()->cpu_features.smap) asm volatile("stac" ::: "cc");
}

static inline void user_access_end() {
    if (smp::get_cpu()->cpu_features.smap) asm volatile("clac" ::: "cc");
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
    scheduler::task *current = smp::get_cpu()->current_task;
    char *kpath = copy_string_from_user(path, 4096);
    if (!kpath) return -1;

    vfs::vfs_node *node = vfs::open(kpath);
    if (!node && (flags & 0x40)) node = vfs::create(kpath, vfs::VfsType::VFS_FILE);
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
    char *kpath = copy_string_from_user(path, 4096);
    if (!kpath) return -1;
    vfs::vfs_node *node = vfs::create(kpath, vfs::VfsType::VFS_FILE);
    heap::kfree(kpath);
    return node ? 0 : -1;
}

int sys_unlink(const char *path) {
    char *kpath = copy_string_from_user(path, 4096);
    if (!kpath) return -1;
    int ret = vfs::unlink(kpath);
    heap::kfree(kpath);
    return ret;
}

int sys_rename(const char *old_path, const char *new_path) {
    char *kold = copy_string_from_user(old_path, 4096);
    if (!kold) return -1;
    char *knew = copy_string_from_user(new_path, 4096);
    if (!knew) {
        heap::kfree(kold);
        return -1;
    }

    int ret = vfs::rename(kold, knew);
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

    char *kpath = copy_string_from_user(path, 4096);
    if (!kpath) return -1;

    vfs::vfs_node *dir = vfs::open(kpath);
    heap::kfree(kpath);
    if (!dir || dir->type != vfs::VfsType::VFS_DIRECTORY) return -1;

    vfs::vfs_dirent dent;
    if (vfs::readdir(dir, index, &dent) != 0) return -1;
    return copy_to_user(user_out, &dent, sizeof(dent)) ? 0 : -1;
}

void *sys_mmap(void *addr, uint64_t length, int prot, int flags) {
    scheduler::task *current = smp::get_cpu()->current_task;
    if (!current || current->type != scheduler::task_type::USER) return (void *)-1;
    if (length == 0) return (void *)-1;

    uint64_t size = page_align_up(length);
    if (size < length) return (void *)-1;
    uint64_t virt = 0;

    if ((flags & MAP_FIXED) != 0) {
        virt = page_align_down((uint64_t)addr);
        if (virt == 0) return (void *)-1;
    } else if (addr != nullptr) {
        virt = page_align_up((uint64_t)addr);
    } else {
        if (current->mmap_next == 0) current->mmap_next = 0x0000400000000000ULL;
        virt = page_align_up(current->mmap_next);
        current->mmap_next = virt + size;
    }

    // Keep mappings canonical and out of the reserved growable user stack.
    if (virt == 0 || virt >= 0x0000800000000000ULL || virt + size < virt ||
        virt + size > 0x0000800000000000ULL) {
        return (void *)-1;
    }
    uint64_t stack_limit = current->user_stack_limit ? current->user_stack_limit
                                                     : scheduler::USER_STACK_TOP - scheduler::MAX_USER_STACK_SIZE;
    uint64_t stack_top = current->user_stack_top ? current->user_stack_top : scheduler::USER_STACK_TOP;
    if (virt < stack_top && virt + size > stack_limit) { return (void *)-1; }

    vmm::PageFlags page_flags = vmm::PageFlags::User;
    if (prot & PROT_WRITE) page_flags |= vmm::PageFlags::Write;
    if (!(prot & PROT_EXEC)) page_flags |= vmm::PageFlags::NX;

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

    return (void *)virt;
}

int sys_munmap(void *addr, uint64_t length) {
    scheduler::task *current = smp::get_cpu()->current_task;
    if (!current || current->type != scheduler::task_type::USER) return -1;
    if (!addr || length == 0) return -1;

    uint64_t virt = page_align_down((uint64_t)addr);
    uint64_t size = page_align_up(((uint64_t)addr - virt) + length);
    if (size < length || virt == 0 || virt >= 0x0000800000000000ULL ||
        size > 0x0000800000000000ULL - virt) {
        return -1;
    }

    uint64_t stack_limit = current->user_stack_limit ? current->user_stack_limit
                                                     : scheduler::USER_STACK_TOP - scheduler::MAX_USER_STACK_SIZE;
    uint64_t stack_top = current->user_stack_top ? current->user_stack_top : scheduler::USER_STACK_TOP;
    if (virt < stack_top && virt + size > stack_limit) return -1;

    for (uint64_t off = 0; off < size; off += pmm::PAGE_SIZE) {
        uint64_t phys = vmm::get_mapping(virt + off, current->cr3);
        if (phys) pmm::unref_page(phys);
        vmm::unmap_page(virt + off, current->cr3);
    }
    return 0;
}

uint64_t sys_brk(void *addr) {
    scheduler::task *current = smp::get_cpu()->current_task;
    if (!current || current->type != scheduler::task_type::USER) return 0;

    uint64_t requested = (uint64_t)addr;
    if (requested == 0) return current->program_break;
    if (requested >= 0x0000800000000000ULL) return current->program_break;
    if (current->heap_start == 0) current->heap_start = page_align_up(current->program_break);
    if (requested < current->heap_start) return current->program_break;

    uint64_t stack_limit = current->user_stack_limit ? current->user_stack_limit
                                                     : scheduler::USER_STACK_TOP - scheduler::MAX_USER_STACK_SIZE;
    if (requested > stack_limit) return current->program_break;

    uint64_t old_break = current->program_break;
    uint64_t old_mapped_end = page_align_up(old_break);
    uint64_t new_mapped_end = page_align_up(requested);

    if (new_mapped_end > old_mapped_end) {
        uint64_t grow = new_mapped_end - old_mapped_end;
        void *mapped = sys_mmap((void *)old_mapped_end, grow, PROT_READ | PROT_WRITE,
                                MAP_ANONYMOUS | MAP_FIXED);
        if (mapped == (void *)-1) return old_break;
    }

    // Shrinking only moves the logical break. Keeping the already-mapped tail is
    // intentional for now because this kernel does not yet track VMAs/physical
    // runs well enough to safely free arbitrary partial heap ranges.
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
            char *kpath = copy_string_from_user((const char *)arg1, 4096);
            if (!kpath) return (uint64_t)-1;

            char **kargv = copy_argv_from_user((char **)arg2);
            if (arg2 && !kargv) {
                heap::kfree(kpath);
                return (uint64_t)-1;
            }

            uint64_t ret = (uint64_t)scheduler::exec(kpath, kargv, r);
            heap::kfree(kpath);
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