#include "mod/scheduler.h"

#include "smp/scheduler.h"
#include "smp/apic.h"
#include "smp/smp.h"
#include "symbol.h"

static inline bool interrupts_enabled_now() {
    uint64_t flags;
    asm volatile("pushfq; popq %0" : "=r"(flags) : : "memory");
    return (flags & (1ULL << 9)) != 0;
}

extern "C" bool kernel_can_sleep(void) {
    smp::cpu_local *cpu = smp::get_cpu();
    return interrupts_enabled_now() && cpu && cpu->current_task && cpu->current_task != cpu->idle_task;
}

extern "C" int kernel_sleep_ticks(uint64_t ticks) {
    if (ticks == 0) return 0;
    if (!kernel_can_sleep()) return -1;
    scheduler::sleep(ticks);
    return 0;
}

extern "C" void kernel_yield(void) {
    if (kernel_can_sleep()) scheduler::yield();
}

extern "C" uint64_t kernel_ticks(void) {
    smp::cpu_local *cpu = smp::get_cpu();
    return cpu ? cpu->ticks : 0;
}

extern "C" uint64_t kernel_monotonic_ticks(void) {
    return apic::get_global_ticks();
}

extern "C" void kernel_wait_queue_init(kernel_wait_queue *queue) {
    if (!queue) return;
    spinlock_init(&queue->lock);
    queue->count = 0;
    queue->overflow = 0;
    queue->seq = 0;
    for (uint32_t i = 0; i < KERNEL_WAIT_QUEUE_MAX_WAITERS; i++) queue->waiters[i] = nullptr;
}

extern "C" uint64_t kernel_wait_queue_seq(kernel_wait_queue *queue) {
    if (!queue) return 0;
    uint64_t flags;
    spinlock_acquire(&queue->lock, &flags);
    uint64_t seq = queue->seq;
    spinlock_release(&queue->lock, flags);
    return seq;
}

extern "C" int kernel_wait_queue_wait_changed(kernel_wait_queue *queue, uint64_t seq) {
    if (!queue) return -1;
    if (!kernel_can_sleep()) return -2;

    for (;;) {
        uint64_t flags;
        spinlock_acquire(&queue->lock, &flags);
        if (queue->seq != seq) {
            spinlock_release(&queue->lock, flags);
            return 0;
        }

        smp::cpu_local *cpu = smp::get_cpu();
        if (!cpu || !cpu->current_task || cpu->current_task == cpu->idle_task) {
            spinlock_release(&queue->lock, flags);
            return -2;
        }

        bool present = false;
        for (uint32_t i = 0; i < queue->count; i++) {
            if (queue->waiters[i] == cpu->current_task) {
                present = true;
                break;
            }
        }
        if (!present) {
            if (queue->count < KERNEL_WAIT_QUEUE_MAX_WAITERS) {
                queue->waiters[queue->count++] = cpu->current_task;
            } else {
                queue->overflow = 1;
                spinlock_release(&queue->lock, flags);
                scheduler::sleep(1);
                continue;
            }
        }

        scheduler::block_current(scheduler::task_state::WAITING, &queue->lock, flags);
    }
}

extern "C" void kernel_wait_queue_wake_all(kernel_wait_queue *queue) {
    if (!queue) return;

    void *waiters[KERNEL_WAIT_QUEUE_MAX_WAITERS];
    uint32_t count = 0;
    uint32_t overflow = 0;

    uint64_t flags;
    spinlock_acquire(&queue->lock, &flags);
    queue->seq++;
    count = queue->count;
    overflow = queue->overflow;
    for (uint32_t i = 0; i < count; i++) {
        waiters[i] = queue->waiters[i];
        queue->waiters[i] = nullptr;
    }
    queue->count = 0;
    queue->overflow = 0;
    spinlock_release(&queue->lock, flags);

    for (uint32_t i = 0; i < count; i++) {
        scheduler::wake(static_cast<scheduler::task *>(waiters[i]));
    }
    (void)overflow;
}

KEXPORT(kernel_can_sleep)
KEXPORT(kernel_sleep_ticks)
KEXPORT(kernel_yield)
KEXPORT(kernel_ticks)
KEXPORT(kernel_monotonic_ticks)
KEXPORT(kernel_wait_queue_init)
KEXPORT(kernel_wait_queue_seq)
KEXPORT(kernel_wait_queue_wait_changed)
KEXPORT(kernel_wait_queue_wake_all)
