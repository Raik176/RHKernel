#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/fsctl.h>
#include <sys/mount.h>

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void wr(int fd, const char *s) { write(fd, s, strlen(s)); }
static void out(const char *s) { wr(STDOUT_FILENO, s); }
static void err(const char *s) { wr(STDERR_FILENO, s); }

static void usage(void) {
    err("usage:\n");
    err("  fs mount [-t type] [-o flags] <source> <target>\n");
    err("  fs umount <target>\n");
    err("  fs probe [-t type] <source>\n");
    err("  fs mounts\n");
    err("  fs types\n");
    err("  fs info\n");
    err("  fs disks\n");
}

static int cat_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[512];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        write(STDOUT_FILENO, buf, (size_t)n);
    }
    return (int)close(fd);
}

static int do_fsctl(int op, struct fsctl_args *args) {
    return fsctl(op, args);
}

static int cmd_mount(int argc, char **argv) {
    const char *type = 0;
    const char *flags = 0;
    int i = 2;
    while (i < argc && argv[i][0] == '-') {
        if (streq(argv[i], "-t") && i + 1 < argc) { type = argv[i + 1]; i += 2; continue; }
        if (streq(argv[i], "-o") && i + 1 < argc) { flags = argv[i + 1]; i += 2; continue; }
        usage();
        return 2;
    }
    if (argc - i != 2) { usage(); return 2; }
    if (mount(argv[i], argv[i + 1], type, flags) == 0) return 0;
    err("fs: mount failed\n");
    return 1;
}

static int cmd_umount(int argc, char **argv) {
    if (argc != 3) { usage(); return 2; }
    if (unmount(argv[2]) == 0) return 0;
    err("fs: umount failed\n");
    return 1;
}

static int cmd_probe(int argc, char **argv) {
    const char *type = 0;
    int i = 2;
    if (i < argc && streq(argv[i], "-t")) {
        if (i + 1 >= argc) { usage(); return 2; }
        type = argv[i + 1];
        i += 2;
    }
    if (argc - i != 1) { usage(); return 2; }
    struct fsctl_args args = { argv[i], 0, type, 0 };
    int r = do_fsctl(FS_CTL_PROBE, &args);
    if (r == FS_PROBE_YES) { out("yes\n"); return 0; }
    if (r == FS_PROBE_NO) { out("no\n"); return 1; }
    if (r == FS_PROBE_UNSUPPORTED) { out("unsupported\n"); return 3; }
    out("error\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    if (streq(argv[1], "mount")) return cmd_mount(argc, argv);
    if (streq(argv[1], "umount") || streq(argv[1], "unmount")) return cmd_umount(argc, argv);
    if (streq(argv[1], "probe")) return cmd_probe(argc, argv);
    if (streq(argv[1], "mounts")) return cat_file("/proc/mounts") == 0 ? 0 : 1;
    if (streq(argv[1], "types") || streq(argv[1], "filesystems")) return cat_file("/proc/filesystems") == 0 ? 0 : 1;
    if (streq(argv[1], "info") || streq(argv[1], "fsinfo")) return cat_file("/proc/fsinfo") == 0 ? 0 : 1;
    if (streq(argv[1], "disks") || streq(argv[1], "drives")) return cat_file("/proc/disks") == 0 ? 0 : 1;
    usage();
    return 2;
}
