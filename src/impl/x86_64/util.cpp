#include "util.h"

void busy_sleep(uint64_t ms) {
    uint64_t start = apic::get_ticks();
    uint64_t ticks_to_wait = ms / apic::get_tick_scale();
    while ((apic::get_ticks() - start) < ticks_to_wait) {
        asm volatile("pause");
    }
}