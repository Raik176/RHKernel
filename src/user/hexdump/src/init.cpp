#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_READ 2
#define SYSCALL_CLOSE 3
#define STDOUT 1
#define STDERR 2

static inline uint64_t syscall1(uint64_t num, uint64_t a1) { uint64_t ret; asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory"); return ret; }
static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) { uint64_t ret; asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory"); return ret; }
static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { syscall3(SYSCALL_WRITE, fd, (uintptr_t)s, slen(s)); }
static uint64_t parse_u64(const char *s, uint64_t def) { if (!s || !*s) return def; uint64_t v = 0; int any = 0; while (*s >= '0' && *s <= '9') { any = 1; v = v * 10 + (uint64_t)(*s - '0'); s++; } return any ? v : def; }
static void outc(char c) { syscall3(SYSCALL_WRITE, STDOUT, (uintptr_t)&c, 1); }
static void hex8(uint8_t v) { const char *h = "0123456789abcdef"; char b[2] = {h[v >> 4], h[v & 15]}; syscall3(SYSCALL_WRITE, STDOUT, (uintptr_t)b, 2); }
static void hex64(uint64_t v) { const char *h = "0123456789abcdef"; char b[16]; for (int i = 15; i >= 0; i--) { b[i] = h[v & 15]; v >>= 4; } syscall3(SYSCALL_WRITE, STDOUT, (uintptr_t)b, 16); }
int main(int argc, char **argv) {
    if (argc < 2) { out(STDERR, "usage: hexdump <path> [bytes]\n"); return 1; }
    uint64_t limit = argc >= 3 ? parse_u64(argv[2], 512) : 512;
    int fd = (int)syscall3(SYSCALL_OPEN, (uintptr_t)argv[1], 0, 0);
    if (fd < 0) { out(STDERR, "hexdump: open failed\n"); return 1; }
    uint8_t buf[16];
    uint64_t done = 0;
    while (done < limit) {
        uint64_t want = limit - done; if (want > 16) want = 16;
        int64_t n = (int64_t)syscall3(SYSCALL_READ, fd, (uintptr_t)buf, want);
        if (n <= 0) break;
        hex64(done); out(STDOUT, "  ");
        for (int64_t i = 0; i < n; i++) { hex8(buf[i]); outc(i == n - 1 ? '\n' : ' '); }
        done += (uint64_t)n;
    }
    syscall1(SYSCALL_CLOSE, fd); return 0;
}
