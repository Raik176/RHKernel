#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "smp/lock.h"

#ifdef __cplusplus
extern "C" {
#endif

struct kernel_completion {
    volatile uint32_t done;
    void *waiter;
    spinlock_t lock;
};

void kernel_completion_init(struct kernel_completion *completion, bool done);
void kernel_completion_reset(struct kernel_completion *completion);
void kernel_completion_signal(struct kernel_completion *completion);
int kernel_completion_wait(struct kernel_completion *completion);

#ifdef __cplusplus
}
#endif
