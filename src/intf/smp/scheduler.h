#pragma once
#include <stdint.h>

#include "file/elf.h"
#include "file/vfs.h"
#include "event.h"
#include "smp/lock.h"
#include "util.h"

namespace scheduler {
    static constexpr uint64_t KERNEL_STACK_SIZE = 1024 * 16;
    static constexpr uint64_t USER_STACK_TOP = 0x00007FFFFFFFF000ULL;
    static constexpr uint64_t USER_STACK_ASLR_WINDOW = 256ULL * 1024 * 1024;
    static constexpr uint64_t MAX_USER_STACK_SIZE = 8ULL * 1024 * 1024;
    static constexpr uint64_t INITIAL_USER_STACK_SIZE = 128ULL * 1024;
    static constexpr uint64_t INITIAL_USER_STACK_RESIDENT_MIN = 16ULL * 1024;
    static constexpr uint64_t USER_MMAP_BASE_MIN = 0x0000200000000000ULL;
    static constexpr uint64_t USER_MMAP_ASLR_WINDOW = 0x0000100000000000ULL;
    static constexpr uint64_t USER32_TOP = 0x00000000F0000000ULL;
    static constexpr uint64_t USER32_MMAP_BASE_MIN = 0x40000000ULL;
    static constexpr uint64_t USER32_MMAP_ASLR_WINDOW = 0x20000000ULL;
    static constexpr uint32_t INITIAL_FD_CAPACITY = 8;

    static_assert(KERNEL_STACK_SIZE % 16 == 0, "Kernel stack size must be 16-byte aligned");
    static_assert(USER_STACK_TOP % 4096 == 0, "User stack top must be page-aligned");
    static_assert(USER_STACK_ASLR_WINDOW % 4096 == 0, "Stack ASLR window must be page-aligned");
    static_assert(MAX_USER_STACK_SIZE % 4096 == 0, "Max user stack size must be page-aligned");
    static_assert(INITIAL_USER_STACK_SIZE % 4096 == 0, "Initial user stack size must be page-aligned");
    static_assert(INITIAL_USER_STACK_SIZE <= MAX_USER_STACK_SIZE, "Initial user stack must fit in max stack");
    static_assert(INITIAL_USER_STACK_RESIDENT_MIN % 4096 == 0, "Resident user stack must be page-aligned");
    static_assert(INITIAL_USER_STACK_RESIDENT_MIN <= INITIAL_USER_STACK_SIZE, "Resident user stack must fit initial stack");
    static_assert(MAX_USER_STACK_SIZE < USER_STACK_ASLR_WINDOW, "Stack must fit inside ASLR window");
    static_assert(USER_MMAP_BASE_MIN % 4096 == 0, "mmap base must be page-aligned");
    static_assert(USER_MMAP_ASLR_WINDOW % 4096 == 0, "mmap ASLR window must be page-aligned");
    static_assert(INITIAL_USER_STACK_SIZE % 16 == 0, "User stack size must be 16-byte aligned");

    enum class task_state { READY, RUNNING, SLEEPING, WAITING, ZOMBIE, BLOCKED, FROZEN };

    struct event_queue {
        task_event entries[TASK_EVENT_QUEUE_SIZE];
        uint32_t head;
        uint32_t count;
        uint32_t dropped;
        uint32_t reserved;
    };

    struct fault_state {
        task_faultctl ctl;
        task_fault_frame frame;
        task_fault_return result;
        regs saved_regs;
        bool active;
        bool return_pending;
        uint32_t depth;
    };
    enum class task_type { KERNEL, USER };
    enum class task_abi { KERNEL, USER64, USER32 };
    enum class vma_type { IMAGE, HEAP, STACK, ANON, DEVICE };

    enum vma_flags : uint32_t {
        VMA_READ = 1U << 0,
        VMA_WRITE = 1U << 1,
        VMA_EXEC = 1U << 2,
        VMA_GROWSDOWN = 1U << 3,
    };

    struct vm_area {
        uint64_t start;
        uint64_t end;
        uint64_t committed_start;
        uint32_t flags;
        uint8_t pkey;
        vma_type type;
        vm_area *next;
    };

    struct task {
        uint64_t id;
        regs *context;
        task_state state;
        task_type type;
        task_abi abi;

        void *kernel_stack;

        uint64_t cr3;
        uint64_t wakeup_time;

