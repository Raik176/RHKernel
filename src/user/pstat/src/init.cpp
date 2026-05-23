#include <stdint.h>
#include <stddef.h>
#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_READ 2
#define SYSCALL_CLOSE 3
#define SYSCALL_READDIR 19
#define STDOUT 1
#define STDERR 2

struct dirent { uint32_t inode; uint32_t type; uint64_t name_len;
    char *name;
    uint64_t name_capacity; };

static inline uint64_t sc1(uint64_t n, uint64_t a) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory"); return r; }
static inline uint64_t sc3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r; }
static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { sc3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, slen(s)); }
static void write_bytes(const char *s, size_t n) { sc3(SYSCALL_WRITE, STDOUT, (uintptr_t)s, n); }

static void copy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void append(char *dst, size_t cap, const char *src) {
    size_t n = slen(dst);
    size_t i = 0;
    while (src && src[i] && n + i + 1 < cap) { dst[n + i] = src[i]; i++; }
    dst[n + i] = 0;
}

static int parse_u64(const char *s) {
    if (!s || !*s) return 0;
    while (*s) { if (*s < '0' || *s > '9') return 0; s++; }
    return 1;
}

static int dump_file(const char *path) {
    int fd = (int)sc3(SYSCALL_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) return -1;
    char buf[1024];
    for (;;) {
        int64_t n = (int64_t)sc3(SYSCALL_READ, (uint64_t)fd, (uintptr_t)buf, sizeof(buf));
        if (n < 0) { sc1(SYSCALL_CLOSE, (uint64_t)fd); return -1; }
        if (n == 0) break;
        write_bytes(buf, (size_t)n);
    }
    sc1(SYSCALL_CLOSE, (uint64_t)fd);
    return 0;
}

static int dump_pid(const char *pid) {
    char path[320];
    copy(path, sizeof(path), "/proc/tasks/");
    append(path, sizeof(path), pid);
    append(path, sizeof(path), "/status");
    if (dump_file(path) != 0) return -1;
    copy(path, sizeof(path), "/proc/tasks/");
    append(path, sizeof(path), pid);
    append(path, sizeof(path), "/maps");
    out(STDOUT, "maps:\n");
    if (dump_file(path) != 0) return -1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 2) { out(STDERR, "usage: pstat [pid]\n"); return 1; }
    if (argc == 2) {
        if (!parse_u64(argv[1]) || dump_pid(argv[1]) != 0) { out(STDERR, "pstat: pid not found\n"); return 1; }
        return 0;
    }
    for (uint64_t i = 0;; i++) {
        dirent de;
        char namebuf[64];
        de.name = namebuf;
        de.name_capacity = sizeof(namebuf);
        int64_t r = (int64_t)sc3(SYSCALL_READDIR, (uintptr_t)"/proc/tasks", i, (uintptr_t)&de);
        if (r < 0) break;
        dump_pid(de.name);
        out(STDOUT, "\n");
    }
    return 0;
}
