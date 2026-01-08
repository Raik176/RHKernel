#include "util.h"

#include "console.h"
#include "smp/apic.h"
#include "smp/smp.h"
#include "symbol/ksym.h"

extern "C" {
extern uint8_t higher_stack_top[];
extern uint8_t higher_stack_bottom[];
}

void busy_sleep(uint64_t ms) {
    uint64_t start = apic::get_ticks();
    uint64_t ticks_to_wait = ms / apic::get_tick_scale();
    while ((apic::get_ticks() - start) < ticks_to_wait) { asm volatile("pause"); }
}

void fill_regs(struct regs *r) {
    // 1. General Purpose Registers
    asm volatile("mov %%rax, %0" : "=m"(r->rax));
    asm volatile("mov %%rbx, %0" : "=m"(r->rbx));
    asm volatile("mov %%rcx, %0" : "=m"(r->rcx));
    asm volatile("mov %%rdx, %0" : "=m"(r->rdx));
    asm volatile("mov %%rsi, %0" : "=m"(r->rsi));
    asm volatile("mov %%rdi, %0" : "=m"(r->rdi));
    asm volatile("mov %%r8,  %0" : "=m"(r->r8));
    asm volatile("mov %%r9,  %0" : "=m"(r->r9));
    asm volatile("mov %%r10, %0" : "=m"(r->r10));
    asm volatile("mov %%r11, %0" : "=m"(r->r11));
    asm volatile("mov %%r12, %0" : "=m"(r->r12));
    asm volatile("mov %%r13, %0" : "=m"(r->r13));
    asm volatile("mov %%r14, %0" : "=m"(r->r14));
    asm volatile("mov %%r15, %0" : "=m"(r->r15));

    asm volatile("lea 0(%%rip), %0" : "=r"(r->rip));
    asm volatile("pushfq; popq %0" : "=m"(r->rflags));

    asm volatile("mov %%cs, %0" : "=m"(r->cs));
    asm volatile("mov %%ss, %0" : "=m"(r->ss));

    asm volatile("mov %%rbp, %0" : "=r"(r->rbp));
    asm volatile("lea 16(%%rsp), %0" : "=r"(r->rsp));

    r->int_no = 0;
    r->err_code = 0;
}

void __attribute__((noreturn)) kpanic(const char *message, struct regs *r) {
    smp::send_halt_mail(-1);
    smp::flush_mail(-1);

    if (r == nullptr) {
        r = {};
        fill_regs(r);
    }

    console::set_color(console::Color::Red);
    console::printf(
        "\n================================================================================\n");
    console::printf(
        "                                KERNEL PANIC                                    \n");
    console::printf(
        "================================================================================\n");
    console::set_color(console::Color::White);

    if (r && r->int_no <= 31 && r->int_no != 0) {
        console::printf("  REASON: %s (Vector %d, Error %d)\n", message, r->int_no, r->err_code);
    } else {
        console::printf("  REASON: %s\n", message);
    }

    smp::cpu_local *cpu = smp::get_cpu();
    console::printf("  CPU   : #%d (LAPIC %d)\n", cpu->cpu_index, cpu->lapic_id);
    if (cpu->current_task) { console::printf("  TASK  : %d\n", cpu->current_task->id); }

    console::printf("\n--- REGISTER STATE ---\n");
    dump_regs(r);

    if (r->int_no == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        console::printf("  CR2 (Faulting Address): %p\n", cr2);
    }

    console::printf("\n--- STACKTRACE ---\n");
    print_stacktrace();

    console::printf(
        "\n================================================================================\n");
    console::printf("  SYSTEM HALTED.\n");

    for (;;) asm volatile("hlt");
}

void print_stacktrace() {
    struct stack_frame {
        struct stack_frame *next;
        uint64_t return_address;
    };

    struct stack_frame *frame;
    asm volatile("mov %%rbp, %0" : "=r"(frame));

    for (uint64_t i = 0; i < 16 && frame; ++i) {
        if ((uintptr_t)frame < (uint64_t)higher_stack_bottom ||
            (uintptr_t)frame > (uint64_t)higher_stack_top || (uint64_t)frame & 0x7) {
            break;
        }

        uintptr_t offset = 0;
        const char *symbol = ksym::get_name(frame->return_address, &offset);

        console::printf("  #%02d [%p @ %s+%p]\n", i, frame->return_address,
                        symbol == nullptr ? "unknown" : symbol, offset);
        frame = frame->next;
    }
}

void dump_regs(struct regs *r) {
    console::set_color(console::Color::LightCyan);
    console::printf("RAX: %p  RBX: %p  RCX: %p  RDX: %p\n", r->rax, r->rbx, r->rcx, r->rdx);
    console::printf("RSI: %p  RDI: %p  RBP: %p  RSP: %p\n", r->rsi, r->rdi, r->rbp, r->rsp);
    console::printf("R8 : %p  R9 : %p  R10: %p  R11: %p\n", r->r8, r->r9, r->r10, r->r11);
    console::printf("R12: %p  R13: %p  R14: %p  R15: %p\n", r->r12, r->r13, r->r14, r->r15);
    console::printf("RIP: %p  CS : %p  SS : %p  FLG: %p\n", r->rip, r->cs, r->rflags);
    console::set_color(console::Color::White);
}