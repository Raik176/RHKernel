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
#define SYSCALL_UNLINK 17
#define SYSCALL_RENAME 18
#define SYSCALL_READDIR 19

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
static void wr(int fd, const char *s) { sc3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, slen(s)); }
static void out(const char *s) { wr(STDOUT, s); }
static void err(const char *s) { wr(STDERR, s); }
static void dec(uint64_t v) { char b[32]; int n=0; if (!v) b[n++]='0'; else { char t[32]; int m=0; while (v) { t[m++]=(char)('0'+v%10); v/=10; } while (m) b[n++]=t[--m]; } sc3(SYSCALL_WRITE, STDOUT, (uintptr_t)b, (uint64_t)n); }
static int fails;
static void pass(const char *name) { out("[PASS] "); out(name); out("\n"); }
static void fail(const char *name) { fails++; out("[FAIL] "); out(name); out("\n"); }
static void expect(const char *name, int ok) { if (ok) pass(name); else fail(name); }

int main(void) {
    expect("getpid returns positive pid", (int64_t)sc0(SYSCALL_GETPID) > 0);
    expect("unknown syscall fails", (int64_t)sc0(0xffff) == -1);
    expect("yield succeeds", (int64_t)sc0(SYSCALL_YIELD) == 0);
    expect("sleep succeeds", (int64_t)sc1(SYSCALL_SLEEP, 1) == 0);
    expect("zero-length write succeeds", (int64_t)sc3(SYSCALL_WRITE, STDOUT, (uintptr_t)"", 0) == 0);
    expect("invalid read fd fails", (int64_t)sc3(SYSCALL_READ, 9999, (uintptr_t)&fails, 1) == -1);
    expect("invalid close fd fails", (int64_t)sc1(SYSCALL_CLOSE, 9999) == -1);
    expect("invalid dup2 fd fails", (int64_t)sc3(SYSCALL_DUP2, 9999, 4, 0) == -1);
    expect("wait without children fails", (int64_t)sc1(SYSCALL_WAIT, 0) == -1);
    uint64_t pid = sc0(SYSCALL_FORK);
    if (pid == 0) sc1(SYSCALL_EXIT, 42);
    if ((int64_t)pid < 0) fail("fork succeeds");
    else {
        int status = 0;
        int64_t waited = (int64_t)sc1(SYSCALL_WAIT, (uintptr_t)&status);
        expect("wait returns child pid", waited == (int64_t)pid);
        expect("child exit status is preserved", status == 42);
    }
    if (fails) { out("syscall: FAIL "); dec((uint64_t)fails); out(" failure(s)\n"); return 1; }
    out("syscall: PASS\n");
    return 0;
}
