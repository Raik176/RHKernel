#include "smp/scheduler.h"

#include "console.h"
#include "file/elf.h"
#include "file/fd.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "smp/apic.h"
#include "smp/smp.h"
#include "string.h"

extern "C" void scheduler_yield_context();

extern "C" void scheduler_schedule_from_context(regs *current_state) {
    scheduler::schedule(current_state, false);
}

namespace scheduler {
    static uint64_t next_tid = 1;
    static task *all_tasks_head = nullptr;

    static void enqueue(smp::cpu_local *cpu, task *t);

    static const char *state_name(task_state state) {
        switch (state) {
            case task_state::READY: return "READY";
            case task_state::RUNNING: return "RUNNING";
            case task_state::SLEEPING: return "SLEEPING";
            case task_state::WAITING: return "WAITING";
            case task_state::ZOMBIE: return "ZOMBIE";
            case task_state::BLOCKED: return "BLOCKED";
        }
        return "UNKNOWN";
    }

    static const char *type_name(task_type type) {
        switch (type) {
            case task_type::KERNEL: return "KERNEL";
            case task_type::USER: return "USER";
        }
        return "UNKNOWN";
    }

    static task *find_init_task() {
        for (task *t = all_tasks_head; t; t = t->global_next) {
            if (t->id == 1) return t;
        }
        return nullptr;
    }

    static bool address_space_used_by_other(uint64_t cr3, task *owner) {
        if (cr3 == 0 || cr3 == vmm::get_kernel_pagemap()) return true;
        for (task *t = all_tasks_head; t; t = t->global_next) {
            if (t != owner && t->cr3 == cr3) return true;
        }
        return false;
    }

    static void detach_from_parent(task *t) {
        if (!t || !t->parent) return;
        task **link = &t->parent->first_child;
        while (*link) {
            if (*link == t) {
                *link = t->next_sibling;
                break;
            }
            link = &(*link)->next_sibling;
        }
        t->parent = nullptr;
        t->next_sibling = nullptr;
    }

    static void reparent_children(task *old_parent) {
        if (!old_parent || !old_parent->first_child) return;

        task *adopter = find_init_task();
        if (adopter == old_parent) adopter = nullptr;

        task *child = old_parent->first_child;
        old_parent->first_child = nullptr;

        while (child) {
            task *next = child->next_sibling;
            child->parent = adopter;
            if (adopter) {
                child->next_sibling = adopter->first_child;
                adopter->first_child = child;
            } else {
                child->next_sibling = nullptr;
            }
            child = next;
        }

        if (adopter && adopter->state == task_state::BLOCKED) {
            adopter->state = task_state::READY;
            enqueue(smp::get_cpu(), adopter);
        }
    }

    static void free_task_resources(task *t) {
        if (!t) return;

        if (t->type == task_type::USER && t->cr3 != 0 && t->cr3 != vmm::get_kernel_pagemap() &&
            !address_space_used_by_other(t->cr3, t)) {
            vmm::destroy_user_address_space(t->cr3);
        }

        if (t->kernel_stack) heap::kfree(t->kernel_stack);
        if (t->fd_table) heap::kfree(t->fd_table);
        heap::kfree(t);
    }

    static void link_global(task *t) {
        t->global_prev = nullptr;
        t->global_next = all_tasks_head;
        if (all_tasks_head) { all_tasks_head->global_prev = t; }
        all_tasks_head = t;
    }

    static void unlink_global(task *t) {
        if (t->global_prev)
            t->global_prev->global_next = t->global_next;
        else if (all_tasks_head == t)
            all_tasks_head = t->global_next;

        if (t->global_next) { t->global_next->global_prev = t->global_prev; }
        t->global_next = nullptr;
        t->global_prev = nullptr;
    }

    static inline void fxsave_task(task *t) {
        asm volatile("fxsave (%0)" : : "r"(t->fxsave_area) : "memory");
    }
    static inline void fxrstor_task(task *t) {
        asm volatile("fxrstor (%0)" : : "r"(t->fxsave_area) : "memory");
    }

    static void enqueue(smp::cpu_local *cpu, task *t) {
        if (t->state != task_state::READY) {
            console::printf("[SCHED] Tried to enqueue not-ready task %d", (uint64_t)t->id);
        }

        int p = t->priority;
        t->next = nullptr;
        t->prev = cpu->task_queues_tail[p];

        if (!cpu->task_queues_head[p]) {
            cpu->task_queues_head[p] = t;
            cpu->task_queues_tail[p] = t;
        } else {
            cpu->task_queues_tail[p]->next = t;
            cpu->task_queues_tail[p] = t;
        }
    }

