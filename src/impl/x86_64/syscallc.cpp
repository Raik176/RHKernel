#include "util.h"
#include "console.h"

#define IA32_EFER   0xC0000080
#define IA32_STAR   0xC0000081
#define IA32_LSTAR  0xC0000082
#define IA32_FMASK  0xC0000084

enum SyscallNumbers {
    SYSCALL_PRINT = 0,
};

extern "C" {
    void syscall_entry();

    void syscall_handler(struct regs* r) {
        uint64_t syscall = r->rax;
        uint64_t arg1 = r->rdi;
        uint64_t arg2 = r->rsi;
        uint64_t arg3 = r->rdx;
        uint64_t arg4 = r->r10;
        uint64_t arg5 = r->r8;
        uint64_t arg6 = r->r9;

        switch (syscall) {
            case SYSCALL_PRINT: {
                const char* user_str = (const char*)arg1;
                console::printf(user_str);
                break;
            }
            default:
                console::printf("Unknown syscall: %d\n", syscall);
                break;
        }
    }

    void enable_syscalls() {
        // 1. Enable SCE (System Call Extensions) in EFER
        uint32_t efer_low, efer_high;
        asm volatile("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(IA32_EFER));
        efer_low |= 1;
        asm volatile("wrmsr" : : "a"(efer_low), "d"(efer_high), "c"(IA32_EFER));

        // 2. Set the entry point address (LSTAR)
        uint64_t addr = (uint64_t)syscall_entry;
        asm volatile("wrmsr" : : "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)), "c"(IA32_LSTAR));
        
        uint64_t star =
            ((uint64_t)0x08 << 32) |   // kernel CS
            ((uint64_t)(0x23 - 16) << 48); // user CS - 16

        asm volatile("wrmsr" : : "a"(0), "d"((uint32_t)(star >> 32)), "c"(IA32_STAR));

        // 4. FMASK: You had 0x202. 
        // Bit 1 (0x2) is reserved and must be 1. Bit 9 (0x200) is IF.
        // This clears the Interrupt Flag on entry so you don't get interrupted 
        // before the swapgs/stack switch is done.
        asm volatile("wrmsr" : : "a"(0x200), "d"(0), "c"(IA32_FMASK));
    }
}