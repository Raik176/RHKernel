#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void out(int fd, const char *s) { write(fd, s, strlen(s)); }

int main() {
    int fd = open("/sys/devices/list", O_RDONLY);
    if (fd < 0) { out(STDERR_FILENO, "lsdev: open /sys/devices/list failed\n"); return 1; }
    char *buf = (char *)malloc(4096);
    if (!buf) { out(STDERR_FILENO, "lsdev: out of memory\n"); close(fd); return 1; }
    for (;;) {
        ssize_t n = read(fd, buf, 4096);
        if (n <= 0) break;
        write(STDOUT_FILENO, buf, (size_t)n);
    }
    free(buf);
    close(fd);
    return 0;
}
