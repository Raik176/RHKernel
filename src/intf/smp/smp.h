#pragma once
#include <stdint.h>

#include "gdt.h"
#include "memory/heap.h"
#include "smp/scheduler.h"

namespace smp {
    struct trampoline_data {
        uint32_t cr3;
        uint64_t cpu_index;
        uint64_t stack_top;
        uint64_t entry_point;
        volatile uint64_t status;
    } __attribute__((packed));

    struct cpu_local {
        cpu_local* self;

        uint64_t ticks;
        void* kernel_stack;

        scheduler::task* current_task;
        scheduler::task* task_queues[scheduler::MAX_QUEUES];
        scheduler::task* idle_task;
        heap::SlabHeader* heap_cache[heap::CACHE_COUNT];

        gdt::gdt_pointer gdt_ptr;
        gdt::gdt_entry gdt_entries[gdt::MAX_ENTRIES];
        gdt::tss tss_entry;

        uint32_t lapic_id;
        uint32_t cpu_index;
    };

    void init_aps();
    void init_bsp();
    uint64_t get_core_count();
    cpu_local* get_cpu_by_index(uint64_t index);

    static inline cpu_local* get_cpu() {
        cpu_local* p;
        asm volatile("mov %%gs:0, %0" : "=r"(p));
        return p;
    }
}  // namespace smp