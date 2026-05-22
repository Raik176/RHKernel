#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_READ 2
#define SYSCALL_CLOSE 3
#define STDOUT 1
#define STDERR 2

static inline uint64_t syscall1(uint64_t num, uint64_t a1) { uint64_t ret; asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory"); return ret; }
static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) { uint64_t ret; asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory"); return ret; }
static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { syscall3(SYSCALL_WRITE, fd, (uintptr_t)s, slen(s)); }
int main() {
    int fd = (int)syscall3(SYSCALL_OPEN, (uintptr_t)"/proc/devices", 0, 0);
    if (fd < 0) { out(STDERR, "lsdev: open /proc/devices failed\n"); return 1; }
    char *buf = (char *)malloc(4096);
    if (!buf) { out(STDERR, "lsdev: out of memory\n"); syscall1(SYSCALL_CLOSE, fd); return 1; }
    for (;;) { int64_t n = (int64_t)syscall3(SYSCALL_READ, fd, (uintptr_t)buf, 4096); if (n <= 0) break; syscall3(SYSCALL_WRITE, STDOUT, (uintptr_t)buf, (uint64_t)n); }
    free(buf); syscall1(SYSCALL_CLOSE, fd); return 0;
}
