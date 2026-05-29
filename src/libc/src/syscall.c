#include <libc.h>
#include <stdint.h>

uint64_t __syscall0(uint64_t num) {
    uint64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(0ULL), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory");
    return ret;
}

uint64_t __syscall1(uint64_t num, uint64_t a1) {
    uint64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory");
    return ret;
}

uint64_t __syscall2(uint64_t num, uint64_t a1, uint64_t a2) {
    uint64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(0ULL) : "rcx", "r11", "memory");
    return ret;
}

uint64_t __syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

uint64_t __syscall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

uint64_t __syscall6(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8 __asm__("r8") = a5;
    register uint64_t r9 __asm__("r9") = a6;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}
