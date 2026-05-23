#include <stdint.h>
#include <stddef.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_READ 2
#define SYSCALL_CLOSE 3
#define SYSCALL_CREATE 16
#define SYSCALL_UNLINK 17
#define SYSCALL_RENAME 18
#define SYSCALL_READDIR 19

#define STDOUT 1
#define STDERR 2
#define VFS_NODE_DIRECTORY 2

struct dirent64 { uint32_t inode; uint32_t type; uint64_t name_len;
    char *name;
    uint64_t name_capacity; };

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

static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static int streq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void wr(int fd, const char *s) { sc3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, slen(s)); }
static void out(const char *s) { wr(STDOUT, s); }
static void err(const char *s) { wr(STDERR, s); }
static int fails;

static void dec(uint64_t v) {
    char b[32];
    int n = 0;
    if (!v) b[n++] = '0';
    else {
        char t[32];
        int m = 0;
        while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
        while (m) b[n++] = t[--m];
    }
    sc3(SYSCALL_WRITE, STDOUT, (uintptr_t)b, (uint64_t)n);
}

static void expect(const char *name, int ok) {
    if (ok) { out("[PASS] "); out(name); out("\n"); return; }
    fails++;
    out("[FAIL] "); out(name); out("\n");
}

static char *join(char *buf, size_t bufsz, const char *a, const char *b) {
    size_t p = 0;
    for (; a[p] && p + 1 < bufsz; p++) buf[p] = a[p];
    if (p && buf[p - 1] != '/' && p + 1 < bufsz) buf[p++] = '/';
    for (size_t i = 0; b[i] && p + 1 < bufsz; i++) buf[p++] = b[i];
    buf[p] = 0;
    return buf;
}

static int has_entry(const char *dir, const char *name, uint32_t type) {
    struct dirent64 de;
    char namebuf[512];

    for (uint64_t i = 0; i < 64; i++) {
        de.name = namebuf;
        de.name_capacity = sizeof(namebuf);
        int64_t r = (int64_t)sc3(SYSCALL_READDIR, (uintptr_t)dir, i, (uintptr_t)&de);
        if (r == -1) return 0;
        if (r < 0) continue;
        if (streq(de.name, name)) return type == 0 || de.type == type;
    }
    return 0;
}

static int read_all(const char *path, char *buf, size_t cap) {
    int fd = (int)sc3(SYSCALL_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) return -1;
    int64_t n = (int64_t)sc3(SYSCALL_READ, (uint64_t)fd, (uintptr_t)buf, cap - 1);
    sc1(SYSCALL_CLOSE, (uint64_t)fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static int contains(const char *haystack, const char *needle) {
    for (size_t i = 0; haystack[i]; i++) {
        size_t j = 0;
        while (needle[j] && haystack[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}


static int write_file(const char *path, const char *payload) {
    int fd = (int)sc3(SYSCALL_OPEN, (uintptr_t)path, 0x40 | 0x200, 0);
    if (fd < 0) return -1;
    int64_t n = (int64_t)sc3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)payload, slen(payload));
    sc1(SYSCALL_CLOSE, (uint64_t)fd);
    return n == (int64_t)slen(payload) ? 0 : -1;
}

static int rename_path(const char *oldp, const char *newp) {
    return (int64_t)sc3(SYSCALL_RENAME, (uintptr_t)oldp, (uintptr_t)newp, 0) < 0 ? -1 : 0;
}

static void cleanup(const char *path) { sc1(SYSCALL_UNLINK, (uintptr_t)path); }

static int run_one(const char *root, const char *kind) {
    char path[320];
    char buf[256];
    char long_name[80] = "Long File Name ";
    size_t p = slen(long_name);
    for (size_t i = 0; kind[i] && p + 5 < sizeof(long_name); i++) {
        char c = kind[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        long_name[p++] = c;
    }
    long_name[p++] = '.'; long_name[p++] = 't'; long_name[p++] = 'x'; long_name[p++] = 't'; long_name[p] = 0;

    out("vfattest: root="); out(root); out(" kind="); out(kind); out("\n");
    expect("root has README.TXT", has_entry(root, "README.TXT", 0));
    expect("root has LFN file", has_entry(root, long_name, 0));
    expect("root has LFN directory", has_entry(root, "Nested Directory", VFS_NODE_DIRECTORY));
    expect("README opens and reads", read_all(join(path, sizeof(path), root, "README.TXT"), buf, sizeof(buf)) > 0 && contains(buf, kind));
    expect("LFN file opens and reads", read_all(join(path, sizeof(path), root, long_name), buf, sizeof(buf)) > 0 && contains(buf, "long file name"));
    char nested[320];
    join(nested, sizeof(nested), root, "Nested Directory");
    expect("nested dir has child LFN", has_entry(nested, "Child Long Name.txt", 0));
    expect("nested child opens and reads", read_all(join(path, sizeof(path), nested, "Child Long Name.txt"), buf, sizeof(buf)) > 0 && contains(buf, "Nested VFAT"));

    char wrp[320], rnp[320], mvp[320];
    join(wrp, sizeof(wrp), root, "Created Long Name.txt");
    join(rnp, sizeof(rnp), root, "Renamed Long Name.txt");
    join(mvp, sizeof(mvp), nested, "Moved Long Name.txt");
    cleanup(wrp); cleanup(rnp); cleanup(mvp);
    expect("create/write LFN file", write_file(wrp, "vfat write path payload") == 0);
    expect("written file reads", read_all(wrp, buf, sizeof(buf)) > 0 && contains(buf, "write path"));
    expect("rename LFN file", rename_path(wrp, rnp) == 0 && has_entry(root, "Renamed Long Name.txt", 0));
    expect("move LFN file", rename_path(rnp, mvp) == 0 && has_entry(nested, "Moved Long Name.txt", 0));
    expect("moved file reads", read_all(mvp, buf, sizeof(buf)) > 0 && contains(buf, "write path"));
    cleanup(mvp);
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "/mnt/fat12";
    const char *kind = argc > 2 ? argv[2] : "fat12";
    int rc = run_one(root, kind);
    if (fails) { out("vfattest: FAIL "); dec((uint64_t)fails); out(" failure(s)\n"); return 1; }
    out("vfattest: PASS\n");
    return rc;
}
