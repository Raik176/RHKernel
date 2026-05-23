#include <stdint.h>
#include <stddef.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_READ 2
#define SYSCALL_CLOSE 3
#define SYSCALL_YIELD 4
#define SYSCALL_SLEEP 5
#define SYSCALL_EXIT 6
#define SYSCALL_WAIT 7
#define SYSCALL_DUP2 8
#define SYSCALL_FORK 10
#define SYSCALL_EXEC 11
#define SYSCALL_GETPID 12
#define SYSCALL_MMAP 13
#define SYSCALL_MUNMAP 14
#define SYSCALL_BRK 15
#define SYSCALL_CREATE 16
#define SYSCALL_UNLINK 17
#define SYSCALL_RENAME 18
#define SYSCALL_READDIR 19
#define SYSCALL_CHDIR 20
#define SYSCALL_GETCWD 21

#define STDOUT 1
#define STDERR 2
#define O_WRONLY 0x1
#define O_RDWR 0x2
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

struct dirent64 { uint32_t inode; uint32_t type; uint64_t name_len;
    char *name;
    uint64_t name_capacity; };

static uint64_t sc0(uint64_t n) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(0ULL), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory"); return r; }
static uint64_t sc1(uint64_t n, uint64_t a) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory"); return r; }
static uint64_t sc3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r; }
static uint64_t sc4(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d) { uint64_t r; register uint64_t r10 asm("r10") = d; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory"); return r; }
static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static int streq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void scopy(char *dst, size_t cap, const char *src) { size_t i = 0; if (!cap) return; while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; } dst[i] = 0; }
static void sappend(char *dst, size_t cap, const char *src) { size_t n = slen(dst), i = 0; if (n >= cap) return; while (src && src[i] && n + i + 1 < cap) { dst[n + i] = src[i]; i++; } dst[n + i] = 0; }
static void wr(int fd, const char *s) { sc3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, slen(s)); }
static void out(const char *s) { wr(STDOUT, s); }
static void err(const char *s) { wr(STDERR, s); }
static void dec(uint64_t v) { char b[32]; int n=0; if (!v) b[n++]='0'; else { char t[32]; int m=0; while (v) { t[m++]=(char)('0'+v%10); v/=10; } while (m) b[n++]=t[--m]; } sc3(SYSCALL_WRITE, STDOUT, (uintptr_t)b, (uint64_t)n); }
static int fails;
static void pass(const char *name) { out("[PASS] "); out(name); out("\n"); }
static void fail(const char *name) { fails++; out("[FAIL] "); out(name); out("\n"); }
static void expect(const char *name, int ok) { if (ok) pass(name); else fail(name); }

static int has_entry(const char *dir, const char *name) {
    struct dirent64 de;
    char namebuf[512];

    for (uint64_t i = 0; i < 256; i++) {
        de.name = namebuf;
        de.name_capacity = sizeof(namebuf);
        int64_t r = (int64_t)sc3(SYSCALL_READDIR, (uintptr_t)dir, i, (uintptr_t)&de);
        if (r == -1) return 0;
        if (r < 0) continue;
        if (streq(de.name, name)) return 1;
    }
    return 0;
}

