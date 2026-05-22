#include <stdint.h>
#include <stddef.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_CLOSE 3

#define STDOUT 1
#define STDERR 2

static inline uint64_t syscall1(uint64_t num, uint64_t a1) {
    uint64_t ret;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { syscall3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, slen(s)); }

static int write_all(int fd, const char *s) {
    size_t len = slen(s);
    size_t done = 0;
    while (done < len) {
        uint64_t n = syscall3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)(s + done), len - done);
        if ((int64_t)n <= 0) return -1;
        done += n;
    }
    return 0;
}

int main(int, char **) {
    int fd = (int)syscall1(SYSCALL_OPEN, (uintptr_t)"/dev/power");
    if (fd < 0) {
        out(STDERR, "restart: cannot open /dev/power\n");
        return 1;
    }

    out(STDOUT, "restart: writing 'restart' to /dev/power\n");
    if (write_all(fd, "restart\n") != 0) {
        out(STDERR, "restart: /dev/power rejected request\n");
        syscall1(SYSCALL_CLOSE, (uint64_t)fd);
        return 1;
    }

    syscall1(SYSCALL_CLOSE, (uint64_t)fd);
    out(STDERR, "restart: request returned without rebooting\n");
    return 1;
}
