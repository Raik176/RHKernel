#include <stdint.h>

#define SYSCALL_WRITE 0
#define SYSCALL_OPEN 1
#define SYSCALL_READ 2
#define SYSCALL_CLOSE 3
#define SYSCALL_YIELD 4
#define SYSCALL_SLEEP 5
#define SYSCALL_EXIT 6
#define SYSCALL_WAIT 7
#define SYSCALL_DUP2 8
#define SYSCALL_CLONE 9
#define SYSCALL_FORK 10
#define SYSCALL_EXEC 11

#define STDOUT 1

static inline uint64_t syscall0(uint64_t num) {
    uint64_t ret;
    asm volatile("syscall" : "=a"(ret) : "a"(num) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t syscall1(uint64_t num, uint64_t a1) {
    uint64_t ret;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1) : "rcx", "r11", "memory");
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

int main() {
    uint64_t pid = syscall0(SYSCALL_FORK);
    if (pid == 0) {
        int fd = syscall1(SYSCALL_OPEN, (uintptr_t)"/dev/input/kbd0");
        if (fd < 0) {
            const char *err = "Failed to open kbd0\n";
            syscall3(SYSCALL_WRITE, STDOUT, (uintptr_t)err, 20);
            syscall0(SYSCALL_EXIT);
        }

        syscall3(SYSCALL_DUP2, fd, 0, 0);
        const char *prog = "/bin/sh";
        int ret = syscall1(SYSCALL_EXEC, (uintptr_t)prog);

        const char *err2 = "Exec failed\n";
        syscall3(SYSCALL_WRITE, STDOUT, (uintptr_t)err2, 12);
        syscall0(SYSCALL_EXIT);
    }

    for (;;) { syscall0(SYSCALL_WAIT); }
}
