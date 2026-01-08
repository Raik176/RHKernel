#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

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
#define STDIN 0

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

int read_line(char *buf, int max_len) {
    int pos = 0;
    while (pos < max_len - 1) {
        char c;
        int64_t n = syscall3(SYSCALL_READ, STDIN, (uintptr_t)&c, 1);

        if (n <= 0) {
            syscall0(SYSCALL_YIELD);
            continue;
        }

        if (c == '\n' || c == '\r') {
            buf[pos] = '\0';
            return pos;
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                const char erase_seq[] = {'\b', ' ', '\b'};
                syscall3(SYSCALL_WRITE, STDOUT, (uintptr_t)erase_seq, 3);
            }
        } else {
            buf[pos++] = c;
            syscall3(SYSCALL_WRITE, STDOUT, (uintptr_t)&c, 1);
        }
    }
    buf[pos] = '\0';
    return pos;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);  // TODO: remove once sbrk is implemented.

    char cmd_buffer[128];
    char *argv[16];
    const char *prompt = "minsh> ";

    while (1) {
        printf(prompt);

        int len = read_line(cmd_buffer, sizeof(cmd_buffer));
        puts("");
        if (len <= 0) { continue; }

        int argc = 0;
        char *ptr = cmd_buffer;

        while (*ptr && argc < 15) {
            while (*ptr == ' ') ptr++;
            if (*ptr == '\0') break;

            argv[argc++] = ptr;

            while (*ptr && *ptr != ' ') ptr++;
            if (*ptr == ' ') {
                *ptr = '\0';
                ptr++;
            }
        }
        argv[argc] = nullptr;

        if (argc > 0) {
            uint64_t pid = fork();
            if (pid == 0) {
                syscall3(SYSCALL_EXEC, (uintptr_t)argv[0], (uintptr_t)argv, 0);

                printf("command not found: %s\n", argv[0]);
                syscall1(SYSCALL_EXIT, 1);
            } else {
                syscall1(SYSCALL_WAIT, 0);
            }
        }
    }
}