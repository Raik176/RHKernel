#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void wr(int fd, const char *s) { write(fd, s, strlen(s)); }
static int fail(const char *s) { wr(STDERR_FILENO, "exfat_rw: "); wr(STDERR_FILENO, s); wr(STDERR_FILENO, "\n"); return 1; }

static int read_text(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static int write_text(const char *path, const char *text, int flags) {
    int fd = open(path, flags, 0644);
    if (fd < 0) return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "/mnt/exfat";
    char a[256], b[256], buf[128];
    snprintf(a, sizeof(a), "%s/%s", root, "rw-create.txt");
    snprintf(b, sizeof(b), "%s/%s", root, "rw-renamed.txt");
    unlink(a);
    unlink(b);
    if (write_text(a, "alpha-data", O_CREAT | O_RDWR | O_TRUNC) != 0) return fail("create/write failed");
    if (read_text(a, buf, sizeof(buf)) <= 0 || strcmp(buf, "alpha-data") != 0) return fail("read after write failed");
    if (rename(a, b) != 0) return fail("rename failed");
    if (open(a, O_RDONLY) >= 0) return fail("old name still opens");
    if (write_text(b, "z", O_RDWR | O_TRUNC) != 0) return fail("truncate rewrite failed");
    if (read_text(b, buf, sizeof(buf)) <= 0 || strcmp(buf, "z") != 0) return fail("read after truncate failed");
    if (unlink(b) != 0) return fail("unlink failed");
    if (open(b, O_RDONLY) >= 0) return fail("deleted name still opens");
    wr(STDOUT_FILENO, "exfat_rw: ok\n");
    return 0;
}
