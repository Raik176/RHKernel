#include "smp/scheduler.h"

#include "memory/heap.h"
#include "memory/vmm.h"
#include "smp/apic.h"
#include "smp/smp.h"
#include "string.h"
#include "console.h"

namespace scheduler {

    static uint64_t next_tid = 1;

    static inline void fxsave_task(task* t) {
        asm volatile(
            "fxsave (%0)"
            :
            : "r"(t->fxsave_area)
            : "memory"
        );
    }
    static inline void fxrstor_task(task* t) {
        asm volatile(
            "fxrstor (%0)"
            :
            : "r"(t->fxsave_area)
            : "memory"
        );
    }

    static task* steal_work(smp::cpu_local* self) {
        uint64_t core_count = smp::get_core_count();
        if (core_count < 2) return nullptr;

        uint64_t start_index = (self->cpu_index + 1) % core_count;

        for (uint64_t i = 0; i < core_count - 1; i++) {
            uint64_t victim_idx = (start_index + i) % core_count;
            smp::cpu_local* victim = smp::get_cpu_by_index(victim_idx);

            if (!victim || victim == self) continue;

            uint64_t victim_flags;
            if (!victim->sched_lock.try_acquire(victim_flags)) continue;

            for (int p = 0; p < MAX_QUEUES; p++) {
                if (victim->task_queues_tail[p]) {
                    task* t = victim->task_queues_tail[p];

                    victim->task_queues_tail[p] = t->prev;

                    if (victim->task_queues_tail[p]) {
                        victim->task_queues_tail[p]->next = nullptr;
                    } else {
                        victim->task_queues_head[p] = nullptr;
                    }

                    victim->sched_lock.release(victim_flags);

                    t->next = nullptr;
                    t->prev = nullptr;
                    t->quantum = TIME_QUANTUMS[t->priority];
                    return t;
                }
            }
            victim->sched_lock.release(victim_flags);
        }
        return nullptr;
    }

    static void enqueue(smp::cpu_local* cpu, task* t) {        
        int p = t->priority;
        t->next = nullptr;
        t->prev = cpu->task_queues_tail[p];

        if (!cpu->task_queues_head[p]) {
            cpu->task_queues_head[p] = t;
            cpu->task_queues_tail[p] = t;
        } else {
            cpu->task_queues_tail[p]->next = t;
            cpu->task_queues_tail[p] = t;
        }
    }

    static task* dequeue(smp::cpu_local* cpu) {
        for (int i = 0; i < MAX_QUEUES; i++) {
            if (cpu->task_queues_head[i]) {
                task* t = cpu->task_queues_head[i];

                cpu->task_queues_head[i] = t->next;

                if (cpu->task_queues_head[i]) {
                    cpu->task_queues_head[i]->prev = nullptr;
                } else {
                    cpu->task_queues_tail[i] = nullptr;
                }

                t->next = nullptr;
                t->prev = nullptr;
                return t;
            }
        }

        task* stolen = steal_work(cpu);
        return stolen ? stolen : cpu->idle_task;
    }

    void init_core() {
        smp::cpu_local* cpu = smp::get_cpu();

        task* idle = (task*)heap::kmalloc(sizeof(task));
        memset(idle, 0, sizeof(task));

        idle->id = 0;
        idle->priority = MAX_QUEUES - 1;
        idle->cr3 = vmm::get_kernel_pagemap();
        idle->state = task_state::RUNNING;
        idle->type = task_type::KERNEL;
        idle->kernel_stack = cpu->kernel_stack;

        cpu->idle_task = idle;
        cpu->current_task = idle;
    }

