#include <stddef.h>
#include <stdint.h>

#define SYSCALL_OPEN 1
#define SYSCALL_READ 2
#define SYSCALL_CLOSE 3
#define SYSCALL_WRITE 0
#define STDOUT 1
#define STDERR 2

static uint64_t sc1(uint64_t n, uint64_t a) {
    uint64_t r;
    asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory");
    return r;
}

static uint64_t sc3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t r;
    asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static size_t slen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void out(int fd, const char *s) { sc3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, slen(s)); }

static int contains(const char *buf, size_t len, const char *needle) {
    size_t nlen = slen(needle);
    if (nlen == 0 || nlen > len) return 0;
    for (size_t i = 0; i <= len - nlen; i++) {
        size_t j = 0;
        while (j < nlen && buf[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

int main(void) {
    int fd = (int)sc3(SYSCALL_OPEN, (uintptr_t)"/proc/cpu/debug", 0, 0);
    if (fd < 0) {
        out(STDERR, "cpufeatures: cannot open /proc/cpu/debug\n");
        return 1;
    }

    char buf[4096];
    size_t used = 0;
    for (;;) {
        if (used == sizeof(buf)) break;
        int64_t n = (int64_t)sc3(SYSCALL_READ, (uint64_t)fd, (uintptr_t)buf + used, sizeof(buf) - used);
        if (n < 0) {
            sc1(SYSCALL_CLOSE, (uint64_t)fd);
            out(STDERR, "cpufeatures: read failed\n");
            return 1;
        }
        if (n == 0) break;
        used += (size_t)n;
    }
    sc1(SYSCALL_CLOSE, (uint64_t)fd);

    if (!contains(buf, used, "features=pge:") || !contains(buf, used, " smep:") ||
        !contains(buf, used, " smap:") || !contains(buf, used, " umip:") ||
        !contains(buf, used, " pat:") || !contains(buf, used, " wc:") ||
        !contains(buf, used, " xsave:") || !contains(buf, used, " avx:") ||
        !contains(buf, used, " fsgsbase_supported:") ||
        !contains(buf, used, " fsgsbase_enabled:")) {
        out(STDERR, "cpufeatures: missing feature fields\n");
        return 1;
    }

    out(STDOUT, "cpufeatures: ok\n");
    return 0;
}
