#include "util.h"
#include "console.h"
#include "smp/scheduler.h"

extern "C" uint64_t syscall_handler(regs* r) {
    // Syscall number is in RAX
    // Arguments: RDI, RSI, RDX, R10, R8, R9

    console::printf("Syscall: %d", r->rax);

    return -1; // Return value passed back to user
}