    static task *dequeue(smp::cpu_local *cpu) {
        for (int i = 0; i < MAX_QUEUES; i++) {
            if (cpu->task_queues_head[i]) {
                task *t = cpu->task_queues_head[i];
                cpu->task_queues_head[i] = t->next;

                if (cpu->task_queues_head[i]) {
                    cpu->task_queues_head[i]->prev = nullptr;
                } else {
                    cpu->task_queues_tail[i] = nullptr;
                }

                t->next = nullptr;
                t->prev = nullptr;
                return t;
            }
        }

        return cpu->idle_task;
    }

    void init_core() {
        smp::cpu_local *cpu = smp::get_cpu();

        task *idle = (task *)heap::kmalloc(sizeof(task));
        memset(idle, 0, sizeof(task));

        idle->id = 0;
        idle->priority = MAX_QUEUES - 1;
        idle->cr3 = vmm::get_kernel_pagemap();
        idle->state = task_state::RUNNING;
        idle->type = task_type::KERNEL;
        idle->kernel_stack = heap::kmalloc(KERNEL_STACK_SIZE);

        link_global(idle);

        cpu->idle_task = idle;
        cpu->current_task = idle;
        cpu->kernel_stack = (void *)((uintptr_t)idle->kernel_stack + KERNEL_STACK_SIZE);
        cpu->tss_entry.rsp0 = (uint64_t)cpu->kernel_stack;
    }

    task *spawn(task_type type, void (*entry_point)(), uint64_t pml4, uint64_t heap_start) {
        if (type == task_type::USER && pml4 == 0) { kpanic("User task requires a valid PML4"); }

        smp::cpu_local *cpu = smp::get_cpu();
        task *t = (task *)heap::kmalloc(sizeof(task));
        memset(t, 0, sizeof(task));

        void *kstack = heap::kmalloc(KERNEL_STACK_SIZE);
        t->kernel_stack = kstack;

        regs *r = (regs *)((uintptr_t)kstack + KERNEL_STACK_SIZE - sizeof(regs));
        memset(r, 0, sizeof(regs));

        t->type = type;
        t->id = next_tid++;
        t->context = r;

        t->heap_start = heap_start;
        t->program_break = heap_start;
        t->mmap_next = 0x0000400000000000ULL;

        t->parent = cpu->current_task;
        if (t->parent) {
            t->next_sibling = t->parent->first_child;
            t->parent->first_child = t;
        }

        t->fd_capacity = INITIAL_FD_CAPACITY;
        t->fd_table = (vfs::open_file **)heap::kmalloc(sizeof(vfs::open_file *) * t->fd_capacity);
        memset(t->fd_table, 0, sizeof(vfs::open_file *) * t->fd_capacity);

        if (t->id == 1) {
            vfs::vfs_node *con = vfs::open("/dev/console");
            if (con) {
                vfs::open_file *file = (vfs::open_file *)heap::kmalloc(sizeof(vfs::open_file));
                file->node = con;
                file->offset = 0;
                file->ref_count = 2;
                spinlock_init(&file->lock);

                t->fd_table[0] = nullptr;  // stdin
                t->fd_table[1] = file;     // stdout
                t->fd_table[2] = file;     // stderr
            }
        }

        if (type == task_type::USER) {
            uint64_t stack_pages = INITIAL_USER_STACK_SIZE / pmm::PAGE_SIZE;
            uint64_t stack_phys = pmm::alloc(stack_pages * pmm::PAGE_SIZE);
            uint64_t stack_virt = USER_STACK_TOP - INITIAL_USER_STACK_SIZE;

            vmm::map_range(stack_virt, stack_phys, INITIAL_USER_STACK_SIZE,
                           vmm::PageFlags::Write | vmm::PageFlags::User, pml4);

            t->user_stack = (void *)stack_virt;
            t->user_stack_limit = USER_STACK_TOP - MAX_USER_STACK_SIZE;
            t->user_stack_top = USER_STACK_TOP;
            r->rip = (uint64_t)entry_point;
            r->rsp = USER_STACK_TOP - 8;
            r->cs = gdt::selectors::UCODE64_SEL;  // User Code
            r->ss = gdt::selectors::UDATA64_SEL;  // User Data
            t->cr3 = pml4;
        } else {
            r->rip = (uint64_t)entry_point;
            r->rsp = (uint64_t)kstack + KERNEL_STACK_SIZE;
            r->cs = gdt::selectors::KCODE_SEL;  // Kernel Code
            r->ss = gdt::selectors::KDATA_SEL;  // Kernel Data
            t->cr3 = pml4 == 0 ? vmm::get_kernel_pagemap() : pml4;
        }

        r->rflags = 0x202;  // IF = 1
        t->state = task_state::READY;
        t->priority = 0;
        t->quantum = TIME_QUANTUMS[0];

        asm volatile("fninit; fxsave (%0)" : : "r"(t->fxsave_area) : "memory");

        uint64_t flags;
        cpu->sched_lock.acquire(flags);
        link_global(t);
        enqueue(cpu, t);
        cpu->sched_lock.release(flags);

        return t;
    }

