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

namespace scheduler {

    static uint64_t next_tid = 1;

    static inline void fxsave_task(task *t) {
        asm volatile("fxsave (%0)" : : "r"(t->fxsave_area) : "memory");
    }
    static inline void fxrstor_task(task *t) {
        asm volatile("fxrstor (%0)" : : "r"(t->fxsave_area) : "memory");
    }

    static void enqueue(smp::cpu_local *cpu, task *t);
    static task *dequeue(smp::cpu_local *cpu);
    static task *steal_work(smp::cpu_local *self);

    // --- Work Stealing ---

    static task *steal_work(smp::cpu_local *self) {
        return nullptr;  // TODO
        uint64_t core_count = smp::get_core_count();
        if (core_count < 2) return nullptr;

        uint64_t start_index = (self->cpu_index + 1) % core_count;

        for (uint64_t i = 0; i < core_count - 1; i++) {
            uint64_t victim_idx = (start_index + i) % core_count;
            smp::cpu_local *victim = smp::get_cpu_by_index(victim_idx);

            if (!victim || victim == self) continue;

            uint64_t victim_flags;
            if (!victim->sched_lock.try_acquire(victim_flags)) continue;

            for (int p = 0; p < MAX_QUEUES; p++) {
                if (victim->task_queues_tail[p]) {
                    task *t = victim->task_queues_tail[p];

                    victim->task_queues_tail[p] = t->prev;
                    if (victim->task_queues_tail[p]) {
                        victim->task_queues_tail[p]->next = nullptr;
                    } else {
                        victim->task_queues_head[p] = nullptr;
                    }

                    victim->sched_lock.release(victim_flags);

                    t->next = nullptr;
                    t->prev = nullptr;
                    t->priority = p;  // Keep priority
                    t->quantum = TIME_QUANTUMS[p];
                    return t;
                }
            }
            victim->sched_lock.release(victim_flags);
        }
        return nullptr;
    }

    static void enqueue(smp::cpu_local *cpu, task *t) {
        if (t->state != task_state::READY) {
            console::printf("[SCHED] Tried to enqueue not-ready task %d", t->id);
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

        task *stolen = steal_work(cpu);
        return stolen ? stolen : cpu->idle_task;
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

        cpu->idle_task = idle;
        cpu->current_task = idle;
        cpu->kernel_stack = idle->kernel_stack;
    }

    task *spawn(task_type type, void (*entry_point)(), uint64_t pml4) {
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

                t->fd_table[0] = nullptr;  // stdin
                t->fd_table[1] = file;     // stdout
                t->fd_table[2] = file;     // stderr
            }
        }

