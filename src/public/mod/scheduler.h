#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "smp/lock.h"

#ifdef __cplusplus
extern "C" {
#endif

bool kernel_can_sleep(void);
int kernel_sleep_ticks(uint64_t ticks);
void kernel_yield(void);
uint64_t kernel_ticks(void);
uint64_t kernel_monotonic_ticks(void);

#define KERNEL_WAIT_QUEUE_MAX_WAITERS 64u

struct kernel_wait_queue {
    spinlock_t lock;
    void *waiters[KERNEL_WAIT_QUEUE_MAX_WAITERS];
    uint32_t count;
    uint32_t overflow;
    uint64_t seq;
};

void kernel_wait_queue_init(struct kernel_wait_queue *queue);
uint64_t kernel_wait_queue_seq(struct kernel_wait_queue *queue);
int kernel_wait_queue_wait_changed(struct kernel_wait_queue *queue, uint64_t seq);
void kernel_wait_queue_wake_all(struct kernel_wait_queue *queue);

#ifdef __cplusplus
}
#endif