    int clone(uint64_t flags, void *child_stack, regs *current_regs) {
        smp::cpu_local *cpu = smp::get_cpu();
        task *parent = cpu->current_task;

        // 1. Allocate and copy the task structure
        task *child = (task *)heap::kmalloc(sizeof(task));
        memcpy(child, parent, sizeof(task));

        child->id = next_tid++;
        child->parent = parent;
        child->first_child = nullptr;
        child->state = task_state::READY;

        child->next = nullptr;
        child->prev = nullptr;
        child->global_next = nullptr;
        child->global_prev = nullptr;

        // 2. Handle Address Space (CoW vs Shared)
        if (flags & 0x1) {  // CLONE_VM
            child->cr3 = parent->cr3;
        } else {
            child->cr3 = vmm::clone_address_space(parent->cr3);
        }

        // 3. Handle File Descriptors. CLONE_FILES is deliberately not shared until
        // descriptor-table refcounts exist. Sharing the raw table corrupts the parent
        // on child exit.
        child->fd_table =
            (vfs::open_file **)heap::kmalloc(sizeof(vfs::open_file *) * parent->fd_capacity);
        child->fd_capacity = parent->fd_capacity;
        for (uint32_t i = 0; i < parent->fd_capacity; i++) {
            child->fd_table[i] = parent->fd_table[i];
            if (child->fd_table[i]) {
                uint64_t file_flags;
                spinlock_acquire(&child->fd_table[i]->lock, &file_flags);
                child->fd_table[i]->ref_count++;
                spinlock_release(&child->fd_table[i]->lock, file_flags);
            }
        }

        // 4. Set up Kernel Stack and Context
        child->kernel_stack = heap::kmalloc(KERNEL_STACK_SIZE);

        // We need to find where the regs struct sits relative to the parent's stack base
        uint64_t stack_offset = (uintptr_t)current_regs - (uintptr_t)parent->kernel_stack;
        child->context = (regs *)((uintptr_t)child->kernel_stack + stack_offset);

        // Copy the registers (the snapshot of the CPU state)
        memcpy(child->context, current_regs, sizeof(regs));

        // 5. Adjust for the Child
        if (child_stack) { child->context->rsp = (uint64_t)child_stack; }

        // The child must return 0 from the syscall
        child->context->rax = 0;

        // 6. Link into the process tree
        child->next_sibling = parent->first_child;
        parent->first_child = child;

        // 7. Initialize FXSAVE area (copy from parent)
        memcpy(child->fxsave_area, parent->fxsave_area, 512);

        // 8. Enqueue child
        uint64_t f;
        cpu->sched_lock.acquire(f);
        link_global(child);
        enqueue(cpu, child);
        cpu->sched_lock.release(f);

        return child->id;
    }

    struct exec_arg {
        char *data;
        size_t len;
    };

    static bool bounded_strlen(const char *s, size_t max_len, size_t *out_len) {
        if (!s || !out_len || max_len == 0) return false;
        for (size_t i = 0; i < max_len; i++) {
            if (s[i] == 0) {
                *out_len = i + 1;
                return true;
            }
        }
        return false;
    }

    static void free_exec_args(exec_arg *args, int argc) {
        if (!args) return;
        for (int i = 0; i < argc; i++) heap::kfree(args[i].data);
        heap::kfree(args);
    }

