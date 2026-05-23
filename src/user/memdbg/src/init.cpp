#include <stdint.h>
#include <stddef.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_READ 2
#define SYSCALL_CLOSE 3
#define STDOUT 1
#define STDERR 2

static inline uint64_t sc1(uint64_t n, uint64_t a) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory"); return r; }
static inline uint64_t sc3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r; }
static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { sc3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, slen(s)); }

int main(int, char **) {
    int fd = (int)sc3(SYSCALL_OPEN, (uintptr_t)"/proc/mem/debug", 0, 0);
    if (fd < 0) { out(STDERR, "memdbg: cannot open /proc/mem/debug\n"); return 1; }
    char buf[1024];
    for (;;) {
        int64_t n = (int64_t)sc3(SYSCALL_READ, (uint64_t)fd, (uintptr_t)buf, sizeof(buf));
        if (n < 0) { out(STDERR, "memdbg: read failed\n"); sc1(SYSCALL_CLOSE, (uint64_t)fd); return 1; }
        if (n == 0) break;
        sc3(SYSCALL_WRITE, STDOUT, (uintptr_t)buf, (uint64_t)n);
    }
    sc1(SYSCALL_CLOSE, (uint64_t)fd);
    return 0;
}
