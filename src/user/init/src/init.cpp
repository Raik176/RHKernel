#include <stdint.h>

#define SYS_WRITE 0
#define SYSCALL_WAIT 7
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

extern "C" void _start() {
    const char *msg_parent = "Init: Forking child to run /bin/test...\n";
    syscall3(SYS_WRITE, STDOUT, (uintptr_t)msg_parent, 40);

    for (;;);
}