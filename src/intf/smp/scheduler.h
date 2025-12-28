#pragma once
#include <stdint.h>
#include "util.h"

struct regs;

namespace scheduler {
    static constexpr uint64_t KERNEL_STACK_SIZE = 1024 * 16;
    static constexpr uint64_t INITIAL_USER_STACK_SIZE = 1024 * 64;

    static_assert(KERNEL_STACK_SIZE % 16 == 0, "Kernel stack size must be 16-byte aligned");
    static_assert(INITIAL_USER_STACK_SIZE % 16 == 0, "User stack size must be 16-byte aligned");
    static_assert(sizeof(struct regs) <= KERNEL_STACK_SIZE, "Regs structure must fit in kernel stack");

    enum class task_state { READY, RUNNING, SLEEPING, DEAD };

    enum class task_type { KERNEL, USER };

    struct task {
        uint64_t id;
        regs* context;
        task_state state;
        task_type type;

        void* kernel_stack;
        void* user_stack;
        uint64_t cr3;

        alignas(16) uint8_t fxsave_area[512];

        int priority;
        uint64_t quantum;
        uint64_t age;

        task* next;
        task* prev;
    };

    static_assert(alignof(task::fxsave_area) == 16, "FXSAVE area must be 16-byte aligned");
    static_assert(sizeof(task::fxsave_area) == 512, "FXSAVE area must be 512 bytes");


    // MLFQ Configuration
    constexpr int MAX_QUEUES = 4;
    constexpr uint64_t TIME_QUANTUMS[MAX_QUEUES] = {2, 4, 8, 16};
    constexpr uint64_t AGING_THRESHOLD = 100;

    static_assert(sizeof(TIME_QUANTUMS) / sizeof(TIME_QUANTUMS[0]) == MAX_QUEUES, "TIME_QUANTUMS array size must match MAX_QUEUES");

    void init_core();
    task* spawn(task_type type, void (*entry_point)(), uint64_t pagemap = 0);
    void yield();

    // The core logic called by idt_handler
    extern "C" regs* schedule(regs* current_state, bool is_timer_tick);

}  // namespace scheduler