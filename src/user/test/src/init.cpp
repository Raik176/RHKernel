#include <stdint.h>

#define SYS_WRITE 0
#define STDOUT 1

static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
}

extern "C" void _start() {
    const char *msg = "Hello from /bin/test! I am a separate process image.\n";
    syscall3(SYS_WRITE, STDOUT, (uintptr_t)msg, 53);

    // Assuming you have an EXIT syscall (SYSCALL_EXIT is 6 in your enum)
    asm volatile("mov $6, %%rax; xor %%rdi, %%rdi; syscall" ::: "rax", "rdi");

    for (;;);
}