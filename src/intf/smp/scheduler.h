#pragma once
#include <stdint.h>

#include "file/vfs.h"
#include "util.h"

namespace scheduler {
    static constexpr uint64_t KERNEL_STACK_SIZE = 1024 * 8;
    static constexpr uint64_t USER_STACK_TOP = 0x00007FFFFFFFF000ULL;
    static constexpr uint64_t MAX_USER_STACK_SIZE = 8ULL * 1024 * 1024;
    static constexpr uint64_t INITIAL_USER_STACK_SIZE = 128ULL * 1024;
    static constexpr uint32_t INITIAL_FD_CAPACITY = 8;

    static_assert(KERNEL_STACK_SIZE % 16 == 0, "Kernel stack size must be 16-byte aligned");
    static_assert(USER_STACK_TOP % 4096 == 0, "User stack top must be page-aligned");
    static_assert(MAX_USER_STACK_SIZE % 4096 == 0, "Max user stack size must be page-aligned");
    static_assert(INITIAL_USER_STACK_SIZE % 4096 == 0, "Initial user stack size must be page-aligned");
    static_assert(INITIAL_USER_STACK_SIZE <= MAX_USER_STACK_SIZE, "Initial user stack must fit in max stack");
    static_assert(INITIAL_USER_STACK_SIZE % 16 == 0, "User stack size must be 16-byte aligned");

    enum class task_state { READY, RUNNING, SLEEPING, WAITING, ZOMBIE, BLOCKED };
    enum class task_type { KERNEL, USER };

    struct task {
        uint64_t id;
        regs *context;
        task_state state;
        task_type type;

        void *kernel_stack;

        // High userspace stack. user_stack is the lowest currently committed
        // address. user_stack_limit is the bottom of the reserved stack window;
        // faults below it are real bugs. user_stack_top is the exclusive top.
        void *user_stack;
        uint64_t user_stack_limit;
        uint64_t user_stack_top;

        uint64_t cr3;
        uint64_t wakeup_time;

        uint64_t heap_start;
        uint64_t program_break;
        uint64_t mmap_next;

        task *parent;
        task *first_child;
        task *next_sibling;

        int exit_code;

        vfs::open_file **fd_table;
        uint64_t fd_capacity;

        alignas(16) uint8_t fxsave_area[512];

        int priority;
        uint64_t quantum;
        uint64_t age;

        task *next, *prev;
        task *global_next, *global_prev;
    };

    static_assert(alignof(task::fxsave_area) == 16, "FXSAVE area must be 16-byte aligned");
    static_assert(sizeof(task::fxsave_area) == 512, "FXSAVE area must be 512 bytes");
    static_assert(offsetof(task, kernel_stack) == 24,
                  "Assembly syscall_entry expects task.kernel_stack at offset 24");

    // MLFQ Configuration
    constexpr int MAX_QUEUES = 4;
    constexpr uint64_t TIME_QUANTUMS[MAX_QUEUES] = {2, 4, 8, 16};
    constexpr uint64_t AGING_THRESHOLD = 100;

    static_assert(sizeof(TIME_QUANTUMS) / sizeof(TIME_QUANTUMS[0]) == MAX_QUEUES,
                  "TIME_QUANTUMS array size must match MAX_QUEUES");

    void init_core();
    task *spawn(task_type type, void (*entry_point)(), uint64_t pagemap = 0, uint64_t heap_start = 0);
    void yield();
    void sleep(uint64_t ticks);
    void exit(int code);
    int wait(int *status);

    task *get_task_by_id(uint64_t id);
    void dump_task(task *t);
    void dump_all_tasks();

    int clone(uint64_t flags, void *child_stack, regs *current_regs);
    int exec(const char *path, char **argv, regs *return_frame);

    extern "C" void context_switch(regs *regs);
    extern "C" void schedule(regs *current_state, bool is_timer_tick);

}  // namespace scheduler