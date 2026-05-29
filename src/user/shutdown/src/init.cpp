#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

static void out(int fd, const char *s) { write(fd, s, strlen(s)); }

static int write_all(int fd, const char *s) {
    size_t len = strlen(s);
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, s + done, len - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

int main(int, char **) {
    int fd = open("/dev/power", O_WRONLY);
    if (fd < 0) {
        out(STDERR_FILENO, "shutdown: cannot open /dev/power\n");
        return 1;
    }

    out(STDOUT_FILENO, "shutdown: writing 'poweroff' to /dev/power\n");
    if (write_all(fd, "poweroff\n") != 0) {
        out(STDERR_FILENO, "shutdown: /dev/power rejected request\n");
        close(fd);
        return 1;
    }

    close(fd);
    out(STDERR_FILENO, "shutdown: request returned without powering off\n");
    return 1;
}
