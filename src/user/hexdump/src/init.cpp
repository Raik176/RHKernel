#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void out(int fd, const char *s) { write(fd, s, strlen(s)); }
static uint64_t parse_u64(const char *s, uint64_t def) { return s && *s ? strtoul(s, 0, 10) : def; }
static void outc(char c) { write(STDOUT_FILENO, &c, 1); }
static void hex8(uint8_t v) { const char *h = "0123456789abcdef"; char b[2] = {h[v >> 4], h[v & 15]}; write(STDOUT_FILENO, b, 2); }
static void hex64(uint64_t v) { const char *h = "0123456789abcdef"; char b[16]; for (int i = 15; i >= 0; i--) { b[i] = h[v & 15]; v >>= 4; } write(STDOUT_FILENO, b, 16); }

int main(int argc, char **argv) {
    if (argc < 2) { out(STDERR_FILENO, "usage: hexdump <path> [bytes]\n"); return 1; }
    uint64_t limit = argc >= 3 ? parse_u64(argv[2], 512) : 512;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { out(STDERR_FILENO, "hexdump: open failed\n"); return 1; }
    uint8_t buf[16];
    uint64_t done = 0;
    while (done < limit) {
        uint64_t want = limit - done;
        if (want > 16) want = 16;
        ssize_t n = read(fd, buf, (size_t)want);
        if (n <= 0) break;
        hex64(done); out(STDOUT_FILENO, "  ");
        for (ssize_t i = 0; i < n; i++) { hex8(buf[i]); outc(i == n - 1 ? '\n' : ' '); }
        done += (uint64_t)n;
    }
    close(fd);
    return 0;
}
