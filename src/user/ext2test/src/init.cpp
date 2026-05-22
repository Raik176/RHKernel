#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_READ 2
#define SYSCALL_CLOSE 3
#define SYSCALL_UNLINK 17
#define SYSCALL_RENAME 18
#define SYSCALL_READDIR 19

#define STDOUT 1
#define STDERR 2
#define O_WRONLY 0x1
#define O_RDWR 0x2
#define O_CREAT 0x40
#define O_TRUNC 0x200

struct dirent64 {
    uint32_t inode;
    uint32_t type;
    char name[256];
};

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
static int streq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void out_fd(int fd, const char *s) { syscall3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, slen(s)); }
static void out(const char *s) { out_fd(STDOUT, s); }
static void err(const char *s) { out_fd(STDERR, s); }

static void write_dec(uint64_t v) {
    char buf[32];
    int pos = 0;
    if (v == 0) buf[pos++] = '0';
    else {
        char tmp[32]; int n = 0;
        while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
        while (n) buf[pos++] = tmp[--n];
    }
    syscall3(SYSCALL_WRITE, STDOUT, (uintptr_t)buf, (uint64_t)pos);
}

static char *join2(const char *dir, const char *name) {
    size_t dl = slen(dir), nl = slen(name);
    int need_slash = dl > 0 && dir[dl - 1] != '/';
    char *outp = (char *)malloc(dl + (size_t)need_slash + nl + 1);
    if (!outp) return nullptr;
    size_t p = 0;
    for (size_t i = 0; i < dl; ++i) outp[p++] = dir[i];
    if (need_slash) outp[p++] = '/';
    for (size_t i = 0; i < nl; ++i) outp[p++] = name[i];
    outp[p] = 0;
    return outp;
}

static int fail(const char *what, const char *path) {
    err("ext2test: FAIL: ");
    err(what);
    if (path) { err(": "); err(path); }
    err("\n");
    return 1;
}

static int write_file(const char *path, const char *data) {
    int fd = (int)syscall3(SYSCALL_OPEN, (uintptr_t)path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return fail("open for write", path);
    size_t len = slen(data);
    int64_t n = (int64_t)syscall3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)data, len);
    syscall1(SYSCALL_CLOSE, (uint64_t)fd);
    if (n != (int64_t)len) return fail("short write", path);
    return 0;
}

static int read_verify(const char *path, const char *expected) {
    int fd = (int)syscall3(SYSCALL_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) return fail("open for read", path);
    char buf[256];
    int64_t n = (int64_t)syscall3(SYSCALL_READ, (uint64_t)fd, (uintptr_t)buf, sizeof(buf) - 1);
    syscall1(SYSCALL_CLOSE, (uint64_t)fd);
    if (n < 0) return fail("read", path);
    buf[n] = 0;
    if (!streq(buf, expected)) {
        err("ext2test: FAIL: content mismatch: "); err(path); err("\n");
        err("  expected: "); err(expected); err("\n");
        err("  got:      "); err(buf); err("\n");
        return 1;
    }
    return 0;
}

static int exists_in_dir(const char *dir, const char *name) {
    struct dirent64 de;
    for (uint64_t i = 0;; ++i) {
        int64_t r = (int64_t)syscall3(SYSCALL_READDIR, (uintptr_t)dir, i, (uintptr_t)&de);
        if (r < 0) return 0;
        if (streq(de.name, name)) return 1;
    }
}

static int one_iteration(const char *root, int iter) {
    char n1[32], n2[32];
    // Keep names short enough for ext2 and readable in shell output.
    n1[0] = 'e'; n1[1] = '2'; n1[2] = 't'; n1[3] = '_';
    n2[0] = 'e'; n2[1] = '2'; n2[2] = 'r'; n2[3] = '_';
    int x = iter;
    for (int i = 0; i < 6; ++i) { n1[4 + i] = (char)('0' + (x % 10)); n2[4 + i] = n1[4 + i]; x /= 10; }
    n1[10] = '.'; n1[11] = 't'; n1[12] = 'x'; n1[13] = 't'; n1[14] = 0;
    n2[10] = '.'; n2[11] = 't'; n2[12] = 'x'; n2[13] = 't'; n2[14] = 0;

    char *p1 = join2(root, n1);
    char *p2 = join2(root, n2);
    if (!p1 || !p2) return fail("out of memory", nullptr);

    const char *payload = "abcdefghijklmnopqrstuvwxyz ext2 write/read test\n";

    // Clean up stale files from interrupted previous runs.
    syscall1(SYSCALL_UNLINK, (uintptr_t)p1);
    syscall1(SYSCALL_UNLINK, (uintptr_t)p2);

    if (write_file(p1, payload)) return 1;
    if (!exists_in_dir(root, n1)) return fail("created file not visible in readdir", p1);
    if (read_verify(p1, payload)) return 1;

    if ((int64_t)syscall3(SYSCALL_RENAME, (uintptr_t)p1, (uintptr_t)p2, 0) < 0) return fail("rename", p1);
    if (exists_in_dir(root, n1)) return fail("old name still visible after rename", p1);
    if (!exists_in_dir(root, n2)) return fail("new name missing after rename", p2);
    if (read_verify(p2, payload)) return 1;

    if ((int64_t)syscall1(SYSCALL_UNLINK, (uintptr_t)p2) < 0) return fail("unlink", p2);
    if (exists_in_dir(root, n2)) return fail("deleted file still visible", p2);

    free(p1);
    free(p2);
    return 0;
}

int main(int argc, char **argv) {
    const char *root = "/mnt/ext2";
    int count = 32;
    if (argc >= 2) root = argv[1];
    if (argc >= 3) {
        int v = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (*p - '0');
        if (v > 0) count = v;
    }

    out("ext2test: root="); out(root); out(" iterations="); write_dec((uint64_t)count); out("\n");
    for (int i = 0; i < count; ++i) {
        if (one_iteration(root, i) != 0) return 1;
        if ((i & 7) == 7) { out("."); }
    }
    out("\next2test: PASS\n");
    return 0;
}
