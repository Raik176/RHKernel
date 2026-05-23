#include <stdint.h>
#include <stddef.h>

#define SYSCALL_WRITE 0
#define SYSCALL_EXIT 6
#define SYSCALL_WAIT 7
#define SYSCALL_FORK 10
#define SYSCALL_MMAP 13
#define SYSCALL_MUNMAP 14
#define SYSCALL_BRK 15

#define STDOUT 1
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

static uint64_t sc0(uint64_t n) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(0ULL), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory"); return r; }
static uint64_t sc1(uint64_t n, uint64_t a) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory"); return r; }
static uint64_t sc3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) { uint64_t r; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r; }
static uint64_t sc4(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d) { uint64_t r; register uint64_t r10 asm("r10") = d; asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory"); return r; }
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sc3(SYSCALL_WRITE, STDOUT, (uintptr_t)s, slen(s)); }
static void dec(uint64_t v) { char b[32]; int n = 0; if (!v) b[n++] = '0'; else { char t[32]; int m = 0; while (v) { t[m++] = (char)('0' + v % 10); v /= 10; } while (m) b[n++] = t[--m]; } sc3(SYSCALL_WRITE, STDOUT, (uintptr_t)b, (uint64_t)n); }

static int fails;
static void expect(const char *name, int ok) { out(ok ? "[PASS] " : "[FAIL] "); out(name); out("\n"); if (!ok) fails++; }

int main(void) {
    unsigned char *a = (unsigned char *)sc4(SYSCALL_MMAP, 0, 4 * 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS);
    expect("initial anonymous VMA", a != MAP_FAILED);
    if (a != MAP_FAILED) {
        void *overlap = (void *)sc4(SYSCALL_MMAP, (uintptr_t)(a + 4096), 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_FIXED);
        expect("fixed mmap refuses VMA overlap", overlap == MAP_FAILED);
        expect("middle munmap splits VMA", (int64_t)sc3(SYSCALL_MUNMAP, (uintptr_t)(a + 4096), 2 * 4096, 0) == 0);
        a[0] = 0x31;
        a[3 * 4096] = 0x32;
        expect("split VMA preserves left and right", a[0] == 0x31 && a[3 * 4096] == 0x32);
        expect("left munmap succeeds", (int64_t)sc3(SYSCALL_MUNMAP, (uintptr_t)a, 4096, 0) == 0);
        expect("right munmap succeeds", (int64_t)sc3(SYSCALL_MUNMAP, (uintptr_t)(a + 3 * 4096), 4096, 0) == 0);
    }

    uint64_t brk0 = sc1(SYSCALL_BRK, 0);
    uint64_t brk1 = sc1(SYSCALL_BRK, brk0 + 3 * 4096);
    expect("brk grows", brk1 == brk0 + 3 * 4096);
    if (brk1 == brk0 + 3 * 4096) {
        unsigned char *p = (unsigned char *)brk0;
        p[0] = 0x44;
        p[2 * 4096] = 0x55;
        expect("heap VMA stores data", p[0] == 0x44 && p[2 * 4096] == 0x55);
        uint64_t brk2 = sc1(SYSCALL_BRK, brk0 + 4096);
        expect("brk shrinks", brk2 == brk0 + 4096);
        uint64_t brk3 = sc1(SYSCALL_BRK, brk0 + 3 * 4096);
        expect("brk regrows after shrink", brk3 == brk0 + 3 * 4096);
    }

    uint64_t pid = sc0(SYSCALL_FORK);
    if (pid == 0) {
        volatile unsigned char stack_probe[64];
        stack_probe[0] = 0xA5;
        stack_probe[63] = 0x5A;
        sc1(SYSCALL_EXIT, (stack_probe[0] == 0xA5 && stack_probe[63] == 0x5A) ? 0 : 2);
    }
    int status = -1;
    int waited = pid > 0 ? (int)sc1(SYSCALL_WAIT, (uintptr_t)&status) : -1;
    expect("fork child stack remains writable", pid > 0 && waited == (int)pid && status == 0);

    if (fails) { out("vma: FAIL "); dec((uint64_t)fails); out(" failure(s)\n"); return 1; }
    out("vma: PASS\n");
    return 0;
}
