#include <sys/types.h>
#include <unistd.h>

#include "stddef.h"
#include "stdint.h"

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

uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
}

void _exit(int code) {
    syscall3(SYSCALL_EXIT, (uint64_t)code, 0, 0);
    __builtin_unreachable();
}

int write(int fd, const void *buf, size_t count) {
    return (int)syscall3(SYSCALL_WRITE, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

int read(int fd, void *buf, size_t count) {
    return (int)syscall3(SYSCALL_READ, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

int close(int fd) { return (int)syscall3(SYSCALL_CLOSE, (uint64_t)fd, 0, 0); }

int getpid(void) { return (int)syscall3(SYSCALL_GETPID, 0, 0, 0); }

void *sbrk(ptrdiff_t increment) {
    return NULL;  // TODO
}

off_t lseek(int fd, off_t offset, int whence) {
    // Requires a SYSCALL_LSEEK in your kernel to modify file->offset
    return 0;
}

int fstat(int fd, void *st) {
    // Requires a SYSCALL_STAT/FSTAT
    return 0;
}

int isatty(int fd) {
    // Typically returns 1 if the FD is a terminal (like COM1 or VGA)
    return -1;
}

int kill(int pid, int sig) {
    // Requires a SYSCALL_KILL
    return -1;
}

pid_t fork(void) { return syscall3(SYSCALL_FORK, 0, 0, 0); }

int sched_yield(void) {
    syscall3(SYSCALL_YIELD, 0, 0, 0);
    return 0;
}