#include "console.h"
#include "file/fd.h"
#include "file/vfs.h"
#include "memory/heap.h"
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
    SYSCALL_GETPID
};

extern "C" {
void syscall_entry();

static inline void user_access_begin() {
    if (smp::get_cpu()->cpu_features.smap) asm volatile("stac" ::: "cc");
}

static inline void user_access_end() {
    if (smp::get_cpu()->cpu_features.smap) asm volatile("clac" ::: "cc");
}

int sys_open(const char *path) {
    scheduler::task *current = smp::get_cpu()->current_task;

    user_access_begin();
    vfs::vfs_node *node = vfs::open(path);
    user_access_end();

    if (!node) return -1;

    vfs::open_file *file = (vfs::open_file *)heap::kmalloc(sizeof(vfs::open_file));
    file->node = node;
    file->offset = 0;
    file->ref_count = 1;  // Initialize reference count

    int fd = fd_manager::alloc_fd(current);
    current->fd_table[fd] = file;
    return fd;
}

int sys_read(int fd, void *buf, uint32_t size) {
    vfs::open_file *file = fd_manager::get_file(fd);
    if (!file) return -1;

    user_access_begin();
    uint32_t bytes_read = vfs::read(file->node, file->offset, size, buf);
    user_access_end();

    file->offset += bytes_read;
    return (int)bytes_read;
}

int sys_write(int fd, const void *buf, uint64_t size) {
    vfs::open_file *file = fd_manager::get_file(fd);
    if (!file) return -1;

    user_access_begin();
    uint32_t written = vfs::write(file->node, file->offset, size, (void *)buf);
    user_access_end();

    file->offset += written;
    return (int)written;
}

int sys_close(int fd) {
    auto *current = smp::get_cpu()->current_task;
    vfs::open_file *file = fd_manager::get_file(fd, current);
    if (!file) return -1;

    file->ref_count--;
    if (file->ref_count == 0) { heap::kfree(file); }

    current->fd_table[fd] = nullptr;
    return 0;
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

    // Assign the new reference
    current->fd_table[newfd] = file;
    file->ref_count++;

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
            return sys_open((const char *)arg1);
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
            user_access_begin();
            uint64_t ret = (uint64_t)scheduler::exec((const char *)arg1, (char **)arg2);
            user_access_end();

            return ret;
        }
        case SYSCALL_WAIT: {
            int *status_ptr = (int *)arg1;

            user_access_begin();
            uint64_t ret = (uint64_t)scheduler::wait(status_ptr);
            user_access_end();

            return ret;
        }
        case SYSCALL_GETPID:
            return smp::get_cpu()->current_task->id;
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
    uint64_t user_base = (gdt::selectors::UCODE64_SEL & ~3);

    uint64_t star = (kernel_base << 32) | (user_base << 48);

    asm volatile("wrmsr" : : "a"(0), "d"((uint32_t)(star >> 32)), "c"(IA32_STAR));
    asm volatile("wrmsr" : : "a"(0x200), "d"(0), "c"(IA32_FMASK));
}
}