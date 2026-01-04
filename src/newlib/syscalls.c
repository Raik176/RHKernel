#include "stdint.h"
#include "stddef.h"

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

int write(int fd, const void *buf, unsigned int count) {
    return (int)syscall3(SYSCALL_WRITE, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

int read(int fd, void *buf, unsigned int count) {
    return (int)syscall3(SYSCALL_READ, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

int close(int fd) {
    return (int)syscall3(SYSCALL_CLOSE, (uint64_t)fd, 0, 0);
}

int getpid(void) {
    return (int)syscall3(SYSCALL_GETPID, 0, 0, 0);
}

void *sbrk(ptrdiff_t increment) {
    return NULL; //TODO
}

int lseek(int fd, int ptr, int dir) {
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

int _open(const char *name, int flags, int mode) {
    return (int)syscall3(SYSCALL_OPEN, (uint64_t)name, (uint64_t)flags, (uint64_t)mode);
}

int _wait(int *status) {
    return (int)syscall3(SYSCALL_WAIT, (uint64_t)status, 0, 0);
}

int _fork(void) {
    return (int)syscall3(SYSCALL_FORK, 0, 0, 0);
}