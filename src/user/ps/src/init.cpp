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

static void copy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void append(char *dst, size_t cap, const char *src) {
    size_t n = slen(dst);
    if (n >= cap) return;
    size_t i = 0;
    while (src && src[i] && n + i + 1 < cap) { dst[n + i] = src[i]; i++; }
    dst[n + i] = 0;
}

static int read_all(const char *path, char *buf, size_t cap, size_t *out_len) {
    int fd = (int)sc3(SYSCALL_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) return -1;
    size_t len = 0;
    while (len + 1 < cap) {
        int64_t n = (int64_t)sc3(SYSCALL_READ, (uint64_t)fd, (uintptr_t)(buf + len), cap - 1 - len);
        if (n < 0) { sc1(SYSCALL_CLOSE, (uint64_t)fd); return -1; }
        if (n == 0) break;
        len += (size_t)n;
    }
    sc1(SYSCALL_CLOSE, (uint64_t)fd);
    buf[len] = 0;
    if (out_len) *out_len = len;
    return 0;
}

static const char *field(const char *buf, const char *key, char *outv, size_t cap) {
    size_t key_len = slen(key);
    for (size_t i = 0; buf[i]; i++) {
        size_t j = 0;
        while (j < key_len && buf[i + j] == key[j]) j++;
        if (j != key_len) continue;
        const char *p = buf + i + key_len;
        size_t n = 0;
        while (p[n] && p[n] != '\n' && n + 1 < cap) { outv[n] = p[n]; n++; }
        outv[n] = 0;
        return outv;
    }
    copy(outv, cap, "?");
    return outv;
}

static void print_task(const char *pid) {
    char path[320];
    char buf[2048];
    char ppid[32], cpu[32], state[32], prio[32], quantum[32], type[32];
    copy(path, sizeof(path), "/proc/tasks/");
    append(path, sizeof(path), pid);
    append(path, sizeof(path), "/status");
    if (read_all(path, buf, sizeof(buf), nullptr) != 0) return;
    out(STDOUT, pid); out(STDOUT, " ");
    out(STDOUT, field(buf, "ppid: ", ppid, sizeof(ppid))); out(STDOUT, " ");
    out(STDOUT, field(buf, "cpu: ", cpu, sizeof(cpu))); out(STDOUT, " ");
    out(STDOUT, field(buf, "state: ", state, sizeof(state))); out(STDOUT, " ");
    out(STDOUT, field(buf, "priority: ", prio, sizeof(prio))); out(STDOUT, " ");
    out(STDOUT, field(buf, "quantum: ", quantum, sizeof(quantum))); out(STDOUT, " ");
    out(STDOUT, field(buf, "type: ", type, sizeof(type))); out(STDOUT, "\n");
}

int main(int, char **) {
    out(STDOUT, "pid ppid cpu state priority quantum type\n");
    for (uint64_t i = 0;; i++) {
        dirent de;
        char namebuf[64];
        de.name = namebuf;
        de.name_capacity = sizeof(namebuf);
        int64_t r = (int64_t)sc3(SYSCALL_READDIR, (uintptr_t)"/proc/tasks", i, (uintptr_t)&de);
        if (r < 0) break;
        print_task(de.name);
    }
    return 0;
}
