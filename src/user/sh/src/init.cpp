#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

#define SYSCALL_WRITE 0
#define SYSCALL_READ 2
#define SYSCALL_YIELD 4
#define SYSCALL_EXIT 6
#define SYSCALL_WAIT 7
#define SYSCALL_FORK 10
#define SYSCALL_EXEC 11
#define SYSCALL_CHDIR 20
#define SYSCALL_GETCWD 21

#define STDIN 0
#define STDOUT 1
#define STDERR 2

static inline uint64_t syscall0(uint64_t num) {
    uint64_t ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(0ULL), "S"(0ULL), "d"(0ULL)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t syscall1(uint64_t num, uint64_t a1) {
    uint64_t ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a1), "S"(0ULL), "d"(0ULL)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
}

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void raw_write_fd(int fd, const void *buf, size_t len) {
    syscall3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)buf, (uint64_t)len);
}

static void raw_write_s(const char *s) {
    raw_write_fd(STDOUT, s, str_len(s));
}

static void raw_write_err(const char *s) {
    raw_write_fd(STDERR, s, str_len(s));
}

static void raw_write_c(char c) {
    raw_write_fd(STDOUT, &c, 1);
}

static char *read_line(void) {
    size_t cap = 128;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return nullptr;

    for (;;) {
        char c = 0;
        int64_t n = (int64_t)syscall3(SYSCALL_READ, STDIN, (uintptr_t)&c, 1);
        if (n <= 0) {
            syscall0(SYSCALL_YIELD);
            continue;
        }

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

        int64_t r = (int64_t)syscall3(SYSCALL_GETCWD, (uintptr_t)buf, cap, 0);
        if (r == 0) return buf;

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
        raw_write_s("builtins: help, cd, pwd, exit\n");
        raw_write_s("external commands: use absolute paths or cd into their directory\n");
        return 1;
    }

    if (str_eq(argv[0], "cd")) {
        const char *path = argc > 1 ? argv[1] : "/";
        if ((int64_t)syscall1(SYSCALL_CHDIR, (uintptr_t)path) < 0) raw_write_err("cd failed\n");
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
        syscall1(SYSCALL_EXIT, 0);
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

        uint64_t pid = syscall0(SYSCALL_FORK);
        if (pid == 0) {
            syscall3(SYSCALL_EXEC, (uintptr_t)argv[0], (uintptr_t)argv, 0);
            raw_write_err("command not found\n");
            syscall1(SYSCALL_EXIT, 1);
        }

        if ((int64_t)pid < 0) {
            raw_write_err("fork failed\n");
        } else {
            syscall1(SYSCALL_WAIT, 0);
        }

        free(argv);
        free(line);
    }
}