#include <stdint.h>
#include <stddef.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_CLOSE 3
#define STDOUT 1
#define STDERR 2
#define O_CREAT 0x40
#define O_WRONLY 0x1

static inline uint64_t syscall1(uint64_t num, uint64_t a1) { uint64_t ret; asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory"); return ret; }
static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) { uint64_t ret; asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory"); return ret; }
static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { syscall3(SYSCALL_WRITE, fd, (uintptr_t)s, slen(s)); }
int main(int argc, char **argv) {
    if (argc < 2) { out(STDERR, "usage: touch <path>...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = (int)syscall3(SYSCALL_OPEN, (uintptr_t)argv[i], O_CREAT | O_WRONLY, 0644);
        if (fd < 0) { out(STDERR, "touch: failed: "); out(STDERR, argv[i]); out(STDERR, "\n"); rc = 1; }
        else syscall1(SYSCALL_CLOSE, fd);
    }
    return rc;
}