        if (type == task_type::USER) {
            uint64_t stack_pages = INITIAL_USER_STACK_SIZE / pmm::PAGE_SIZE;
            uint64_t stack_phys = pmm::alloc(stack_pages * pmm::PAGE_SIZE);
            uint64_t stack_virt = 0x00007FFFFFFFF000 - INITIAL_USER_STACK_SIZE;

            vmm::map_range(stack_virt, stack_phys, INITIAL_USER_STACK_SIZE,
                           vmm::PageFlags::Present | vmm::PageFlags::Write | vmm::PageFlags::User,
                           pml4);

            t->user_stack = (void *)stack_virt;
            r->rip = (uint64_t)entry_point;
            r->rsp = (uint64_t)stack_virt + INITIAL_USER_STACK_SIZE;
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

        // 3. Handle File Descriptors
        if (flags & 0x2) {  // CLONE_FILES
            // Task already points to parent's table via memcpy, just need to manage it
            // usually you'd implement a ref-count for the whole table if sharing
        } else {
            child->fd_table =
                (vfs::open_file **)heap::kmalloc(sizeof(vfs::open_file *) * parent->fd_capacity);
            child->fd_capacity = parent->fd_capacity;
            for (uint32_t i = 0; i < parent->fd_capacity; i++) {
                child->fd_table[i] = parent->fd_table[i];
                if (child->fd_table[i]) child->fd_table[i]->ref_count++;
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
        enqueue(cpu, child);
        cpu->sched_lock.release(f);

        return child->id;
    }

    int exec(const char *path, char **argv) {  // TODO: safer
        smp::cpu_local *cpu = smp::get_cpu();
        task *current = cpu->current_task;

        int argc = 0;
        char **temp_argv = nullptr;

        if (argv != nullptr) {
            while (argv[argc] != nullptr) argc++;

            temp_argv = (char **)heap::kmalloc(sizeof(char *) * (argc + 1));
            for (int i = 0; i < argc; i++) {
                size_t len = strlen(argv[i]) + 1;
                temp_argv[i] = (char *)heap::kmalloc(len);
                memcpy(temp_argv[i], argv[i], len);
            }
            temp_argv[argc] = nullptr;
        }

        elf::elf_info info = elf::load(path);
        if (info.entry == 0) {
            // free temp_argv
            return -1;
        }

        uint64_t old_cr3 = current->cr3;
        current->cr3 = info.pml4;

        asm volatile("mov %0, %%cr3" : : "r"(current->cr3) : "memory");

        uint64_t stack_pages = INITIAL_USER_STACK_SIZE / pmm::PAGE_SIZE;
        uint64_t stack_phys = pmm::alloc(stack_pages * pmm::PAGE_SIZE);
        uint64_t stack_virt = 0x00007FFFFFFFF000 - INITIAL_USER_STACK_SIZE;

        vmm::map_range(stack_virt, stack_phys, INITIAL_USER_STACK_SIZE,
                       vmm::PageFlags::Present | vmm::PageFlags::Write | vmm::PageFlags::User,
                       current->cr3);

        uint64_t *stack_ptr = (uint64_t *)((uintptr_t)stack_virt + INITIAL_USER_STACK_SIZE);
        uint64_t user_argv_ptrs[argc + 1];

        for (int i = argc - 1; i >= 0; i--) {
            size_t len = strlen(temp_argv[i]) + 1;
            stack_ptr = (uint64_t *)((uintptr_t)stack_ptr - len);
            memcpy(stack_ptr, temp_argv[i], len);
            user_argv_ptrs[i] = (uint64_t)stack_ptr;
            heap::kfree(temp_argv[i]);
        }

        user_argv_ptrs[argc] = 0;
        if (temp_argv) heap::kfree(temp_argv);

        stack_ptr = (uint64_t *)((uintptr_t)stack_ptr & ~0xF);
        for (int i = argc; i >= 0; i--) {
            stack_ptr--;
            *stack_ptr = user_argv_ptrs[i];
        }
        uint64_t argv_ptr_for_rdi = (uint64_t)stack_ptr;

        current->user_stack = (void *)stack_virt;

        regs *r = current->context;
        memset(r, 0, sizeof(regs));

        r->rip = info.entry;
        r->rsp = (uintptr_t)stack_ptr;
        r->rdi = (uint64_t)argc;
        r->rsi = argv_ptr_for_rdi;
        r->rflags = 0x202;                    // IF = 1
        r->cs = gdt::selectors::UCODE64_SEL;  // User Code Selector
        r->ss = gdt::selectors::UDATA64_SEL;  // User Data Selector

        asm volatile("fninit; fxsave (%0)" : : "r"(current->fxsave_area) : "memory");

        if (old_cr3 != vmm::get_kernel_pagemap()) { vmm::destroy_user_address_space(old_cr3); }

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

    void yield() { asm volatile("int $0x81"); }

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

        uint64_t flags;
        cpu->sched_lock.acquire(flags);

        current->exit_code = code;
        current->state = task_state::ZOMBIE;

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

                    if (child->kernel_stack) heap::kfree(child->kernel_stack);
                    if (child->fd_table) heap::kfree(child->fd_table);
                    heap::kfree(child);

                    cpu->sched_lock.release(flags);
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