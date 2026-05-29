#include <fcntl.h>
#include <input.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static void raw_write_fd(int fd, const void *buf, size_t len) { write(fd, buf, len); }
static void raw_write_s(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void raw_write_err(const char *s) { write(STDERR_FILENO, s, strlen(s)); }
static void raw_write_c(char c) { write(STDOUT_FILENO, &c, 1); }

static int read_launch_search(void) {
    int fd = open("/task/self/launch/search", O_RDWR);
    if (fd < 0) return -1;

    char buf[256];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) break;
        raw_write_fd(STDOUT_FILENO, buf, (size_t)n);
    }

    close(fd);
    return 0;
}

static int write_launch_search(int argc, char **argv) {
    size_t len = 0;
    for (int i = 1; i < argc; i++) len += strlen(argv[i]) + 1;
    if (len == 0 || len > 4096) return -1;

    char *buf = (char *)malloc(len);
    if (!buf) return -1;

    size_t p = 0;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        for (size_t j = 0; j < n; j++) buf[p++] = argv[i][j];
        buf[p++] = '\n';
    }

    int fd = open("/task/self/launch/search", O_RDWR);
    if (fd < 0) {
        free(buf);
        return -1;
    }

    ssize_t written = write(fd, buf, len);
    close(fd);
    free(buf);
    return written == (int64_t)len ? 0 : -1;
}

static int read_input_char(char *out) {
    for (;;) {
        struct input_event ev;
        ssize_t n = read(STDIN_FILENO, &ev, sizeof(ev));
        if (n <= 0) {
            sched_yield();
            continue;
        }
        if ((size_t)n != sizeof(ev)) continue;
        if (ev.type != INPUT_EVENT_KEY) continue;
        if (ev.value != INPUT_KEY_PRESSED && ev.value != INPUT_KEY_REPEATED) continue;
        if (ev.text == 0 || ev.text > 0x7f) continue;
        *out = (char)ev.text;
        return 0;
    }
}

static char *read_line(void) {
    size_t cap = 128;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return nullptr;

    for (;;) {
        char c = 0;
        if (read_input_char(&c) != 0) continue;

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            raw_write_c('\n');
            return buf;
        }

        if (c == '\b' || c == 0x7f) {
            if (len > 0) {
                len--;
                raw_write_s("\b \b");
            }
            continue;
        }

        if (c < 0x20 || c == 0x7f) continue;

        if (len + 1 >= cap) {
            size_t new_cap = cap * 2;
            char *new_buf = (char *)realloc(buf, new_cap);
            if (!new_buf) {
                free(buf);
                return nullptr;
            }

            buf = new_buf;
            cap = new_cap;
        }

        buf[len++] = c;
        raw_write_c(c);
    }
}

static char **split_args(char *line, int *argc_out) {
    int argc = 0;
    int cap = 8;
    char **argv = (char **)malloc((size_t)cap * sizeof(char *));
    if (!argv) return nullptr;

    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        if (argc + 1 >= cap) {
            int new_cap = cap * 2;
            char **new_argv = (char **)realloc(argv, (size_t)new_cap * sizeof(char *));
            if (!new_argv) {
                free(argv);
                return nullptr;
            }

            argv = new_argv;
            cap = new_cap;
        }

        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }

    argv[argc] = nullptr;
    *argc_out = argc;
    return argv;
}

static char *getcwd_alloc(void) {
    size_t cap = 128;

    for (;;) {
        char *buf = (char *)malloc(cap);
        if (!buf) return nullptr;

        if (getcwd(buf, cap)) return buf;

        free(buf);

        if (cap > (1UL << 20)) return nullptr;
        cap *= 2;
    }
}

static void print_prompt(void) {
    char *cwd = getcwd_alloc();
    if (cwd) {
        raw_write_s(cwd);
        free(cwd);
    } else {
        raw_write_s("?");
    }

    raw_write_s(" > ");
}

static int run_builtin(int argc, char **argv) {
    if (argc == 0) return 1;

    if (str_eq(argv[0], "help")) {
        raw_write_s("builtins: help, cd, pwd, launch, exit\n");
        raw_write_s("external commands: bare names use /task/self/launch/search\n");
        return 1;
    }

    if (str_eq(argv[0], "cd")) {
        const char *path = argc > 1 ? argv[1] : "/";
        if (chdir(path) != 0) raw_write_err("cd failed\n");
        return 1;
    }

    if (str_eq(argv[0], "launch")) {
        if (argc == 1) {
            if (read_launch_search() != 0) raw_write_err("launch read failed\n");
        } else if (write_launch_search(argc, argv) != 0) {
            raw_write_err("launch write failed\n");
        }
        return 1;
    }

    if (str_eq(argv[0], "pwd")) {
        char *cwd = getcwd_alloc();
        if (!cwd) {
            raw_write_err("pwd failed\n");
            return 1;
        }

        raw_write_s(cwd);
        raw_write_s("\n");
        free(cwd);
        return 1;
    }

    if (str_eq(argv[0], "exit")) {
        _exit(0);
        return 1;
    }

    return 0;
}

int main(int, char **) {
    for (;;) {
        print_prompt();

        char *line = read_line();
        if (!line) {
            raw_write_err("minsh: out of memory\n");
            continue;
        }

        int argc = 0;
        char **argv = split_args(line, &argc);
        if (!argv) {
            raw_write_err("minsh: out of memory\n");
            free(line);
            continue;
        }

        if (argc == 0 || run_builtin(argc, argv)) {
            free(argv);
            free(line);
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            exec(argv[0], argv);
            raw_write_err("command not found\n");
            _exit(1);
        }

        if (pid < 0) {
            raw_write_err("fork failed\n");
        } else {
            wait(0);
        }

        free(argv);
        free(line);
    }
}