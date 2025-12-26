#include <stdint.h>

extern "C" void _start() {
    uint64_t syscall_number = 0;
    uint64_t result;

    asm volatile (
        "syscall"
        : "=a" (result)           // Output: the return value will be in RAX
        : "a" (syscall_number)    // Input: put syscall_number into RAX
        : "rcx", "r11", "memory"  // Clobbers: these registers are modified by the CPU
    );

    // After the syscall returns, you can loop or check the result
    volatile int i = 0;
    while (true) {
        i = result; // Just to use the variable
        i++;
    }
}