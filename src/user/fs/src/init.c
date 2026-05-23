#include <stdint.h>
#include <stddef.h>
#include <unistd.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_READ 2
#define SYSCALL_CLOSE 3
#define SYSCALL_FSCTL 22

#define STDOUT 1
#define STDERR 2

#define FS_CTL_MOUNT 1
#define FS_CTL_UNMOUNT 2
#define FS_CTL_PROBE 3

#define FS_PROBE_NO 0
#define FS_PROBE_YES 1
#define FS_PROBE_ERR -1
#define FS_PROBE_UNSUPPORTED -2

struct fsctl_args {
    const char *source;
    const char *target;
    const char *fstype;
    const char *flags;
};

static uint64_t sc1(uint64_t n, uint64_t a) {
    uint64_t r;
    asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory");
    return r;
}

static uint64_t sc2(uint64_t n, uint64_t a, uint64_t b) {
    uint64_t r;
    asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(0ULL) : "rcx", "r11", "memory");
    return r;
}

static uint64_t sc3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t r;
    asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static size_t slen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void wr(int fd, const char *s) { sc3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, slen(s)); }
static void out(const char *s) { wr(STDOUT, s); }
static void err(const char *s) { wr(STDERR, s); }

static void usage(void) {
    err("usage:\n");
    err("  fs mount [-t type] [-o flags] <source> <target>\n");
    err("  fs umount <target>\n");
    err("  fs probe [-t type] <source>\n");
    err("  fs mounts\n");
    err("  fs types\n");
}

static int cat_file(const char *path) {
    int fd = (int)sc3(SYSCALL_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) return -1;
    char buf[512];
    for (;;) {
        int64_t n = (int64_t)sc3(SYSCALL_READ, (uint64_t)fd, (uintptr_t)buf, sizeof(buf));
        if (n < 0) { sc1(SYSCALL_CLOSE, (uint64_t)fd); return -1; }
        if (n == 0) break;
        sc3(SYSCALL_WRITE, STDOUT, (uintptr_t)buf, (uint64_t)n);
    }
    return (int)sc1(SYSCALL_CLOSE, (uint64_t)fd);
}

static int fsctl(int op, struct fsctl_args *args) {
    return (int)sc2(SYSCALL_FSCTL, (uint64_t)op, (uintptr_t)args);
}

static int cmd_mount(int argc, char **argv) {
    const char *type = 0;
    const char *flags = 0;
    int i = 2;
    while (i < argc && argv[i][0] == '-') {
        if (streq(argv[i], "-t") && i + 1 < argc) { type = argv[i + 1]; i += 2; continue; }
        if (streq(argv[i], "-o") && i + 1 < argc) { flags = argv[i + 1]; i += 2; continue; }
        usage();
        return 2;
    }
    if (argc - i != 2) { usage(); return 2; }
    struct fsctl_args args = { argv[i], argv[i + 1], type, flags };
    if (fsctl(FS_CTL_MOUNT, &args) == 0) return 0;
    err("fs: mount failed\n");
    return 1;
}

static int cmd_umount(int argc, char **argv) {
    if (argc != 3) { usage(); return 2; }
    struct fsctl_args args = { 0, argv[2], 0, 0 };
    if (fsctl(FS_CTL_UNMOUNT, &args) == 0) return 0;
    err("fs: umount failed\n");
    return 1;
}

static int cmd_probe(int argc, char **argv) {
    const char *type = 0;
    int i = 2;
    if (i < argc && streq(argv[i], "-t")) {
        if (i + 1 >= argc) { usage(); return 2; }
        type = argv[i + 1];
        i += 2;
    }
    if (argc - i != 1) { usage(); return 2; }
    struct fsctl_args args = { argv[i], 0, type, 0 };
    int r = fsctl(FS_CTL_PROBE, &args);
    if (r == FS_PROBE_YES) { out("yes\n"); return 0; }
    if (r == FS_PROBE_NO) { out("no\n"); return 1; }
    if (r == FS_PROBE_UNSUPPORTED) { out("unsupported\n"); return 3; }
    out("error\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    if (streq(argv[1], "mount")) return cmd_mount(argc, argv);
    if (streq(argv[1], "umount") || streq(argv[1], "unmount")) return cmd_umount(argc, argv);
    if (streq(argv[1], "probe")) return cmd_probe(argc, argv);
    if (streq(argv[1], "mounts")) return cat_file("/proc/mounts") == 0 ? 0 : 1;
    if (streq(argv[1], "types") || streq(argv[1], "filesystems")) return cat_file("/proc/filesystems") == 0 ? 0 : 1;
    usage();
    return 2;
}
