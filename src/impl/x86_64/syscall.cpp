#include "console.h"
#include "memory/heap.h"
#include "smp/scheduler.h"
#include "smp/smp.h"
#include "string.h"
#include "util.h"

#define IA32_EFER 0xC0000080
#define IA32_STAR 0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_FMASK 0xC0000084

enum SyscallNumbers {
    SYSCALL_PRINT = 0,
    SYSCALL_OPEN,
    SYSCALL_READ,
    SYSCALL_CLOSE,
    SYSCALL_YIELD,
    SYSCALL_SLEEP
};

extern "C" {
void syscall_entry();

int get_free_fd(scheduler::task *t) {
    for (uint32_t i = 0; i < t->fd_capacity; i++) {
        if (t->fd_table[i] == nullptr) return (int)i;
    }

    uint32_t old_capacity = t->fd_capacity;
    uint32_t new_capacity = (old_capacity == 0) ? scheduler::INITIAL_FD_CAPACITY : old_capacity * 2;

    vfs::open_file **new_table =
        (vfs::open_file **)heap::kmalloc(sizeof(vfs::open_file *) * new_capacity);
    memset(new_table, 0, sizeof(vfs::open_file *) * new_capacity);

    if (old_capacity > 0) {
        memcpy(new_table, t->fd_table, sizeof(vfs::open_file *) * old_capacity);
        heap::kfree(t->fd_table);
    }

    t->fd_table = new_table;
    t->fd_capacity = new_capacity;

    return (int)old_capacity;
}

int sys_open(const char *path) {
    scheduler::task *current = smp::get_cpu()->current_task;
    vfs::vfs_node *node = vfs::open(path);
    if (!node) return -1;

    int fd = get_free_fd(current);

    vfs::open_file *file = (vfs::open_file *)heap::kmalloc(sizeof(vfs::open_file));
    file->node = node;
    file->offset = 0;

    current->fd_table[fd] = file;
    return fd;
}

int sys_read(int fd, void *buf, uint32_t size) {
    scheduler::task *current = smp::get_cpu()->current_task;

    // Bounds check against dynamic capacity
    if (fd < 0 || (uint32_t)fd >= current->fd_capacity || !current->fd_table[fd]) { return -1; }

    vfs::open_file *file = current->fd_table[fd];
    uint32_t bytes_read = vfs::read(file->node, file->offset, size, buf);

    file->offset += bytes_read;
    return (int)bytes_read;
}

int sys_close(int fd) {
    scheduler::task *current = smp::get_cpu()->current_task;

    if (fd < 0 || (uint32_t)fd >= current->fd_capacity || !current->fd_table[fd]) { return -1; }

    heap::kfree(current->fd_table[fd]);
    current->fd_table[fd] = nullptr;
    return 0;
}

uint64_t syscall_handler(struct regs *r) {
    uint64_t syscall = r->rax;
    uint64_t arg1 = r->rdi;
    uint64_t arg2 = r->rsi;
    uint64_t arg3 = r->rdx;
    uint64_t arg4 = r->r10;
    uint64_t arg5 = r->r8;
    uint64_t arg6 = r->r9;

    switch (syscall) {
        case SYSCALL_PRINT: {
            const char *user_str = (const char *)arg1;
            console::printf(user_str);
            return 0;
        }
        case SYSCALL_OPEN:
            return sys_open((const char *)arg1);
        case SYSCALL_READ:  // TODO: map to user space?
            return sys_read((int)arg1, (void *)arg2, (uint32_t)arg3);
        case SYSCALL_CLOSE:
            return sys_close((int)arg1);
        case SYSCALL_YIELD:
            scheduler::yield();
            return 0;
        case SYSCALL_SLEEP:
            scheduler::sleep(arg1);
            return 0;
        default:
            console::printf("Unknown syscall: %d\n", syscall);
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

    uint64_t star = ((uint64_t)0x08 << 32) |        // kernel CS
                    ((uint64_t)(0x23 - 16) << 48);  // user CS - 16

    asm volatile("wrmsr" : : "a"(0), "d"((uint32_t)(star >> 32)), "c"(IA32_STAR));
    asm volatile("wrmsr" : : "a"(0x200), "d"(0), "c"(IA32_FMASK));
}
}