#pragma once
#include <stdint.h>

struct regs;

namespace scheduler {
    static constexpr uint64_t KERNEL_STACK_SIZE = 1024 * 16;

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

        int priority;
        uint64_t quantum;
        uint64_t age;

        task* next;
    };

    // MLFQ Configuration
    constexpr int MAX_QUEUES = 4;
    constexpr uint64_t TIME_QUANTUMS[MAX_QUEUES] = {2, 4, 8, 16};
    constexpr uint64_t AGING_THRESHOLD = 100;

    void init_core();
    void spawn(task_type type, void (*entry_point)());
    void yield();

    // The core logic called by idt_handler
    extern "C" regs* schedule(regs* current_state, bool is_timer_tick);

}  // namespace scheduler