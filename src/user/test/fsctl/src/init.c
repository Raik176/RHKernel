#include <stdint.h>
#include <stddef.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_READ 2
#define SYSCALL_CLOSE 3
#define SYSCALL_FSCTL 22

#define STDOUT 1

#define FS_CTL_MOUNT 1
#define FS_CTL_UNMOUNT 2
#define FS_CTL_PROBE 3
#define FS_PROBE_ERR -1

struct fsctl_args {
    const char *source;
    const char *target;
    const char *fstype;
    const char *flags;
};

static uint64_t sc1(uint64_t n, uint64_t a) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory"); return r; }
static uint64_t sc2(uint64_t n, uint64_t a, uint64_t b) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(0ULL) : "rcx", "r11", "memory"); return r; }
static uint64_t sc3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r; }
static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(const char *s) { sc3(SYSCALL_WRITE, STDOUT, (uintptr_t)s, slen(s)); }
static int fails;
static void expect(const char *name, int ok) { out(ok ? "[PASS] " : "[FAIL] "); out(name); out("\n"); if (!ok) fails++; }

static int read_some(const char *path) {
    int fd = (int)sc3(SYSCALL_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) return -1;
    char buf[16];
    int64_t n = (int64_t)sc3(SYSCALL_READ, (uint64_t)fd, (uintptr_t)buf, sizeof(buf));
    int c = (int)sc1(SYSCALL_CLOSE, (uint64_t)fd);
    return n >= 0 && c == 0 ? 0 : -1;
}

int main(void) {
    struct fsctl_args empty = { 0, 0, 0, 0 };
    expect("fsctl rejects null mount paths", (int64_t)sc2(SYSCALL_FSCTL, FS_CTL_MOUNT, (uintptr_t)&empty) == -1);
    expect("fsctl rejects null unmount path", (int64_t)sc2(SYSCALL_FSCTL, FS_CTL_UNMOUNT, (uintptr_t)&empty) == -1);
    expect("fsctl probe missing source errors", (int64_t)sc2(SYSCALL_FSCTL, FS_CTL_PROBE, (uintptr_t)&empty) == FS_PROBE_ERR);
    expect("/proc/filesystems is readable", read_some("/proc/filesystems") == 0);
    expect("/proc/mounts is readable", read_some("/proc/mounts") == 0);
    if (fails) { out("fsctl: FAIL\n"); return 1; }
    out("fsctl: PASS\n");
    return 0;
}
