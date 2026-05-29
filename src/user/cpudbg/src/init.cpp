#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

static void out(int fd, const char *s) { write(fd, s, strlen(s)); }

static int dump_file(const char *path, int required) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (required) { out(STDERR_FILENO, "cpudbg: cannot open "); out(STDERR_FILENO, path); out(STDERR_FILENO, "\n"); }
        return -1;
    }
    char buf[1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) { out(STDERR_FILENO, "cpudbg: read failed\n"); close(fd); return -1; }
        if (n == 0) break;
        write(STDOUT_FILENO, buf, (size_t)n);
    }
    close(fd);
    return 0;
}

int main(int, char **) {
    out(STDOUT_FILENO, "cpu_count:\n");
    if (dump_file("/proc/cpu/count", 1) != 0) return 1;
    out(STDOUT_FILENO, "vendor:\n");
    if (dump_file("/proc/cpu/vendor", 1) != 0) return 1;
    out(STDOUT_FILENO, "features:\n");
    if (dump_file("/proc/cpu/features", 1) != 0) return 1;
    return 0;
}
