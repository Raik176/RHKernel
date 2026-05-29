#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

static void wr(int fd, const char *s) { write(fd, s, strlen(s)); }

static int contains(const char *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (!m || m > n) return 0;
    for (size_t i = 0; i + m <= n; i++) {
        size_t j = 0;
        while (j < m && hay[i + j] == needle[j]) j++;
        if (j == m) return 1;
    }
    return 0;
}

static int write_search(const char *text) {
    int fd = open("/task/self/launch/search", O_RDWR);
    if (fd < 0) return -1;
    ssize_t r = write(fd, text, strlen(text));
    close(fd);
    return r == (ssize_t)strlen(text) ? 0 : -1;
}

static int read_search(char *buf, size_t cap, size_t *out_len) {
    int fd = open("/task/self/launch/search", O_RDWR);
    if (fd < 0) return -1;
    size_t pos = 0;
    while (pos < cap) {
        ssize_t r = read(fd, buf + pos, cap - pos);
        if (r < 0) { close(fd); return -1; }
        if (r == 0) break;
        pos += (size_t)r;
    }
    close(fd);
    if (out_len) *out_len = pos;
    return 0;
}

static int expect_bad_write(const char *text) {
    int fd = open("/task/self/launch/search", O_RDWR);
    if (fd < 0) return -1;
    ssize_t r = write(fd, text, strlen(text));
    close(fd);
    return r == 0 ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc == 2 && argv[1][0] == '-' && argv[1][1] == '-') return 0;

    char buf[512];
    size_t len = 0;
    if (read_search(buf, sizeof(buf), &len) != 0 || !contains(buf, len, "/bin\n")) {
        wr(STDERR_FILENO, "launch_search: default read failed\n");
        return 1;
    }

    if (expect_bad_write("bin\n") != 0 || expect_bad_write("/bin\n/bin\n") != 0 ||
        expect_bad_write("/bin\n\n") != 0 || expect_bad_write("/bin/..\n") != 0) {
        wr(STDERR_FILENO, "launch_search: invalid search accepted\n");
        return 1;
    }

    if (write_search("/bin/test\n/bin\n") != 0) {
        wr(STDERR_FILENO, "launch_search: valid write failed\n");
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        char *child_argv[] = {"launch_search", "--child", 0};
        exec("launch_search", child_argv);
        _exit(23);
    }
    if (pid < 0) {
        wr(STDERR_FILENO, "launch_search: fork failed\n");
        return 1;
    }

    int status = -1;
    if (wait(&status) != pid || status != 0) {
        wr(STDERR_FILENO, "launch_search: bare exec failed\n");
        return 1;
    }

    wr(STDOUT_FILENO, "launch_search: ok\n");
    return 0;
}
