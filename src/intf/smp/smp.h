#pragma once
#include <stdint.h>

#include "gdt.h"
#include "memory/heap.h"
#include "smp/scheduler.h"
#include "util.h"

namespace smp {
    static constexpr int MAILBOX_SIZE = 32;
    static constexpr int64_t MAIL_RECEIVER_ALL = -2;
    static constexpr int64_t MAIL_RECEIVER_OTHERS = -1;

    struct trampoline_data {
        uint32_t cr3;
        uint64_t cpu_index;
        uint64_t stack_top;
        uint64_t entry_point;
        volatile uint64_t status;
    } __attribute__((packed));

    enum class mail_type {
        HALT,
        TLB_SHOOTDOWN  // TODO: implement
    };

    struct mail {
        mail_type type;
        uint64_t sender_core;
        volatile bool *handled;

        union {
            struct {
                uint64_t cr3;
                uint64_t addr;
                uint32_t pages;
            } tlb;
        };
    };

    struct cpu_local {
        cpu_local *self;

        uint64_t ticks;
        void *kernel_stack;
        void *user_rsp;

        scheduler::task *current_task;
        scheduler::task *task_queues_tail[scheduler::MAX_QUEUES];
        scheduler::task *task_queues_head[scheduler::MAX_QUEUES];
        scheduler::task *sleep_list_head;
        scheduler::task *idle_task;
        lock::spinlock sched_lock;

        heap::SlabHeader *heap_cache[heap::CACHE_COUNT];

        gdt::gdt_pointer gdt_ptr;
        gdt::gdt_entry gdt_entries[gdt::MAX_ENTRIES];
        gdt::tss tss_entry;

        uint32_t lapic_id;
        uint32_t cpu_index;

        mail *mailbox[MAILBOX_SIZE];
        uint32_t mail_head;
        uint32_t mail_tail;
        lock::spinlock mail_lock;

        struct cpu_features cpu_features;
    };

    static_assert(offsetof(cpu_local, self) == 0,
                  "GS:0 must be 'self' for pointer indirection to work");

    static_assert(offsetof(cpu_local, kernel_stack) == 16,
                  "Assembly syscall_entry expects kernel_stack at GS:16");

    static_assert(offsetof(cpu_local, user_rsp) == 24,
                  "Assembly syscall_entry expects user_rsp at GS:24");

    void init_aps();
    void init_bsp();
    uint64_t get_core_count();
    cpu_local *get_cpu_by_index(uint64_t index);

    void send_halt_mail(int64_t target_cpu);
    void flush_mail(int64_t target_cpu);

    static inline cpu_local *get_cpu() {
        cpu_local *p;
        asm volatile("mov %%gs:0, %0" : "=r"(p));
        return p;
    }
}  // namespace smp