        vm_area *vma_list;
        vm_area *heap_vma;
        vm_area *stack_vma;
        uint64_t program_break;
        uint64_t mmap_hint;

        task *parent;
        task *first_child;
        task *next_sibling;

        int exit_code;

        vfs::open_file **fd_table;
        uint64_t *fd_bitmap;
        uint64_t fd_capacity;
        uint64_t next_fd_hint;
        void *syscall_iobuf;
        vfs::vfs_node *cwd;
        char *cwd_path;
        uint64_t cwd_path_len;
        char **launch_search;
        uint64_t launch_search_count;

        event_queue events;
        fault_state fault;
        bool kill_pending;
        int kill_code;
        uint64_t kill_source;

        void *fpu_area;
        void *fpu_storage;
        uint64_t fs_base;
        uint64_t gs_base;
        uint32_t pkru;

        int priority;
        uint64_t quantum;
        uint64_t enqueue_tick;
        uint64_t last_cpu_index;

        task *next, *prev;
        task *global_next, *global_prev;
    };

    static_assert(offsetof(task, kernel_stack) == 32,
                  "task.kernel_stack offset changed");

    constexpr int MAX_QUEUES = 4;
    constexpr uint64_t TIME_QUANTUMS[MAX_QUEUES] = {2, 4, 8, 16};
    constexpr uint64_t AGING_THRESHOLD = 100;

    static_assert(sizeof(TIME_QUANTUMS) / sizeof(TIME_QUANTUMS[0]) == MAX_QUEUES,
                  "TIME_QUANTUMS array size must match MAX_QUEUES");

    void init_core();
    task *spawn(task_type type, void (*entry_point)(), uint64_t pagemap = 0, uint64_t heap_start = 0,
                const elf::elf_info *image = nullptr);
    task *spawn_init(void (*entry_point)(), uint64_t pagemap, uint64_t heap_start,
                     const elf::elf_info *image = nullptr);
    bool unfreeze(task *t);
    void yield();
    void sleep(uint64_t ticks);
    bool block_current(task_state state, spinlock_t *release_lock, uint64_t release_flags);
    bool wake(task *t);
    void exit(int code);
    int wait(int *status);

    task *get_task_by_id(uint64_t id);
    bool task_id_by_index(uint64_t index, uint64_t *id_out);
    uint64_t format_task_list(char *buf, uint64_t cap, bool detail, uint64_t only_pid);
    uint64_t format_task_status(char *buf, uint64_t cap, uint64_t pid);
    uint64_t format_task_maps(char *buf, uint64_t cap, uint64_t pid);
    uint64_t format_task_fds(char *buf, uint64_t cap, uint64_t pid);
    uint64_t format_launch_search(char *buf, uint64_t cap, uint64_t pid);
    bool set_launch_search_from_text(uint64_t pid, const char *text, uint64_t size);
    uint64_t read_event(uint64_t pid, task_event *out, uint64_t count, bool block);
    uint64_t send_event(uint64_t pid, const task_event *events, uint64_t count);
    bool read_faultctl(uint64_t pid, task_faultctl *out);
    bool write_faultctl(uint64_t pid, const task_faultctl *ctl);
    uint64_t read_fault(uint64_t pid, task_fault_frame *out, uint64_t count);
    bool write_fault_return(uint64_t pid, const task_fault_return *result);
    bool handle_user_exception(regs *r, uint32_t type, uint64_t detail);
    bool complete_fault_return_if_pending(regs *r);
    bool apply_pending_kill(regs *r);
    char *resolve_launch_exec(task *t, const char *name);
    vm_area *add_vma(task *t, uint64_t start, uint64_t end, uint64_t committed_start,
                     uint32_t flags, vma_type type);
    void clear_vmas(task *t);
    vm_area *find_vma(task *t, uint64_t addr);
    vm_area *find_vma_exact(task *t, uint64_t start, uint64_t end);
    bool vma_range_free(task *t, uint64_t start, uint64_t end);
    bool remove_vma(task *t, uint64_t start, uint64_t end);
    bool clone_vmas(task *dst, const task *src);
    void dump_task(task *t);
    void dump_all_tasks();

    int clone(uint64_t flags, void *child_stack, regs *current_regs);
    int exec(const char *path, char **argv, regs *return_frame);

    extern "C" void context_switch(regs *regs) __attribute__((noreturn));
    extern "C" void schedule(regs *current_state, bool is_timer_tick) __attribute__((noreturn));

}  // namespace scheduler