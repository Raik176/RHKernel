#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

struct dirent { uint32_t inode; uint32_t type; uint64_t name_len;
    char *name;
    uint64_t name_capacity; };

static void out(int fd, const char *s) { write(fd, s, strlen(s)); }
static void write_bytes(const char *s, size_t n) { write(STDOUT_FILENO, s, n); }

static void copy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void append(char *dst, size_t cap, const char *src) {
    size_t n = strlen(dst);
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
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        write_bytes(buf, (size_t)n);
    }
    close(fd);
    return 0;
}

static int dump_pid(const char *pid) {
    char path[320];
    copy(path, sizeof(path), "/sys/kernel/debug/scheduler/tasks/");
    append(path, sizeof(path), pid);
    append(path, sizeof(path), "/status");
    if (dump_file(path) != 0) return -1;
    copy(path, sizeof(path), "/sys/kernel/debug/scheduler/tasks/");
    append(path, sizeof(path), pid);
    append(path, sizeof(path), "/maps");
    out(STDOUT_FILENO, "maps:\n");
    if (dump_file(path) != 0) return -1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 2) { out(STDERR_FILENO, "usage: pstat [pid]\n"); return 1; }
    if (argc == 2) {
        if (!parse_u64(argv[1]) || dump_pid(argv[1]) != 0) { out(STDERR_FILENO, "pstat: pid not found\n"); return 1; }
        return 0;
    }
    for (uint64_t i = 0;; i++) {
        dirent de;
        char namebuf[64];
        de.name = namebuf;
        de.name_capacity = sizeof(namebuf);
        int64_t r = (int64_t)readdir("/sys/kernel/debug/scheduler/tasks", i, &de);
        if (r < 0) break;
        dump_pid(de.name);
        out(STDOUT_FILENO, "\n");
    }
    return 0;
}
