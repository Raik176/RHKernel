#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void out(int fd, const char *s) { write(fd, s, strlen(s)); }

int main(int argc, char **argv) {
    if (argc < 2) { out(STDERR_FILENO, "usage: touch <path>...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_CREAT | O_WRONLY, 0644);
        if (fd < 0) { out(STDERR_FILENO, "touch: failed: "); out(STDERR_FILENO, argv[i]); out(STDERR_FILENO, "\n"); rc = 1; }
        else close(fd);
    }
    return rc;
}