    static bool copy_exec_args(char **argv, exec_arg **out_args, int *out_argc) {
        constexpr int MAX_EXEC_ARGC = 64;
        constexpr size_t MAX_EXEC_ARG_LEN = 4096;

        if (!out_args || !out_argc) return false;
        *out_args = nullptr;
        *out_argc = 0;
        if (!argv) return true;

        int argc = 0;
        while (argc < MAX_EXEC_ARGC && argv[argc]) argc++;
        if (argc == MAX_EXEC_ARGC && argv[argc]) return false;

        exec_arg *args = (exec_arg *)heap::kmalloc(sizeof(exec_arg) * argc);
        if (argc != 0 && !args) return false;
        if (argc != 0) memset(args, 0, sizeof(exec_arg) * argc);

        for (int i = 0; i < argc; i++) {
            size_t len = 0;
            if (!bounded_strlen(argv[i], MAX_EXEC_ARG_LEN, &len)) {
                free_exec_args(args, argc);
                return false;
            }

            args[i].data = (char *)heap::kmalloc(len);
            if (!args[i].data) {
                free_exec_args(args, argc);
                return false;
            }
            memcpy(args[i].data, argv[i], len);
            args[i].len = len;
        }

        *out_args = args;
        *out_argc = argc;
        return true;
    }

    static void *user_stack_to_kernel(uint64_t stack_phys, uint64_t stack_base, uint64_t addr,
                                      uint64_t len) {
        if (addr < stack_base || len > USER_STACK_TOP - addr) return nullptr;
        uint64_t off = addr - stack_base;
        if (off >= INITIAL_USER_STACK_SIZE || len > INITIAL_USER_STACK_SIZE - off) return nullptr;
        return (void *)((uintptr_t)p2v(stack_phys) + off);
    }

    int exec(const char *path, char **argv, regs *return_frame) {
        smp::cpu_local *cpu = smp::get_cpu();
        task *current = cpu ? cpu->current_task : nullptr;
        if (!current || !path || !return_frame) {
            console::printf("[EXEC] bad call current=%p path=%p frame=%p\n", current, path, return_frame);
            return -1;
        }

        exec_arg *args = nullptr;
        int argc = 0;
        if (!copy_exec_args(argv, &args, &argc)) {
            console::printf("[EXEC] argv copy failed for %s argv=%p\n", path, argv);
            return -1;
        }

        elf::elf_info info = elf::load(path);
        if (info.entry == 0 || info.pml4 == 0) {
            console::printf("[EXEC] elf load failed for %s entry=%p pml4=%p argc=%d\n", path,
                            info.entry, info.pml4, (uint64_t)argc);
            free_exec_args(args, argc);
            return -1;
        }

        uint64_t stack_pages = INITIAL_USER_STACK_SIZE / pmm::PAGE_SIZE;
        uint64_t stack_phys = pmm::alloc(stack_pages * pmm::PAGE_SIZE);
        if (!stack_phys) {
            console::printf("[EXEC] stack allocation failed for %s\n", path);
            free_exec_args(args, argc);
            vmm::destroy_user_address_space(info.pml4);
            return -1;
        }
        memset(p2v(stack_phys), 0, INITIAL_USER_STACK_SIZE);

        uint64_t stack_virt = USER_STACK_TOP - INITIAL_USER_STACK_SIZE;
        vmm::map_range(stack_virt, stack_phys, INITIAL_USER_STACK_SIZE,
                       vmm::PageFlags::Write | vmm::PageFlags::User, info.pml4);

        constexpr int MAX_EXEC_ARGC = 64;
        uint64_t stack_ptr = USER_STACK_TOP;
        uint64_t user_argv_ptrs[MAX_EXEC_ARGC + 1];
        memset(user_argv_ptrs, 0, sizeof(user_argv_ptrs));

        for (int i = argc - 1; i >= 0; i--) {
            size_t len = args[i].len;
            if (len == 0 || len > stack_ptr - stack_virt) {
                console::printf("[EXEC] arg %d does not fit for %s len=%d stack_left=%d\n",
                                (uint64_t)i, path, len, stack_ptr - stack_virt);
                free_exec_args(args, argc);
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }

            stack_ptr -= len;
            void *dst = user_stack_to_kernel(stack_phys, stack_virt, stack_ptr, len);
            if (!dst) {
                console::printf("[EXEC] arg stack translation failed for %s ptr=%p len=%d\n",
                                path, stack_ptr, len);
                free_exec_args(args, argc);
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }
            memcpy(dst, args[i].data, len);
            user_argv_ptrs[i] = stack_ptr;
        }
        user_argv_ptrs[argc] = 0;
        free_exec_args(args, argc);

        stack_ptr &= ~0xFULL;
        if (((argc + 1) & 1) == 0) {
            stack_ptr -= sizeof(uint64_t);
            void *slot = user_stack_to_kernel(stack_phys, stack_virt, stack_ptr, sizeof(uint64_t));
            if (!slot) {
                console::printf("[EXEC] align slot translation failed for %s ptr=%p\n", path,
                                stack_ptr);
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }
            *(uint64_t *)slot = 0;
        }

        for (int i = argc; i >= 0; i--) {
            stack_ptr -= sizeof(uint64_t);
            void *slot = user_stack_to_kernel(stack_phys, stack_virt, stack_ptr, sizeof(uint64_t));
            if (!slot) {
                console::printf("[EXEC] argv slot translation failed for %s ptr=%p index=%d\n",
                                path, stack_ptr, (uint64_t)i);
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }
            *(uint64_t *)slot = user_argv_ptrs[i];
        }
        uint64_t argv_ptr_for_rdi = stack_ptr;

        uint64_t old_cr3 = current->cr3;
        current->cr3 = info.pml4;
        current->heap_start = info.heap_start;
        current->program_break = info.heap_start;
        current->mmap_next = 0x0000400000000000ULL;
        current->user_stack = (void *)stack_virt;
        current->user_stack_limit = USER_STACK_TOP - MAX_USER_STACK_SIZE;
        current->user_stack_top = USER_STACK_TOP;

        regs *r = return_frame;
        current->context = r;
        memset(r, 0, sizeof(regs));

        r->rip = info.entry;
        r->rsp = stack_ptr;
        r->rdi = (uint64_t)argc;
        r->rsi = argv_ptr_for_rdi;
        r->rax = 0;
        r->rflags = 0x202;
        r->cs = gdt::selectors::UCODE64_SEL;
        r->ss = gdt::selectors::UDATA64_SEL;

        asm volatile("fninit; fxsave (%0)" : : "r"(current->fxsave_area) : "memory");
        asm volatile("mov %0, %%cr3" : : "r"(current->cr3) : "memory");

        if (old_cr3 != vmm::get_kernel_pagemap() && !address_space_used_by_other(old_cr3, current)) {
            vmm::destroy_user_address_space(old_cr3);
        }

        return 0;
    }