int main(void) {
    int fd = (int)sc3(SYSCALL_OPEN, (uintptr_t)"/bin/test/fs", 0, 0);
    expect("open existing initramfs file", fd >= 0);
    if (fd >= 0) {
        unsigned char magic[4] = {0};
        int64_t n = (int64_t)sc3(SYSCALL_READ, (uint64_t)fd, (uintptr_t)magic, sizeof(magic));
        expect("read exact ELF magic length", n == 4);
        expect("ELF magic matches", magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F');
        expect("close existing file", (int64_t)sc1(SYSCALL_CLOSE, (uint64_t)fd) == 0);
    }
    expect("open missing path fails", (int64_t)sc3(SYSCALL_OPEN, (uintptr_t)"/does/not/exist", 0, 0) == -1);
    expect("readdir root finds bin", has_entry("/", "bin"));
    expect("readdir /bin finds test directory", has_entry("/bin", "test"));
    expect("readdir /bin/test finds fs", has_entry("/bin/test", "fs"));
    struct dirent64 de;
    char readdir_name[32];
    de.name = readdir_name;
    de.name_capacity = sizeof(readdir_name);
    expect("readdir missing directory fails", (int64_t)sc3(SYSCALL_READDIR, (uintptr_t)"/missing", 0, (uintptr_t)&de) == -1);

    char long_name[301];
    for (size_t i = 0; i < sizeof(long_name) - 1; i++) long_name[i] = (char)('a' + (i % 26));
    long_name[sizeof(long_name) - 1] = 0;
    char long_path[304];
    scopy(long_path, sizeof(long_path), "/");
    sappend(long_path, sizeof(long_path), long_name);
    expect("create long VFS name", (int64_t)sc1(SYSCALL_CREATE, (uintptr_t)long_path) == 0);
    uint64_t long_index = 0xffffffffffffffffULL;
    char tiny_name[8];
    for (uint64_t i = 0; i < 512; i++) {
        de.name = tiny_name;
        de.name_capacity = sizeof(tiny_name);
        int64_t r = (int64_t)sc3(SYSCALL_READDIR, (uintptr_t)"/", i, (uintptr_t)&de);
        if (r == -1) break;
        if (de.name_len == sizeof(long_name) - 1) { long_index = i; break; }
    }
    expect("readdir reports long name length", long_index != 0xffffffffffffffffULL);
    if (long_index != 0xffffffffffffffffULL) {
        char long_buf[sizeof(long_name)];
        de.name = long_buf;
        de.name_capacity = sizeof(long_buf);
        expect("readdir copies long name", (int64_t)sc3(SYSCALL_READDIR, (uintptr_t)"/", long_index, (uintptr_t)&de) == 0 && streq(de.name, long_name));
    }
    expect("unlink long VFS name", (int64_t)sc1(SYSCALL_UNLINK, (uintptr_t)long_path) == 0);

    char cwd[64];
    expect("getcwd succeeds", (int64_t)sc3(SYSCALL_GETCWD, (uintptr_t)cwd, sizeof(cwd), 0) == 0);
    expect("absolute chdir succeeds", (int64_t)sc1(SYSCALL_CHDIR, (uintptr_t)"/bin/test") == 0);
    expect("getcwd after absolute chdir", (int64_t)sc3(SYSCALL_GETCWD, (uintptr_t)cwd, sizeof(cwd), 0) == 0 && streq(cwd, "/bin/test"));
    fd = (int)sc3(SYSCALL_OPEN, (uintptr_t)"fs", 0, 0);
    expect("open relative to cwd", fd >= 0);
    if (fd >= 0) expect("close relative file", (int64_t)sc1(SYSCALL_CLOSE, (uint64_t)fd) == 0);
    expect("dot resolves in cwd", has_entry(".", "fs"));
    expect("parent relative chdir succeeds", (int64_t)sc1(SYSCALL_CHDIR, (uintptr_t)"../..") == 0);
    expect("getcwd after parent chdir", (int64_t)sc3(SYSCALL_GETCWD, (uintptr_t)cwd, sizeof(cwd), 0) == 0 && streq(cwd, "/bin"));
    expect("relative chdir from /bin succeeds", (int64_t)sc1(SYSCALL_CHDIR, (uintptr_t)"test") == 0);
    expect("getcwd after relative chdir", (int64_t)sc3(SYSCALL_GETCWD, (uintptr_t)cwd, sizeof(cwd), 0) == 0 && streq(cwd, "/bin/test"));
    expect("getcwd too-small buffer fails", (int64_t)sc3(SYSCALL_GETCWD, (uintptr_t)cwd, 1, 0) == -1);
    expect("chdir file fails", (int64_t)sc1(SYSCALL_CHDIR, (uintptr_t)"/bin/test/fs") == -1);
    if (fails) { out("fs: FAIL "); dec((uint64_t)fails); out(" failure(s)\n"); return 1; }
    out("fs: PASS\n");
    return 0;
}
