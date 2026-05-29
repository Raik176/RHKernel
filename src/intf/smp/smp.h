#pragma once
#include <stdint.h>

#include "gdt.h"
#include "memory/heap.h"
#include "smp/scheduler.h"
#include "util.h"

namespace smp {
    static constexpr uint64_t PANIC_STACK_SIZE = 16 * 1024;
    static constexpr int64_t MAIL_RECEIVER_ALL = -2;
    static constexpr int64_t MAIL_RECEIVER_OTHERS = -1;

    struct trampoline_data {
        uint32_t cr3;
        uint32_t flags;
        uint64_t cpu_index;
        uint64_t stack_top;
        uint64_t entry_point;
        volatile uint64_t status;
        uint64_t cpu_local_ptr;
        uint32_t lapic_id;
    } __attribute__((packed));

    enum class mail_type {
        HALT,
        TLB_SHOOTDOWN,
        RESCHEDULE
    };

    enum class mail_status {
        QUEUED,
        ALREADY_PENDING,
        INVALID_TARGET,
        INVALID_MESSAGE,
        BUSY
    };

    struct mail {
        mail_type type;
        uint64_t sender_core;
        mail *next;
        volatile bool queued;
        volatile bool handled;

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
        void *panic_stack;
        scheduler::task *task_queues_tail[scheduler::MAX_QUEUES];
        scheduler::task *task_queues_head[scheduler::MAX_QUEUES];
        uint64_t task_queue_oldest_tick[scheduler::MAX_QUEUES];
        uint32_t task_queue_count[scheduler::MAX_QUEUES];
        uint32_t runnable_count;
        uint64_t steal_attempts;
        uint64_t steal_successes;
        uint64_t steal_locked;
        uint64_t steal_empty;
        scheduler::task *sleep_list_head;
        scheduler::task *idle_task;
        spinlock_t sched_lock;

        heap::SlabHeader *heap_cache[heap::CACHE_COUNT];

        gdt::gdt_pointer gdt_ptr;
        gdt::gdt_entry gdt_entries[gdt::MAX_ENTRIES];
        gdt::tss tss_entry;

        uint32_t lapic_id;
        uint32_t cpu_index;

        mail *mail_head;
        mail *mail_tail;
        uint64_t mail_depth;
        spinlock_t mail_lock;
        mail tlb_shootdown_mail;
        mail reschedule_mail;
        mail halt_mail;
        volatile bool reschedule_pending;
        uint64_t mail_enqueued;
        uint64_t mail_handled;
        uint64_t mail_busy;
        uint64_t mail_invalid;
        uint64_t mail_coalesced;
        uint64_t reschedule_requests;
        uint64_t reschedule_ipis;
        uint64_t reschedule_deferred;
        uint64_t reschedule_switches;

        struct cpu_features cpu_features;
    };

    static_assert(offsetof(cpu_local, self) == 0,
                  "GS:0 must be 'self' for pointer indirection to work");

    static_assert(offsetof(cpu_local, kernel_stack) == 16,
                  "Assembly syscall_entry expects kernel_stack at GS:16");

    static_assert(offsetof(cpu_local, user_rsp) == 24,
                  "Assembly syscall_entry expects user_rsp at GS:24");

    static_assert(offsetof(cpu_local, current_task) == 32,
                  "Assembly syscall_entry expects current_task at GS:32");

    void init_aps();
    void init_bsp();
    uint64_t get_core_count();
    cpu_local *get_cpu_by_index(uint64_t index);

    bool send_tlb_shootdown_mail(int64_t target_cpu, mail *message, uint64_t cr3, uint64_t addr,
                                  uint32_t pages);
    bool send_reschedule_mail(uint64_t target_cpu);
    void send_halt_mail(int64_t target_cpu);
    void flush_mail(int64_t target_cpu);
    void panic_stop_others();

    static inline cpu_local *get_cpu() {
        cpu_local *p;
        asm volatile("mov %%gs:0, %0" : "=r"(p));
        return p;
    }
}  // namespace smp