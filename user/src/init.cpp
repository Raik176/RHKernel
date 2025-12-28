#include <stdint.h>

extern "C" void _start() {
    const char* msg = "Hello from syscall!\n";
    uint64_t result;

    asm volatile (
        "syscall"
        : "=a" (result)           // RAX output
        : "a" (0),                // syscall number = 0
          "D" (msg)               // first arg = pointer in RDI
        : "rcx", "r11", "memory"
    );

    while (true) { } // hang
}
