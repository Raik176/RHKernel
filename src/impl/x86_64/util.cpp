#include "util.h"

#include "smp/apic.h"
#include "console.h"

void busy_sleep(uint64_t ms) {
    uint64_t start = apic::get_ticks();
    uint64_t ticks_to_wait = ms / apic::get_tick_scale();
    while ((apic::get_ticks() - start) < ticks_to_wait) { asm volatile("pause"); }
}

void __attribute__((noreturn)) kpanic(const char* message) {
    apic::write_reg(apic::Register::ICRLO, 0x000C0000 | idt::HALT_VECTOR);

    console::printf("\n--- KERNEL PANIC ---\n");
    console::printf(message);
    console::printf("\nHalting system...");

    for(;;)
        asm volatile("hlt");
}