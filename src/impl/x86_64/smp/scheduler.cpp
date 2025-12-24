#include "smp/scheduler.h"

#include "memory/heap.h"
#include "memory/vmm.h"
#include "smp/apic.h"
#include "smp/smp.h"
#include "string.h"

namespace scheduler {

    static uint64_t next_tid = 1;

    static void enqueue(smp::cpu_local* cpu, task* t) {
        int p = t->priority;
        t->next = nullptr;
        if (!cpu->task_queues[p]) {
            cpu->task_queues[p] = t;
        } else {
            task* last = cpu->task_queues[p];
            while (last->next) last = last->next;
            last->next = t;
        }
    }

    static task* dequeue(smp::cpu_local* cpu) {
        for (int i = 0; i < MAX_QUEUES; i++) {
            if (cpu->task_queues[i]) {
                task* t = cpu->task_queues[i];
                cpu->task_queues[i] = t->next;
                return t;
            }
        }
        return cpu->idle_task;
    }

    void init_core() {
        smp::cpu_local* cpu = smp::get_cpu();

        task* idle = (task*)heap::kmalloc(sizeof(task));
        memset(idle, 0, sizeof(task));

        idle->id = 0;
        idle->priority = MAX_QUEUES - 1;
        idle->state = task_state::RUNNING;

        cpu->idle_task = idle;
        cpu->current_task = idle;
    }

    void spawn(task_type type, void (*entry_point)()) {
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
            void* ustack = heap::kmalloc(16384);
            t->user_stack = ustack;

            r->rip = (uint64_t)entry_point;
            r->rsp = (uint64_t)ustack + 16384;  // User RSP points to user stack
            r->cs = 0x23;                       // User Code Selector (GDT index 4 | RPL 3)
            r->ss = 0x1B;                       // User Data Selector (GDT index 3 | RPL 3)

            t->cr3 = (uint64_t)vmm::create_user_address_space();  // TODO
        } else {
            // Kernel Task
            r->rip = (uint64_t)entry_point;
            r->rsp = (uint64_t)kstack + KERNEL_STACK_SIZE;  // Kernel RSP points to its own kstack
            r->cs = 0x08;                                   // Kernel Code
            r->ss = 0x10;                                   // Kernel Data
            t->user_stack = nullptr;
        }

        r->rflags = 0x202;  // Interrupts enabled
        t->state = task_state::READY;
        t->priority = 0;
        t->quantum = TIME_QUANTUMS[0];

        enqueue(cpu, t);
    }

    regs* schedule(regs* current_state, bool is_timer_tick) {
        smp::cpu_local* cpu = smp::get_cpu();
        if (!cpu) return current_state;

        task* current = cpu->current_task;

        if (current) { current->context = current_state; }

        // 1. Aging Logic
        for (int i = 1; i < MAX_QUEUES; i++) {
            task** prev = &cpu->task_queues[i];
            task* item = cpu->task_queues[i];
            while (item) {
                item->age++;
                if (item->age > AGING_THRESHOLD) {
                    *prev = item->next;
                    task* promote = item;
                    item = item->next;

                    promote->priority = i - 1;
                    promote->age = 0;
                    promote->quantum = TIME_QUANTUMS[promote->priority];
                    enqueue(cpu, promote);
                } else {
                    prev = &item->next;
                    item = item->next;
                }
            }
        }

        // 2. MLFQ Feedback Logic
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
                        if (cpu->task_queues[i]) {
                            current->state = task_state::READY;
                            enqueue(cpu, current);
                            goto pick_next;
                        }
                    }
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

        smp::get_cpu()->tss_entry.rsp0 = (uint64_t)next->kernel_stack + KERNEL_STACK_SIZE;

        // asm volatile("mov %0, %%cr3" : : "r"(next->cr3) : "memory"); TODO

        return next->context;
    }

    void yield() { asm volatile("int $0x81"); }

}  // namespace scheduler