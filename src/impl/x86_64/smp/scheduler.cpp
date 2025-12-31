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

    // --- Wait Queue Implementation ---

    void wait_queue::wait(uint64_t &lock_flags) {
        smp::cpu_local *cpu = smp::get_cpu();
        task *current = cpu->current_task;

        current->state = task_state::WAITING;

        // Add to wait queue linked list (reusing the 'next' pointer for the queue)
        current->global_next = head;
        head = current;

        // Release the scheduler lock before yielding
        cpu->sched_lock.release(lock_flags);
        yield();

        // Re-acquire lock after waking up to maintain caller's expectations
        cpu->sched_lock.acquire(lock_flags);
    }

    void wait_queue::wake_one() {
        if (!head) return;

        task *t = head;
        head = head->global_next;

        t->state = task_state::READY;
        // In a real SMP system, you'd need to find the CPU this task belongs to
        // or a global balancer. Here we assume it returns to the current CPU.
        enqueue(smp::get_cpu(), t);
    }

    void wait_queue::wake_all() {
        while (head) wake_one();
    }

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
            r->cs = 0x23;  // User Code
            r->ss = 0x1B;  // User Data
            t->cr3 = pml4;
        } else {
            r->rip = (uint64_t)entry_point;
            r->rsp = (uint64_t)kstack + KERNEL_STACK_SIZE;
            r->cs = 0x08;  // Kernel Code
            r->ss = 0x10;  // Kernel Data
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

        // Reset process-specific queues and links
        child->death_queue.head = nullptr;
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

    int exec(const char *path) {  // TODO: safer
        smp::cpu_local *cpu = smp::get_cpu();
        task *current = cpu->current_task;

        // 1. Load the new ELF file
        // elf::load creates a new PML4 and maps the program segments
        elf::elf_info info = elf::load(path);
        if (info.entry == 0) {
            return -1;  // Failed to load or find file
        }

        // 2. Prepare the new address space
        // We save the old CR3 so we can destroy it after switching
        uint64_t old_cr3 = current->cr3;
        current->cr3 = info.pml4;

        // Switch to the new address space immediately
        asm volatile("mov %0, %%cr3" : : "r"(current->cr3) : "memory");

        // 3. Reset the User Stack
        // We need to map a fresh stack in the NEW address space
        uint64_t stack_pages = INITIAL_USER_STACK_SIZE / pmm::PAGE_SIZE;
        uint64_t stack_phys = pmm::alloc(stack_pages * pmm::PAGE_SIZE);
        uint64_t stack_virt = 0x00007FFFFFFFF000 - INITIAL_USER_STACK_SIZE;

        vmm::map_range(stack_virt, stack_phys, INITIAL_USER_STACK_SIZE,
                       vmm::PageFlags::Present | vmm::PageFlags::Write | vmm::PageFlags::User,
                       current->cr3);

        current->user_stack = (void *)stack_virt;

        // 4. Reset the CPU context
        // We reuse the existing kernel stack but reset the register state
        regs *r = current->context;
        memset(r, 0, sizeof(regs));

        r->rip = info.entry;
        r->rsp = (uint64_t)stack_virt + INITIAL_USER_STACK_SIZE;
        r->rflags = 0x202;  // IF = 1
        r->cs = 0x23;       // User Code Selector
        r->ss = 0x1B;       // User Data Selector

        // 5. Reset FPU/SSE state for the new program
        asm volatile("fninit; fxsave (%0)" : : "r"(current->fxsave_area) : "memory");

        // 6. Cleanup the old address space
        // Since we aren't using CoW in this specific path yet,
        // we destroy the previous PML4 and its associated private memory.
        if (old_cr3 != vmm::get_kernel_pagemap()) { vmm::destroy_user_address_space(old_cr3); }

        // Note: We do NOT call yield().
        // When the syscall handler returns, it will use the modified `regs`
        // in current->context to "return" into the entry point of the new ELF.
        return 0;
    }

    regs *schedule(regs *current_state, bool is_timer_tick) {
        smp::cpu_local *cpu = smp::get_cpu();
        if (!cpu) return current_state;

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
                    return current_state;
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

        return next->context;
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

        console::printf("Process %d exiting with code %d\n", current->id, code);
        uint64_t flags;
        cpu->sched_lock.acquire(flags);

        current->exit_code = code;
        current->state = task_state::ZOMBIE;

        // Close all file descriptors
        for (uint64_t i = 0; i < current->fd_capacity; i++) { fd_manager::close_fd(i, current); }

        // Notify parent
        if (current->parent) { current->parent->death_queue.wake_all(); }

        cpu->sched_lock.release(flags);
        yield();
        while (true) asm("hlt");  // Should never reach here
    }

    int wait(int *status) {
        smp::cpu_local *cpu = smp::get_cpu();
        uint64_t flags;

        while (true) {
            cpu->sched_lock.acquire(flags);
            task *current = cpu->current_task;
            task *prev_child = nullptr;
            task *child = current->first_child;

            while (child) {
                if (child->state == task_state::ZOMBIE) {
                    int id = child->id;
                    if (status) *status = child->exit_code;

                    // Unlink child
                    if (prev_child)
                        prev_child->next_sibling = child->next_sibling;
                    else
                        current->first_child = child->next_sibling;

                    // Cleanup memory
                    heap::kfree(child->kernel_stack);
                    heap::kfree(child->fd_table);
                    heap::kfree(child);

                    cpu->sched_lock.release(flags);
                    return id;
                }
                prev_child = child;
                child = child->next_sibling;
            }

            // No zombie children found, wait on queue
            current->death_queue.wait(flags);  // This releases flags internally
        }
    }

}  // namespace scheduler