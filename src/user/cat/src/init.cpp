#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void out(int fd, const char *s) { write(fd, s, strlen(s)); }

static int cat_one(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { out(STDERR_FILENO, "cat: open failed: "); out(STDERR_FILENO, path); out(STDERR_FILENO, "\n"); return 1; }
    char *buf = (char *)malloc(4096);
    if (!buf) { out(STDERR_FILENO, "cat: out of memory\n"); close(fd); return 1; }
    for (;;) {
        ssize_t n = read(fd, buf, 4096);
        if (n <= 0) break;
        write(STDOUT_FILENO, buf, (size_t)n);
    }
    free(buf);
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { out(STDERR_FILENO, "usage: cat <path>...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) if (cat_one(argv[i]) != 0) rc = 1;
    return rc;
}
