#pragma once
#include <stdint.h>

namespace smp {
    struct trampoline_data{
        uint32_t cr3;
        uint64_t cpu_index;
        uint64_t stack_top;
        uint64_t entry_point;
        volatile uint64_t status;
    }__attribute__((packed));

    struct cpu_local {
        cpu_local* self;
    
        uint32_t lapic_id;
        uint32_t cpu_index;
    
        uint64_t ticks;
        void* kernel_stack;
    } __attribute__((packed));

    void init_aps();
    void init_bsp();
    uint64_t get_core_count();

    static inline cpu_local* get_cpu() {
        cpu_local* p;
        asm volatile("mov %%gs:0, %0" : "=r"(p));
        return p;
    }
}