#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct dirent64 {
    uint32_t inode;
    uint32_t type;
    uint64_t name_len;
    char *name;
    uint64_t name_capacity;
};

static void wr(int fd, const char *s) { write(fd, s, strlen(s)); }
static int fail(const char *s) { wr(STDERR_FILENO, "exfat_ro: "); wr(STDERR_FILENO, s); wr(STDERR_FILENO, "\n"); return 1; }

static int read_all(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static int dir_has(const char *path, const char *name) {
    char namebuf[256];
    struct dirent64 de;
    for (uint64_t i = 0;; i++) {
        memset(&de, 0, sizeof(de));
        if (readdir(path, i, &de) < 0) return 0;
        if (de.name_len >= sizeof(namebuf)) return 0;
        de.name = namebuf;
        de.name_capacity = sizeof(namebuf);
        if (readdir(path, i, &de) < 0) return 0;
        namebuf[de.name_len] = 0;
        if (strcmp(namebuf, name) == 0) return 1;
    }
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "/mnt/exfat";
    char path[256];
    char buf[128];
    if (!dir_has(root, "README.TXT")) return fail("README.TXT missing");
    snprintf(path, sizeof(path), "%s/%s", root, "readme.txt");
    if (read_all(path, buf, sizeof(buf)) <= 0) return fail("case-insensitive open failed");
    if (strcmp(buf, "exfat driver smoke test\n") != 0) return fail("README.TXT content mismatch");
    snprintf(path, sizeof(path), "%s/%s", root, "DIR/child.txt");
    if (read_all(path, buf, sizeof(buf)) <= 0) return fail("nested read failed");
    if (strcmp(buf, "nested file\n") != 0) return fail("nested content mismatch");
    wr(STDOUT_FILENO, "exfat_ro: ok\n");
    return 0;
}
