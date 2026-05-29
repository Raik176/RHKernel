#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

static void wr(int fd, const char *s) { write(fd, s, strlen(s)); }
static void err(const char *s) { wr(STDERR_FILENO, s); }

static void usage(void) { err("usage: unmount <target>\n"); }

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] == 0) { usage(); return 2; }
    if (unmount(argv[1]) == 0) return 0;
    err("unmount: failed: ");
    err(argv[1]);
    err("\n");
    return 1;
}
