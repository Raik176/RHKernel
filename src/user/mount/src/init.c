#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

static void wr(int fd, const char *s) { write(fd, s, strlen(s)); }
static void out(const char *s) { wr(STDOUT_FILENO, s); }
static void err(const char *s) { wr(STDERR_FILENO, s); }

static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static int cat_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    char buf[512];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) break;
        write(STDOUT_FILENO, buf, (size_t)n);
    }

    return close(fd);
}

static void usage(void) {
    err("usage: mount [-t type] [-o flags] <source> <target>\n");
    err("       mount\n");
}

int main(int argc, char **argv) {
    if (argc == 1) return cat_file("/proc/mounts") == 0 ? 0 : 1;

    const char *type = 0;
    const char *flags = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (streq(argv[i], "-t")) {
            if (i + 1 >= argc || argv[i + 1][0] == 0) { usage(); return 2; }
            type = argv[i + 1];
            i += 2;
            continue;
        }
        if (streq(argv[i], "-o")) {
            if (i + 1 >= argc || argv[i + 1][0] == 0) { usage(); return 2; }
            flags = argv[i + 1];
            i += 2;
            continue;
        }
        usage();
        return 2;
    }

    if (argc - i != 2) { usage(); return 2; }

    if (mount(argv[i], argv[i + 1], type, flags) == 0) return 0;
    err("mount: failed: ");
    err(argv[i]);
    err(" on ");
    err(argv[i + 1]);
    err("\n");
    return 1;
}