    task* spawn(task_type type, void (*entry_point)(), uint64_t pml4) {
        if (type == task_type::USER && pml4 == 0) {
            kpanic("User task requires a valid PML4");
        }

        smp::cpu_local* cpu = smp::get_cpu();
        task* t = (task*)heap::kmalloc(sizeof(task));
        memset(t, 0, sizeof(task));

        void* kstack = heap::kmalloc(KERNEL_STACK_SIZE);
        t->kernel_stack = kstack;

        regs* r = (regs*)((uintptr_t)kstack + KERNEL_STACK_SIZE - sizeof(regs));
        memset(r, 0, sizeof(regs));

        t->type = type;
        t->id = next_tid++;
        t->context = r;

        if (type == task_type::USER) {
            void* ustack = heap::kmalloc(INITIAL_USER_STACK_SIZE);
            t->user_stack = ustack;

            r->rip = (uint64_t)entry_point;
            r->rsp = (uint64_t)ustack + INITIAL_USER_STACK_SIZE;  // User RSP points to user stack
            r->cs = 0x23;                       // User Code Selector (GDT index 4 | RPL 3)
            r->ss = 0x1B;                       // User Data Selector (GDT index 3 | RPL 3)

            t->cr3 = pml4;
        } else {
            r->rip = (uint64_t)entry_point;
            r->rsp = (uint64_t)kstack + KERNEL_STACK_SIZE;
            r->cs = 0x08;                                   // Kernel Code
            r->ss = 0x10;                                   // Kernel Data
            t->user_stack = nullptr;

            t->cr3 = pml4 == 0 ? vmm::get_kernel_pagemap() : pml4;
        }

        r->rflags = 0x202;  // Interrupts enabled
        t->state = task_state::READY;
        t->priority = 0;
        t->quantum = TIME_QUANTUMS[0];

        asm volatile(
            "fninit\n\t"
            "fxsave (%0)"
            : 
            : "r"(t->fxsave_area) 
            : "memory"
        );

        uint64_t flags;
        cpu->sched_lock.acquire(flags);
        enqueue(cpu, t);
        cpu->sched_lock.release(flags);

        return t;
    }

    regs* schedule(regs* current_state, bool is_timer_tick) {
        smp::cpu_local* cpu = smp::get_cpu();
        if (!cpu) return current_state;

        uint64_t flags;
        cpu->sched_lock.acquire(flags);

        task* current = cpu->current_task;

        if (current) {
            current->context = current_state;
            fxsave_task(current);
        }

        for (int i = 1; i < MAX_QUEUES; i++) {
            task* item = cpu->task_queues_head[i];
            while (item) {
                task* next_item = item->next;
                item->age++;
            
                if (item->age > AGING_THRESHOLD) {
                    if (item->prev) item->prev->next = item->next;
                    else cpu->task_queues_head[i] = item->next;
                
                    if (item->next) item->next->prev = item->prev;
                    else cpu->task_queues_tail[i] = item->prev;
                
                    item->priority = i - 1;
                    item->age = 0;
                    item->quantum = TIME_QUANTUMS[item->priority];
                    enqueue(cpu, item);
                }
                item = next_item;
            }
        }

        if (current && current != cpu->idle_task) {
            if (is_timer_tick) {
                current->quantum--;

                if (current->quantum == 0) {
                    if (current->priority < MAX_QUEUES - 1) current->priority++;
                    current->quantum = TIME_QUANTUMS[current->priority];
                    current->state = task_state::READY;
                    enqueue(cpu, current);
                } else {
                    for (int i = 0; i < current->priority; i++) {
                        if (cpu->task_queues_head[i]) {
                            current->state = task_state::READY;
                            enqueue(cpu, current);
                            goto pick_next;
                        }
                    }

                    cpu->sched_lock.release(flags);
                    return current_state;
                }
            } else {
                current->state = task_state::READY;
                current->quantum = TIME_QUANTUMS[current->priority];
                enqueue(cpu, current);
            }
        }

    pick_next:
        task* next = dequeue(cpu);
        next->state = task_state::RUNNING;
        cpu->current_task = next;

        fxrstor_task(next);

        smp::get_cpu()->tss_entry.rsp0 = (uint64_t)next->kernel_stack + KERNEL_STACK_SIZE;

        asm volatile("mov %0, %%cr3" : : "r"(next->cr3) : "memory");

        cpu->sched_lock.release(flags);

        return next->context;
    }

    void yield() { asm volatile("int $0x81"); }

}  // namespace scheduler