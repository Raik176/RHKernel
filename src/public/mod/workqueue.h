#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*kernel_work_fn_t)(void *arg);

struct kernel_work {
    kernel_work_fn_t fn;
    void *arg;
    struct kernel_work *next;
    volatile uint32_t flags;
};

struct kernel_delayed_work {
    struct kernel_work work;
    struct kernel_delayed_work *next;
    uint64_t due_tick;
    volatile uint32_t flags;
};

enum {
    KERNEL_WORK_PENDING = 1U << 0,
    KERNEL_WORK_RUNNING = 1U << 1,
};

enum {
    KERNEL_DELAYED_WORK_PENDING = 1U << 0,
};

void kernel_work_init(struct kernel_work *work, kernel_work_fn_t fn, void *arg);
int kernel_queue_work(struct kernel_work *work);
void kernel_delayed_work_init(struct kernel_delayed_work *work, kernel_work_fn_t fn, void *arg);
int kernel_queue_delayed_work(struct kernel_delayed_work *work, uint64_t delay_ticks);
int kernel_cancel_delayed_work(struct kernel_delayed_work *work);

#ifdef __cplusplus
}
#endif
