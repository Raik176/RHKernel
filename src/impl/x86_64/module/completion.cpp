#include "mod/completion.h"

#include "mod/scheduler.h"
#include "smp/scheduler.h"
#include "smp/smp.h"
#include "symbol.h"

extern "C" void kernel_completion_init(kernel_completion *completion, bool done) {
    if (!completion) return;
    completion->done = done ? 1 : 0;
    completion->waiter = nullptr;
    spinlock_init(&completion->lock);
}

extern "C" void kernel_completion_reset(kernel_completion *completion) {
    if (!completion) return;
    uint64_t flags;
    spinlock_acquire(&completion->lock, &flags);
    completion->done = 0;
    completion->waiter = nullptr;
    spinlock_release(&completion->lock, flags);
}

extern "C" void kernel_completion_signal(kernel_completion *completion) {
    if (!completion) return;

    scheduler::task *waiter = nullptr;
    uint64_t flags;
    spinlock_acquire(&completion->lock, &flags);
    completion->done = 1;
    waiter = static_cast<scheduler::task *>(completion->waiter);
    completion->waiter = nullptr;
    spinlock_release(&completion->lock, flags);

    if (waiter) scheduler::wake(waiter);
}

extern "C" int kernel_completion_wait(kernel_completion *completion) {
    if (!completion) return -1;
    if (!kernel_can_sleep()) return -2;

    for (;;) {
        uint64_t flags;
        spinlock_acquire(&completion->lock, &flags);
        if (completion->done) {
            completion->done = 0;
            completion->waiter = nullptr;
            spinlock_release(&completion->lock, flags);
            return 0;
        }

        smp::cpu_local *cpu = smp::get_cpu();
        if (!cpu || !cpu->current_task || cpu->current_task == cpu->idle_task) {
            spinlock_release(&completion->lock, flags);
            return -2;
        }
        completion->waiter = cpu->current_task;
        scheduler::block_current(scheduler::task_state::WAITING, &completion->lock, flags);
    }
}

KEXPORT(kernel_completion_init)
KEXPORT(kernel_completion_reset)
KEXPORT(kernel_completion_signal)
KEXPORT(kernel_completion_wait)
