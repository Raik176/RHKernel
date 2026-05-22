#include <stdint.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_CLOSE 3
#define SYSCALL_SLEEP 5
#define SYSCALL_EXIT 6
#define SYSCALL_WAIT 7
#define SYSCALL_DUP2 8
#define SYSCALL_FORK 10
#define SYSCALL_EXEC 11

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

static uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void write_fd(int fd, const char *s) {
    syscall3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, str_len(s));
}

static void write_dec(uint64_t v) {
    char buf[32];
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[32];
        int t = 0;
        while (v && t < 32) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t) buf[n++] = tmp[--t];
    }
    syscall3(SYSCALL_WRITE, STDERR, (uintptr_t)buf, (uint64_t)n);
}

static void launch_shell_child() {
    int fd = (int)syscall1(SYSCALL_OPEN, (uintptr_t)"/dev/input/kbd0");
    if (fd < 0) {
        write_fd(STDERR, "init: failed to open /dev/input/kbd0\n");
        syscall1(SYSCALL_EXIT, 1);
    }

    if (syscall3(SYSCALL_DUP2, (uint64_t)fd, 0, 0) != 0) {
        write_fd(STDERR, "init: dup2(kbd0, stdin) failed\n");
        syscall1(SYSCALL_EXIT, 1);
    }

    // /bin/sh's main() ignores argv, so do not pass a stack-local argv array here.
    // Command execution from the shell still passes argv normally; this only affects
    // the initial shell handoff from init.
    uint64_t exec_ret = syscall3(SYSCALL_EXEC, (uintptr_t)"/bin/sh", 0, 0);

    write_fd(STDERR, "init: exec /bin/sh failed ret=");
    write_dec(exec_ret);
    write_fd(STDERR, "\n");
    syscall1(SYSCALL_EXIT, 1);
}

int main() {
    for (;;) {
        uint64_t pid = syscall0(SYSCALL_FORK);
        if (pid == 0) { launch_shell_child(); }

        if ((int64_t)pid < 0) {
            write_fd(STDERR, "init: fork failed; retrying\n");
            syscall1(SYSCALL_SLEEP, 100);
            continue;
        }

        int status = 0;
        syscall1(SYSCALL_WAIT, (uintptr_t)&status);
    }
}