    void schedule(regs *current_state, bool is_timer_tick) {
        smp::cpu_local *cpu = smp::get_cpu();
        if (!cpu) context_switch(current_state);

        uint64_t flags;
        cpu->sched_lock.acquire(flags);

        if (is_timer_tick) {
            task *prev_sleep = nullptr;
            task *sleep_item = cpu->sleep_list_head;
            while (sleep_item) {
                if (cpu->ticks >= sleep_item->wakeup_time) {
                    task *to_wake = sleep_item;
                    if (prev_sleep)
                        prev_sleep->next = sleep_item->next;
                    else
                        cpu->sleep_list_head = sleep_item->next;

                    sleep_item = sleep_item->next;
                    to_wake->state = task_state::READY;
                    enqueue(cpu, to_wake);
                    continue;
                }
                prev_sleep = sleep_item;
                sleep_item = sleep_item->next;
            }
        }

        task *current = cpu->current_task;
        if (current) {
            current->context = current_state;
            fxsave_task(current);
        }

        for (int i = 1; i < MAX_QUEUES; i++) {
            task *item = cpu->task_queues_head[i];
            while (item) {
                task *next_item = item->next;
                item->age++;
                if (item->age > AGING_THRESHOLD) {
                    if (item->prev)
                        item->prev->next = item->next;
                    else
                        cpu->task_queues_head[i] = item->next;
                    if (item->next)
                        item->next->prev = item->prev;
                    else
                        cpu->task_queues_tail[i] = item->prev;

                    item->priority = i - 1;
                    item->age = 0;
                    item->quantum = TIME_QUANTUMS[item->priority];
                    enqueue(cpu, item);
                }
                item = next_item;
            }
        }

        if (current && current != cpu->idle_task && current->state == task_state::RUNNING) {
            if (is_timer_tick) {
                current->quantum--;
                if (current->quantum == 0) {
                    if (current->priority < MAX_QUEUES - 1) current->priority++;
                    current->quantum = TIME_QUANTUMS[current->priority];
                    current->state = task_state::READY;
                    enqueue(cpu, current);
                } else {
                    for (int i = 0; i < current->priority; i++) {
                        if (cpu->task_queues_head[i]) {
                            current->state = task_state::READY;
                            enqueue(cpu, current);
                            goto pick_next;
                        }
                    }
                    cpu->sched_lock.release(flags);
                    context_switch(current_state);
                }
            } else {
                current->state = task_state::READY;
                enqueue(cpu, current);
            }
        }

    pick_next:
        task *next = dequeue(cpu);
        next->state = task_state::RUNNING;
        cpu->current_task = next;

        fxrstor_task(next);

        uint64_t kstack_top = (uint64_t)next->kernel_stack + KERNEL_STACK_SIZE;
        cpu->tss_entry.rsp0 = kstack_top;
        cpu->kernel_stack = (void *)kstack_top;

        if (next->cr3 != 0) { asm volatile("mov %0, %%cr3" : : "r"(next->cr3) : "memory"); }

        cpu->sched_lock.release(flags);

        context_switch(next->context);
    }

