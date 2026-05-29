#include "smp/workqueue.h"

#include <stdarg.h>

#include "console.h"
#include "smp/lock.h"
#include "smp/scheduler.h"
#include "smp/apic.h"
#include "string.h"
#include "symbol.h"

namespace workqueue {
    static spinlock_t lock;
    static kernel_work *head = nullptr;
    static kernel_work *tail = nullptr;
    static scheduler::task *worker = nullptr;
    static scheduler::task *timer = nullptr;
    static kernel_delayed_work *delayed_head = nullptr;
    static bool initialized = false;
    static bool started = false;
    static stats counters;

    static void worker_main();
    static void timer_main();

    void init() {
        spinlock_init(&lock);
        memset(&counters, 0, sizeof(counters));
        initialized = true;
    }

    bool start() {
        if (!initialized) init();
        uint64_t flags;
        spinlock_acquire(&lock, &flags);
        if (started) {
            spinlock_release(&lock, flags);
            return true;
        }
        spinlock_release(&lock, flags);

        scheduler::task *t = scheduler::spawn(scheduler::task_type::KERNEL, worker_main);
        if (!t) return false;
        scheduler::task *timer_task = scheduler::spawn(scheduler::task_type::KERNEL, timer_main);
        if (!timer_task) return false;

        spinlock_acquire(&lock, &flags);
        worker = t;
        timer = timer_task;
        started = true;
        counters.worker_pid = t->id;
        spinlock_release(&lock, flags);

        scheduler::wake(t);
        scheduler::wake(timer_task);
        return true;
    }

    int queue(kernel_work *work) {
        if (!work || !work->fn) return -1;

        uint64_t flags;
        spinlock_acquire(&lock, &flags);
        if (!initialized) {
            spinlock_release(&lock, flags);
            return -3;
        }
        if (work->flags & (KERNEL_WORK_PENDING | KERNEL_WORK_RUNNING)) {
            counters.rejected++;
            spinlock_release(&lock, flags);
            return -2;
        }

        work->next = nullptr;
        work->flags = KERNEL_WORK_PENDING;
        if (tail) {
            tail->next = work;
            tail = work;
        } else {
            head = work;
            tail = work;
        }

        counters.queued++;
        counters.depth++;
        if (counters.depth > counters.max_depth) counters.max_depth = counters.depth;
        scheduler::task *w = worker;
        spinlock_release(&lock, flags);

        if (w && scheduler::wake(w)) {
            uint64_t stat_flags;
            spinlock_acquire(&lock, &stat_flags);
            counters.wakeups++;
            spinlock_release(&lock, stat_flags);
        }
        return 0;
    }

    static kernel_work *take_one() {
        kernel_work *work = head;
        if (!work) return nullptr;
        head = work->next;
        if (!head) tail = nullptr;
        work->next = nullptr;
        work->flags = KERNEL_WORK_RUNNING;
        if (counters.depth) counters.depth--;
        return work;
    }

    static void queue_ready_delayed(uint64_t now) {
        kernel_delayed_work **pp = &delayed_head;
        while (*pp) {
            kernel_delayed_work *dw = *pp;
            if (dw->due_tick > now) {
                pp = &dw->next;
                continue;
            }
            if (dw->work.flags & (KERNEL_WORK_PENDING | KERNEL_WORK_RUNNING)) {
                dw->due_tick = now + 1;
                pp = &dw->next;
                continue;
            }
            *pp = dw->next;
            dw->next = nullptr;
            dw->flags &= ~KERNEL_DELAYED_WORK_PENDING;
            dw->work.next = nullptr;
            dw->work.flags = KERNEL_WORK_PENDING;
            if (tail) {
                tail->next = &dw->work;
                tail = &dw->work;
            } else {
                head = &dw->work;
                tail = &dw->work;
            }
            counters.queued++;
            counters.depth++;
            if (counters.depth > counters.max_depth) counters.max_depth = counters.depth;
            if (worker) scheduler::wake(worker);
        }
    }

    static void timer_main() {
        for (;;) {
            uint64_t flags;
            spinlock_acquire(&lock, &flags);
            queue_ready_delayed(apic::get_global_ticks());
            spinlock_release(&lock, flags);
            scheduler::sleep(1);
        }
    }

    static void worker_main() {
        for (;;) {
            uint64_t flags;
            spinlock_acquire(&lock, &flags);
            kernel_work *work = take_one();
            if (!work) {
                counters.waits++;
                scheduler::block_current(scheduler::task_state::WAITING, &lock, flags);
                continue;
            }
            spinlock_release(&lock, flags);

            work->fn(work->arg);

            spinlock_acquire(&lock, &flags);
            work->flags = 0;
            counters.completed++;
            spinlock_release(&lock, flags);
        }
    }

