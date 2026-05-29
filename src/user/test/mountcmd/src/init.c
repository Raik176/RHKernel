#include <fcntl.h>
#include <string.h>
#include <sys/fsctl.h>
#include <sys/mount.h>
#include <unistd.h>

static void wr(int fd, const char *s) { write(fd, s, strlen(s)); }
static void out(const char *s) { wr(STDOUT_FILENO, s); }
static void err(const char *s) { wr(STDERR_FILENO, s); }

static int fail(const char *s) {
    err("mountcmd: ");
    err(s);
    err("\n");
    return 1;
}

int main(void) {
    struct fsctl_args args = { 0, 0, 0, 0 };
    if (sizeof(args.source) != sizeof(char *)) return fail("bad fsctl ABI");
    if (mount(0, "/", 0, 0) >= 0) return fail("mount accepted null source");
    if (unmount(0) >= 0) return fail("unmount accepted null target");

    int fd = open("/proc/mounts", O_RDONLY);
    if (fd < 0) return fail("/proc/mounts missing");
    close(fd);

    out("mountcmd: ok\n");
    return 0;
}