    task *get_task_by_id(uint64_t id) {
        for (task *t = all_tasks_head; t; t = t->global_next) {
            if (t->id == id) { return t; }
        }
        return nullptr;
    }

    void dump_task(task *t) {
        if (!t) {
            console::printf("  TASK  : <none>\n");
            return;
        }

        console::printf(
            "  TASK  : id=%d state=%s type=%s parent=%d cr3=%p kstack=%p ctx=%p\n", (uint64_t)t->id,
            state_name(t->state), type_name(t->type), t->parent ? (uint64_t)t->parent->id : 0, t->cr3,
            t->kernel_stack, t->context);
        console::printf("          prio=%d quantum=%d age=%d wakeup=%d exit=%d heap=[%p..%p] mmap_next=%p\n",
                        (uint64_t)t->priority, t->quantum, t->age, t->wakeup_time, (uint64_t)t->exit_code,
                        t->heap_start, t->program_break, t->mmap_next);
        console::printf("          ustack=[%p..%p) limit=%p fd_capacity=%d\n", t->user_stack,
                        t->user_stack_top, t->user_stack_limit, t->fd_capacity);
    }

    void dump_all_tasks() {
        console::printf("--- TASK LIST ---\n");
        for (task *t = all_tasks_head; t; t = t->global_next) { dump_task(t); }
    }

    void yield() { scheduler_yield_context(); }

    void sleep(uint64_t ticks) {
        smp::cpu_local *cpu = smp::get_cpu();
        uint64_t flags;
        cpu->sched_lock.acquire(flags);

        task *current = cpu->current_task;
        current->state = task_state::SLEEPING;
        current->wakeup_time = cpu->ticks + ticks;
        current->next = cpu->sleep_list_head;
        cpu->sleep_list_head = current;

        cpu->sched_lock.release(flags);
        yield();
    }

    void exit(int code) {
        smp::cpu_local *cpu = smp::get_cpu();
        task *current = cpu->current_task;

        if (current && current->id == 1) {
            console::printf("[SCHED] init process exited with status %d; panicking.\n", (uint64_t)code);
            kpanic("init process exited");
        }

        uint64_t flags;
        cpu->sched_lock.acquire(flags);

        current->exit_code = code;
        current->state = task_state::ZOMBIE;
        reparent_children(current);

        if (current->parent) {
            if (current->parent->state == task_state::BLOCKED) {
                current->parent->state = task_state::READY;
                enqueue(cpu, current->parent);
            }
        }

        for (uint64_t i = 0; i < current->fd_capacity; i++) { fd_manager::close_fd(i, current); }

        cpu->sched_lock.release(flags);

        yield();
    }

    int wait(int *status) {
        smp::cpu_local *cpu = smp::get_cpu();
        task *current = cpu->current_task;

        while (true) {
            uint64_t flags;
            cpu->sched_lock.acquire(flags);

            task *prev_sibling = nullptr;
            task *child = current->first_child;

            if (!child) {
                cpu->sched_lock.release(flags);
                return -1;  // No children to wait for
            }

            while (child) {
                if (child->state == task_state::ZOMBIE) {
                    int child_id = child->id;

                    if (status) { *status = child->exit_code; }

                    if (prev_sibling) {
                        prev_sibling->next_sibling = child->next_sibling;
                    } else {
                        current->first_child = child->next_sibling;
                    }

                    unlink_global(child);

                    cpu->sched_lock.release(flags);
                    free_task_resources(child);
                    return child_id;
                }
                prev_sibling = child;
                child = child->next_sibling;
            }

            // No zombies found, but children exist: Block the parent
            current->state = task_state::BLOCKED;
            cpu->sched_lock.release(flags);

            yield();
        }
    }

}  // namespace scheduler