    void snapshot(stats *out) {
        if (!out) return;
        uint64_t flags;
        spinlock_acquire(&lock, &flags);
        *out = counters;
        out->initialized = initialized;
        out->started = started;
        out->depth = counters.depth;
        spinlock_release(&lock, flags);
    }

    static void append(char *buf, uint64_t cap, uint64_t *pos, const char *s) {
        while (*s && *pos + 1 < cap) buf[(*pos)++] = *s++;
        if (*pos < cap) buf[*pos] = 0;
    }

    static void appendf(char *buf, uint64_t cap, uint64_t *pos, const char *fmt, ...) {
        if (!buf || !pos || *pos >= cap) return;
        char tmp[160];
        va_list args;
        va_start(args, fmt);
        int len = vsnprintf(tmp, sizeof(tmp), fmt, args);
        va_end(args);
        if (len > 0) append(buf, cap, pos, tmp);
    }

    uint64_t format_status(char *buf, uint64_t cap) {
        if (!buf || cap == 0) return 0;
        stats s;
        snapshot(&s);
        uint64_t pos = 0;
        buf[0] = 0;
        appendf(buf, cap, &pos, "initialized: %d\n", s.initialized ? 1 : 0);
        appendf(buf, cap, &pos, "started: %d\n", s.started ? 1 : 0);
        appendf(buf, cap, &pos, "worker_pid: %d\n", s.worker_pid);
        appendf(buf, cap, &pos, "depth: %d\n", s.depth);
        appendf(buf, cap, &pos, "max_depth: %d\n", s.max_depth);
        appendf(buf, cap, &pos, "queued: %d\n", s.queued);
        appendf(buf, cap, &pos, "completed: %d\n", s.completed);
        appendf(buf, cap, &pos, "rejected: %d\n", s.rejected);
        appendf(buf, cap, &pos, "wakeups: %d\n", s.wakeups);
        appendf(buf, cap, &pos, "waits: %d\n", s.waits);
        return pos;
    }
}

extern "C" void kernel_work_init(kernel_work *work, kernel_work_fn_t fn, void *arg) {
    if (!work) return;
    work->fn = fn;
    work->arg = arg;
    work->next = nullptr;
    work->flags = 0;
}

extern "C" int kernel_queue_work(kernel_work *work) { return workqueue::queue(work); }

extern "C" void kernel_delayed_work_init(kernel_delayed_work *work, kernel_work_fn_t fn, void *arg) {
    if (!work) return;
    kernel_work_init(&work->work, fn, arg);
    work->next = nullptr;
    work->due_tick = 0;
    work->flags = 0;
}

extern "C" int kernel_queue_delayed_work(kernel_delayed_work *work, uint64_t delay_ticks) {
    if (!work || !work->work.fn) return -1;
    uint64_t flags;
    spinlock_acquire(&workqueue::lock, &flags);
    if (!workqueue::initialized) {
        spinlock_release(&workqueue::lock, flags);
        return -3;
    }
    if ((work->flags & KERNEL_DELAYED_WORK_PENDING) ||
        (work->work.flags & KERNEL_WORK_PENDING)) {
        workqueue::counters.rejected++;
        spinlock_release(&workqueue::lock, flags);
        return -2;
    }
    work->due_tick = apic::get_global_ticks() + delay_ticks;
    work->flags = KERNEL_DELAYED_WORK_PENDING;

    kernel_delayed_work **pp = &workqueue::delayed_head;
    while (*pp && (*pp)->due_tick <= work->due_tick) pp = &(*pp)->next;
    work->next = *pp;
    *pp = work;

    scheduler::task *timer = workqueue::timer;
    spinlock_release(&workqueue::lock, flags);
    if (timer) scheduler::wake(timer);
    return 0;
}

extern "C" int kernel_cancel_delayed_work(kernel_delayed_work *work) {
    if (!work) return -1;
    uint64_t flags;
    spinlock_acquire(&workqueue::lock, &flags);
    kernel_delayed_work **pp = &workqueue::delayed_head;
    while (*pp) {
        if (*pp == work) {
            *pp = work->next;
            work->next = nullptr;
            work->flags &= ~KERNEL_DELAYED_WORK_PENDING;
            spinlock_release(&workqueue::lock, flags);
            return 1;
        }
        pp = &(*pp)->next;
    }
    spinlock_release(&workqueue::lock, flags);
    return 0;
}

KEXPORT(kernel_work_init)
KEXPORT(kernel_queue_work)
KEXPORT(kernel_delayed_work_init)
KEXPORT(kernel_queue_delayed_work)
KEXPORT(kernel_cancel_delayed_work)
