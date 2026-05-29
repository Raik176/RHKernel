#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void write_fd(int fd, const char *s) { write(fd, s, strlen(s)); }

static void write_dec(uint64_t v) {
    char buf[32];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    else {
        char tmp[32];
        int t = 0;
        while (v && t < 32) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t) buf[n++] = tmp[--t];
    }
    write(STDERR_FILENO, buf, (size_t)n);
}

static void launch_shell_child() {
    int fd = open("/dev/input/events", O_RDONLY);
    if (fd < 0) {
        write_fd(STDERR_FILENO, "init: failed to open /dev/input/events\n");
        _exit(1);
    }

    if (dup2(fd, STDIN_FILENO) != 0) {
        write_fd(STDERR_FILENO, "init: dup2(input events, stdin) failed\n");
        _exit(1);
    }

    int exec_ret = exec("/bin/sh", 0);
    write_fd(STDERR_FILENO, "init: exec /bin/sh failed ret=");
    write_dec((uint64_t)exec_ret);
    write_fd(STDERR_FILENO, "\n");
    _exit(1);
}

int main() {
    for (;;) {
        pid_t pid = fork();
        if (pid == 0) launch_shell_child();

        if (pid < 0) {
            write_fd(STDERR_FILENO, "init: fork failed; retrying\n");
            sleep(100);
            continue;
        }

        int status = 0;
        wait(&status);
    }
}
