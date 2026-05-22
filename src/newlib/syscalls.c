#include <sys/types.h>
#include <stdarg.h>
#include <unistd.h>

struct _reent;

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

uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
}

uint64_t syscall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a4;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
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

int open(const char *path, int flags, ...) {
    return (int)syscall3(SYSCALL_OPEN, (uint64_t)path, (uint64_t)flags, 0);
}

int getpid(void) { return (int)syscall3(SYSCALL_GETPID, 0, 0, 0); }

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)fd;
    (void)offset;
    return (void *)syscall4(SYSCALL_MMAP, (uint64_t)addr, (uint64_t)length, (uint64_t)prot,
                            (uint64_t)flags);
}

int munmap(void *addr, size_t length) {
    return (int)syscall3(SYSCALL_MUNMAP, (uint64_t)addr, (uint64_t)length, 0);
}

int brk(void *addr) {
    uint64_t ret = syscall3(SYSCALL_BRK, (uint64_t)addr, 0, 0);
    return ret == (uint64_t)addr ? 0 : -1;
}

void *sbrk(ptrdiff_t increment) {
    void *old_break = (void *)syscall3(SYSCALL_BRK, 0, 0, 0);
    if (increment == 0) return old_break;

    uintptr_t old_addr = (uintptr_t)old_break;
    uintptr_t new_addr = old_addr + increment;
    if ((increment > 0 && new_addr < old_addr) || (increment < 0 && new_addr > old_addr)) {
        return (void *)-1;
    }

    void *new_break = (void *)new_addr;
    if (brk(new_break) != 0) return (void *)-1;
    return old_break;
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
    return fd >= 0 && fd <= 2;
}

int unlink(const char *path) { return (int)syscall3(SYSCALL_UNLINK, (uint64_t)path, 0, 0); }
int rename(const char *old_path, const char *new_path) { return (int)syscall3(SYSCALL_RENAME, (uint64_t)old_path, (uint64_t)new_path, 0); }

int kill(int pid, int sig) {
    // Requires a SYSCALL_KILL
    return -1;
}

pid_t fork(void) { return syscall3(SYSCALL_FORK, 0, 0, 0); }

int sched_yield(void) {
    syscall3(SYSCALL_YIELD, 0, 0, 0);
    return 0;
}

/* Newlib hooks.  Newlib already provides the reentrant _*_r wrappers
   (writer.c/readr.c/closer.c/lseekr.c/etc.) and those wrappers call these
   primitive underscored syscalls.  Defining _*_r here causes duplicate-symbol
   link errors, so only provide the primitive hooks below. */
void *_sbrk(ptrdiff_t increment) { return sbrk(increment); }
int _write(int fd, const void *buf, size_t count) { return write(fd, buf, count); }
int _read(int fd, void *buf, size_t count) { return read(fd, buf, count); }
int _close(int fd) { return close(fd); }
int _open(const char *path, int flags, ...) { return open(path, flags); }
off_t _lseek(int fd, off_t offset, int whence) { return lseek(fd, offset, whence); }
int _fstat(int fd, void *st) { return fstat(fd, st); }
int _isatty(int fd) { return isatty(fd); }
int _kill(int pid, int sig) { return kill(pid, sig); }
int _getpid(void) { return getpid(); }
