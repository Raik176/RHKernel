#include <stdint.h>

extern "C" void _start() {
    const char *msg = "Hello from syscall!\n";
    uint64_t result;

    asm volatile("syscall" : "=a"(result) : "a"(0), "D"(msg) : "rcx", "r11", "memory");

    while (true) {}  // hang
}