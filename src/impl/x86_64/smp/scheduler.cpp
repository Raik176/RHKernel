#include "smp/scheduler.h"

#include "console.h"
#include "file/elf.h"
#include "file/fd.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "security/random.h"
#include "smp/smp.h"
#include "string.h"

extern "C" void scheduler_yield_context();

extern "C" void scheduler_schedule_from_context(regs *current_state) {
    scheduler::schedule(current_state, false);
}

namespace scheduler {
    static uint64_t next_tid = 1;
    static task *all_tasks_head = nullptr;
    static bool use_xsave = false;
    static uint64_t xsave_mask = 0x3;
    static uint32_t fpu_state_size = 512;

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

    static const char *vma_type_name(vma_type type) {
        switch (type) {
            case vma_type::IMAGE: return "IMAGE";
            case vma_type::HEAP: return "HEAP";
            case vma_type::STACK: return "STACK";
            case vma_type::ANON: return "ANON";
        }
        return "UNKNOWN";
    }

    static bool is_page_range(uint64_t start, uint64_t end) {
        return start != 0 && end > start && (start % pmm::PAGE_SIZE) == 0 &&
               (end % pmm::PAGE_SIZE) == 0 && end < 0x0000800000000000ULL;
    }

    static bool vma_overlaps(const vm_area *vma, uint64_t start, uint64_t end) {
        return start < vma->end && end > vma->start;
    }

    vm_area *find_vma(task *t, uint64_t addr) {
        if (!t) return nullptr;
        for (vm_area *vma = t->vma_list; vma; vma = vma->next) {
            if (addr >= vma->start && addr < vma->end) return vma;
        }
        return nullptr;
    }

    vm_area *find_vma_exact(task *t, uint64_t start, uint64_t end) {
        if (!t) return nullptr;
        for (vm_area *vma = t->vma_list; vma; vma = vma->next) {
            if (vma->start == start && vma->end == end) return vma;
        }
        return nullptr;
    }

    bool vma_range_free(task *t, uint64_t start, uint64_t end) {
        if (!t || !is_page_range(start, end)) return false;
        for (vm_area *vma = t->vma_list; vma; vma = vma->next) {
            if (vma_overlaps(vma, start, end)) return false;
        }
        return true;
    }

    vm_area *add_vma(task *t, uint64_t start, uint64_t end, uint64_t committed_start,
                     uint32_t flags, vma_type type) {
        if (!t || !is_page_range(start, end)) return nullptr;
        if (committed_start < start || committed_start > end ||
            (committed_start % pmm::PAGE_SIZE) != 0) {
            return nullptr;
        }
        if (!vma_range_free(t, start, end)) return nullptr;

        vm_area *area = (vm_area *)heap::kmalloc(sizeof(vm_area));
        if (!area) return nullptr;
        area->start = start;
        area->end = end;
        area->committed_start = committed_start;
        area->flags = flags;
        area->type = type;
        area->next = nullptr;

        vm_area **link = &t->vma_list;
        while (*link && (*link)->start < start) link = &(*link)->next;
        area->next = *link;
        *link = area;
        return area;
    }

    bool remove_vma(task *t, uint64_t start, uint64_t end) {
        if (!t || !is_page_range(start, end)) return false;
        vm_area **link = &t->vma_list;
        while (*link) {
            vm_area *area = *link;
            if (area->start == start && area->end == end) {
                *link = area->next;
                if (t->heap_vma == area) t->heap_vma = nullptr;
                if (t->stack_vma == area) t->stack_vma = nullptr;
                heap::kfree(area);
                return true;
            }
            link = &area->next;
        }
        return false;
    }

    void clear_vmas(task *t) {
        if (!t) return;
        vm_area *area = t->vma_list;
        while (area) {
            vm_area *next = area->next;
            heap::kfree(area);
            area = next;
        }
        t->vma_list = nullptr;
        t->heap_vma = nullptr;
        t->stack_vma = nullptr;
        t->program_break = 0;
        t->mmap_hint = 0;
    }

    bool clone_vmas(task *dst, const task *src) {
        if (!dst || !src) return false;
        dst->vma_list = nullptr;
        dst->heap_vma = nullptr;
        dst->stack_vma = nullptr;
        for (const vm_area *area = src->vma_list; area; area = area->next) {
            vm_area *copy = add_vma(dst, area->start, area->end, area->committed_start,
                                    area->flags, area->type);
            if (!copy) {
                clear_vmas(dst);
                return false;
            }
            if (src->heap_vma == area) dst->heap_vma = copy;
            if (src->stack_vma == area) dst->stack_vma = copy;
        }
        dst->program_break = src->program_break;
        dst->mmap_hint = src->mmap_hint;
        return true;
    }

    static uint64_t random_page_offset(uint64_t page_count) {
        if (page_count == 0) return 0;
        return (random::next_u64() % page_count) * pmm::PAGE_SIZE;
    }

    static uint64_t random_user_stack_top() {
        uint64_t max_pages = USER_STACK_ASLR_WINDOW / pmm::PAGE_SIZE;
        uint64_t guard_pages = MAX_USER_STACK_SIZE / pmm::PAGE_SIZE;
        uint64_t slide = random_page_offset(max_pages - guard_pages);
        return USER_STACK_TOP - slide;
    }

    static uint64_t random_mmap_base() {
        uint64_t pages = USER_MMAP_ASLR_WINDOW / pmm::PAGE_SIZE;
        return USER_MMAP_BASE_MIN + random_page_offset(pages);
    }

    static uint32_t elf_segment_vma_flags(uint32_t flags) {
        uint32_t out = 0;
        if (flags & PF_R) out |= VMA_READ;
        if (flags & PF_W) out |= VMA_WRITE;
        if (flags & PF_X) out |= VMA_EXEC;
        return out;
    }

    static bool add_elf_image_vmas(task *t, const elf::elf_info &info) {
        if (info.segment_count == 0 || info.segment_count > elf::MAX_LOAD_SEGMENTS) return false;
        for (uint32_t i = 0; i < info.segment_count; i++) {
            const elf::elf_load_segment &seg = info.segments[i];
            if (!add_vma(t, seg.start, seg.end, seg.start,
                         elf_segment_vma_flags(seg.flags), vma_type::IMAGE)) {
                return false;
            }
        }
        return true;
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
        clear_vmas(t);

        if (t->kernel_stack) heap::kfree(t->kernel_stack);
        if (t->fpu_storage) heap::kfree(t->fpu_storage);
        if (t->fd_table) heap::kfree(t->fd_table);
        if (t->cwd_path) heap::kfree(t->cwd_path);
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

    static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
                             uint32_t *ecx, uint32_t *edx) {
        asm volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
    }

    static inline uint64_t xgetbv(uint32_t index) {
        uint32_t eax = 0, edx = 0;
        asm volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
        return ((uint64_t)edx << 32) | eax;
    }

    static void detect_fpu_state_size() {
        fpu_state_size = 512;
        if (!use_xsave) return;

        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
        cpuid(0xD, 0, &eax, &ebx, &ecx, &edx);
        if ((eax & xsave_mask) != xsave_mask || ebx < 512) {
            kpanic("Invalid CPUID xstate size");
        }
        fpu_state_size = (ebx + 63U) & ~63U;
    }

    static void alloc_fpu_state(task *t) {
        t->fpu_storage = heap::kmalloc(fpu_state_size + 63);
        if (!t->fpu_storage) kpanic("FPU state allocation failed");
        uintptr_t aligned = ((uintptr_t)t->fpu_storage + 63U) & ~63ULL;
        t->fpu_area = (void *)aligned;
    }

    static inline void save_fpu(task *t) {
        if (!t || !t->fpu_area) return;
        if (use_xsave) {
            uint32_t eax = (uint32_t)xsave_mask;
            uint32_t edx = (uint32_t)(xsave_mask >> 32);
            asm volatile("xsave (%0)" : : "r"(t->fpu_area), "a"(eax), "d"(edx) : "memory");
        } else {
            asm volatile("fxsave (%0)" : : "r"(t->fpu_area) : "memory");
        }
    }

    static inline void restore_fpu(task *t) {
        if (!t || !t->fpu_area) return;
        if (use_xsave) {
            uint32_t eax = (uint32_t)xsave_mask;
            uint32_t edx = (uint32_t)(xsave_mask >> 32);
            asm volatile("xrstor (%0)" : : "r"(t->fpu_area), "a"(eax), "d"(edx) : "memory");
        } else {
            asm volatile("fxrstor (%0)" : : "r"(t->fpu_area) : "memory");
        }
    }

    static void init_fpu_state(task *t) {
        if (!t->fpu_area) alloc_fpu_state(t);
        memset(t->fpu_area, 0, fpu_state_size);
        *(uint16_t *)((uint8_t *)t->fpu_area + 0) = 0x037F;
        *(uint32_t *)((uint8_t *)t->fpu_area + 24) = 0x1F80;
    }

    static void refresh_queue_oldest(smp::cpu_local *cpu, int p) {
        cpu->task_queue_oldest_tick[p] = cpu->task_queues_head[p] ? cpu->task_queues_head[p]->enqueue_tick : 0;
    }

    static void unlink_queued(smp::cpu_local *cpu, task *t, int p) {
        if (t->prev)
            t->prev->next = t->next;
        else
            cpu->task_queues_head[p] = t->next;

        if (t->next)
            t->next->prev = t->prev;
        else
            cpu->task_queues_tail[p] = t->prev;

        t->next = nullptr;
        t->prev = nullptr;
        refresh_queue_oldest(cpu, p);
    }

    static void enqueue(smp::cpu_local *cpu, task *t) {
        if (t->state != task_state::READY) {
            console::printf("[SCHED] Tried to enqueue not-ready task %d", (uint64_t)t->id);
        }

        int p = t->priority;
        t->enqueue_tick = cpu->ticks;
        t->last_cpu_index = cpu->cpu_index;
        t->next = nullptr;
        t->prev = cpu->task_queues_tail[p];

        if (!cpu->task_queues_head[p]) {
            cpu->task_queues_head[p] = t;
            cpu->task_queues_tail[p] = t;
            cpu->task_queue_oldest_tick[p] = t->enqueue_tick;
        } else {
            cpu->task_queues_tail[p]->next = t;
            cpu->task_queues_tail[p] = t;
        }
    }

    static void promote_expired_tasks(smp::cpu_local *cpu) {
        for (int i = 1; i < MAX_QUEUES; i++) {
            while (cpu->task_queues_head[i] &&
                   cpu->ticks - cpu->task_queue_oldest_tick[i] > AGING_THRESHOLD) {
                task *item = cpu->task_queues_head[i];
                unlink_queued(cpu, item, i);
                item->priority = i - 1;
                item->quantum = TIME_QUANTUMS[item->priority];
                enqueue(cpu, item);
            }
        }
    }

    static task *dequeue(smp::cpu_local *cpu) {
        promote_expired_tasks(cpu);

        for (int i = 0; i < MAX_QUEUES; i++) {
            if (cpu->task_queues_head[i]) {
                task *t = cpu->task_queues_head[i];
                unlink_queued(cpu, t, i);
                return t;
            }
        }

        return cpu->idle_task;
    }

    void init_core() {
        smp::cpu_local *cpu = smp::get_cpu();
        if (cpu->cpu_features.xsave) {
            uint64_t xcr0 = xgetbv(0);
            use_xsave = true;
            xsave_mask = cpu->cpu_features.avx ? (xcr0 & 0x7) : (xcr0 & 0x3);
        }
        detect_fpu_state_size();

        task *idle = (task *)heap::kmalloc(sizeof(task));
        memset(idle, 0, sizeof(task));

        idle->id = 0;
        idle->priority = MAX_QUEUES - 1;
        idle->last_cpu_index = cpu->cpu_index;
        idle->cr3 = vmm::get_kernel_pagemap();
        idle->state = task_state::RUNNING;
        idle->type = task_type::KERNEL;
        idle->kernel_stack = heap::kmalloc(KERNEL_STACK_SIZE);
        idle->cwd = vfs::get_root();
        idle->cwd_path = strdup("/");
        init_fpu_state(idle);

        link_global(idle);

        cpu->idle_task = idle;
        cpu->current_task = idle;
        cpu->kernel_stack = (void *)((uintptr_t)idle->kernel_stack + KERNEL_STACK_SIZE);
        cpu->tss_entry.rsp0 = (uint64_t)cpu->kernel_stack;
    }

    task *spawn(task_type type, void (*entry_point)(), uint64_t pml4, uint64_t heap_start,
                const elf::elf_info *image) {
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

        t->program_break = heap_start;
        t->mmap_hint = type == task_type::USER ? random_mmap_base() : 0;

        if (type == task_type::USER) {
            if (image && !add_elf_image_vmas(t, *image)) {
                kpanic("Failed to create image VMAs");
            }
            if (heap_start &&
                !(t->heap_vma = add_vma(t, heap_start, heap_start + pmm::PAGE_SIZE, heap_start,
                                        VMA_READ | VMA_WRITE, vma_type::HEAP))) {
                kpanic("Failed to create heap VMA");
            }
        }

        t->parent = cpu->current_task;
        if (t->parent) {
            t->next_sibling = t->parent->first_child;
            t->parent->first_child = t;
        }

        t->fd_capacity = INITIAL_FD_CAPACITY;
        t->fd_table = (vfs::open_file **)heap::kmalloc(sizeof(vfs::open_file *) * t->fd_capacity);
        memset(t->fd_table, 0, sizeof(vfs::open_file *) * t->fd_capacity);
        t->cwd = t->parent && t->parent->cwd ? t->parent->cwd : vfs::get_root();
        t->cwd_path = strdup(t->parent && t->parent->cwd_path ? t->parent->cwd_path : "/");
        if (!t->cwd_path) kpanic("cwd allocation failed");

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
            if (!stack_phys) kpanic("User stack allocation failed");

            uint64_t stack_top = random_user_stack_top();
            uint64_t stack_virt = stack_top - INITIAL_USER_STACK_SIZE;

            vmm::map_range(stack_virt, stack_phys, INITIAL_USER_STACK_SIZE,
                           vmm::PageFlags::Write | vmm::PageFlags::User | vmm::PageFlags::NX, pml4);
            memset(p2v(stack_phys), 0, INITIAL_USER_STACK_SIZE);

            uint64_t stack_ptr = stack_top;
            uint8_t random_bytes[16];
            random::fill(random_bytes, sizeof(random_bytes));
            stack_ptr -= sizeof(random_bytes);
            memcpy((void *)((uintptr_t)p2v(stack_phys) + (stack_ptr - stack_virt)), random_bytes,
                   sizeof(random_bytes));
            uint64_t at_random_ptr = stack_ptr;

            auto push_u64 = [&](uint64_t value) {
                stack_ptr -= sizeof(uint64_t);
                *(uint64_t *)((uintptr_t)p2v(stack_phys) + (stack_ptr - stack_virt)) = value;
            };

            stack_ptr &= ~0xFULL;
            push_u64(0);
            push_u64(0);
            push_u64(at_random_ptr);
            push_u64(25);
            push_u64(0);
            uint64_t auxv_ptr = stack_ptr;
            push_u64(0);

            t->stack_vma = add_vma(t, stack_top - MAX_USER_STACK_SIZE, stack_top, stack_virt,
                                   VMA_READ | VMA_WRITE | VMA_GROWSDOWN, vma_type::STACK);
            if (!t->stack_vma) kpanic("Failed to create stack VMA");
            r->rip = (uint64_t)entry_point;
            r->rsp = stack_ptr;
            r->rdi = 0;
            r->rsi = stack_ptr;
            r->rdx = auxv_ptr;
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

        init_fpu_state(t);

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

        child->vma_list = nullptr;
        child->heap_vma = nullptr;
        child->stack_vma = nullptr;
        child->fpu_area = nullptr;
        child->fpu_storage = nullptr;
        child->cwd = parent->cwd ? parent->cwd : vfs::get_root();
        child->cwd_path = strdup(parent->cwd_path ? parent->cwd_path : "/");
        if (!child->cwd_path) {
            heap::kfree(child);
            return -1;
        }
        if (!clone_vmas(child, parent)) {
            heap::kfree(child);
            return -1;
        }

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

        // 7. Copy FPU state
        alloc_fpu_state(child);
        memcpy(child->fpu_area, parent->fpu_area, fpu_state_size);

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

    static void *user_stack_to_kernel(uint64_t stack_phys, uint64_t stack_base, uint64_t stack_top,
                                      uint64_t addr, uint64_t len) {
        if (addr < stack_base || addr > stack_top || len > stack_top - addr) return nullptr;
        uint64_t off = addr - stack_base;
        if (off >= INITIAL_USER_STACK_SIZE || len > INITIAL_USER_STACK_SIZE - off) return nullptr;
        return (void *)((uintptr_t)p2v(stack_phys) + off);
    }

    int exec(const char *path, char **argv, regs *return_frame) {
        smp::cpu_local *cpu = smp::get_cpu();
        task *current = cpu ? cpu->current_task : nullptr;
        if (!current || !path || !return_frame) {
            return -1;
        }

        exec_arg *args = nullptr;
        int argc = 0;
        if (!copy_exec_args(argv, &args, &argc)) {
            return -1;
        }

        elf::elf_info info = elf::load(path);
        if (info.entry == 0 || info.pml4 == 0) {
            free_exec_args(args, argc);
            return -1;
        }

        uint64_t stack_pages = INITIAL_USER_STACK_SIZE / pmm::PAGE_SIZE;
        uint64_t stack_phys = pmm::alloc(stack_pages * pmm::PAGE_SIZE);
        if (!stack_phys) {
            free_exec_args(args, argc);
            vmm::destroy_user_address_space(info.pml4);
            return -1;
        }
        memset(p2v(stack_phys), 0, INITIAL_USER_STACK_SIZE);

        uint64_t stack_top = random_user_stack_top();
        uint64_t stack_virt = stack_top - INITIAL_USER_STACK_SIZE;
        vmm::map_range(stack_virt, stack_phys, INITIAL_USER_STACK_SIZE,
                       vmm::PageFlags::Write | vmm::PageFlags::User | vmm::PageFlags::NX, info.pml4);

        constexpr int MAX_EXEC_ARGC = 64;
        uint64_t stack_ptr = stack_top;
        uint64_t user_argv_ptrs[MAX_EXEC_ARGC + 1];
        memset(user_argv_ptrs, 0, sizeof(user_argv_ptrs));

        for (int i = argc - 1; i >= 0; i--) {
            size_t len = args[i].len;
            if (len == 0 || len > stack_ptr - stack_virt) {
                free_exec_args(args, argc);
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }

            stack_ptr -= len;
            void *dst = user_stack_to_kernel(stack_phys, stack_virt, stack_top, stack_ptr, len);
            if (!dst) {
                free_exec_args(args, argc);
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }
            memcpy(dst, args[i].data, len);
            user_argv_ptrs[i] = stack_ptr;
        }
        user_argv_ptrs[argc] = 0;
        free_exec_args(args, argc);

        uint8_t random_bytes[16];
        random::fill(random_bytes, sizeof(random_bytes));
        stack_ptr -= sizeof(random_bytes);
        void *random_slot = user_stack_to_kernel(stack_phys, stack_virt, stack_top, stack_ptr,
                                                 sizeof(random_bytes));
        if (!random_slot) {
            vmm::destroy_user_address_space(info.pml4);
            return -1;
        }
        memcpy(random_slot, random_bytes, sizeof(random_bytes));
        uint64_t at_random_ptr = stack_ptr;

        auto push_u64 = [&](uint64_t value) -> bool {
            stack_ptr -= sizeof(uint64_t);
            void *slot = user_stack_to_kernel(stack_phys, stack_virt, stack_top, stack_ptr,
                                              sizeof(uint64_t));
            if (!slot) return false;
            *(uint64_t *)slot = value;
            return true;
        };

        stack_ptr &= ~0xFULL;
        if (!push_u64(0) || !push_u64(0) || !push_u64(at_random_ptr) || !push_u64(25) ||
            !push_u64(0)) {
            vmm::destroy_user_address_space(info.pml4);
            return -1;
        }
        uint64_t auxv_ptr_for_rdx = stack_ptr;

        for (int i = argc; i >= 0; i--) {
            if (!push_u64(user_argv_ptrs[i])) {
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }
        }
        uint64_t argv_ptr_for_rdi = stack_ptr;

        task new_vm;
        memset(&new_vm, 0, sizeof(new_vm));
        new_vm.program_break = info.heap_start;
        new_vm.mmap_hint = random_mmap_base();

        if (info.load_base == 0 || info.load_end <= info.load_base ||
            !add_elf_image_vmas(&new_vm, info) ||
            !(new_vm.heap_vma = add_vma(&new_vm, info.heap_start, info.heap_start + pmm::PAGE_SIZE,
                                        info.heap_start, VMA_READ | VMA_WRITE, vma_type::HEAP)) ||
            !(new_vm.stack_vma = add_vma(&new_vm, stack_top - MAX_USER_STACK_SIZE, stack_top,
                                         stack_virt, VMA_READ | VMA_WRITE | VMA_GROWSDOWN,
                                         vma_type::STACK))) {
            clear_vmas(&new_vm);
            vmm::destroy_user_address_space(info.pml4);
            return -1;
        }

        uint64_t old_cr3 = current->cr3;
        clear_vmas(current);
        current->cr3 = info.pml4;
        current->vma_list = new_vm.vma_list;
        current->heap_vma = new_vm.heap_vma;
        current->stack_vma = new_vm.stack_vma;
        current->program_break = new_vm.program_break;
        current->mmap_hint = new_vm.mmap_hint;

        regs *r = return_frame;
        current->context = r;
        memset(r, 0, sizeof(regs));

        r->rip = info.entry;
        r->rsp = stack_ptr;
        r->rdi = (uint64_t)argc;
        r->rsi = argv_ptr_for_rdi;
        r->rdx = auxv_ptr_for_rdx;
        r->rax = 0;
        r->rflags = 0x202;
        r->cs = gdt::selectors::UCODE64_SEL;
        r->ss = gdt::selectors::UDATA64_SEL;

        init_fpu_state(current);
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
            save_fpu(current);
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

        restore_fpu(next);

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

    bool task_id_by_index(uint64_t index, uint64_t *id_out) {
        if (!id_out) return false;
        for (task *t = all_tasks_head; t; t = t->global_next) {
            if (t->id == 0) continue;
            if (index == 0) {
                *id_out = t->id;
                return true;
            }
            index--;
        }
        return false;
    }

    static void append(char *buf, uint64_t cap, uint64_t *pos, const char *s) {
        while (*s && *pos + 1 < cap) buf[(*pos)++] = *s++;
        if (*pos < cap) buf[*pos] = 0;
    }

    static void appendf(char *buf, uint64_t cap, uint64_t *pos, const char *fmt, ...) {
        if (!buf || !pos || *pos >= cap) return;

        char tmp[384];
        va_list args;
        va_start(args, fmt);
        int len = vsnprintf(tmp, sizeof(tmp), fmt, args);
        va_end(args);

        if (len <= 0) return;
        append(buf, cap, pos, tmp);
    }

    static uint64_t task_age_ticks(task *t) {
        if (!t || t->state != task_state::READY) return 0;
        smp::cpu_local *cpu = smp::get_cpu_by_index(t->last_cpu_index);
        if (!cpu || cpu->ticks < t->enqueue_tick) return 0;
        return cpu->ticks - t->enqueue_tick;
    }

    uint64_t format_task_list(char *buf, uint64_t cap, bool detail, uint64_t only_pid) {
        if (!buf || cap == 0) return 0;
        uint64_t pos = 0;
        buf[0] = 0;

        if (!detail) {
            append(buf, cap, &pos, "pid ppid cpu state priority quantum age type\n");
        }

        for (task *t = all_tasks_head; t; t = t->global_next) {
            if (t->id == 0) continue;
            if (only_pid != UINT64_MAX && t->id != only_pid) continue;

            uint64_t ppid = t->parent ? t->parent->id : 0;
            uint64_t age = task_age_ticks(t);
            if (!detail) {
                appendf(buf, cap, &pos, "%d %d %d %s %d %d %d %s\n", t->id, ppid,
                        t->last_cpu_index, state_name(t->state), (uint64_t)t->priority,
                        t->quantum, age, type_name(t->type));
                continue;
            }

            appendf(buf, cap, &pos,
                    "pid: %d\nppid: %d\ncpu: %d\nstate: %s\ntype: %s\npriority: %d\nquantum: %d\nenqueue_tick: %d\nage_ticks: %d\nwakeup_tick: %d\nexit_code: %d\ncr3: %p\nkernel_stack: %p\ncontext: %p\nprogram_break: %p\nmmap_hint: %p\nfd_capacity: %d\n",
                    t->id, ppid, t->last_cpu_index, state_name(t->state), type_name(t->type),
                    (uint64_t)t->priority, t->quantum, t->enqueue_tick, age, t->wakeup_time,
                    (uint64_t)t->exit_code, t->cr3, t->kernel_stack, t->context,
                    t->program_break, t->mmap_hint, t->fd_capacity);
            for (vm_area *vma = t->vma_list; vma; vma = vma->next) {
                appendf(buf, cap, &pos, "vma: %s %p-%p committed=%p flags=%x\n",
                        vma_type_name(vma->type), vma->start, vma->end,
                        vma->committed_start, (uint64_t)vma->flags);
            }
            append(buf, cap, &pos, "\n");
        }

        if (only_pid != UINT64_MAX && pos == 0) {
            appendf(buf, cap, &pos, "pid %d not found\n", only_pid);
        }
        return pos;
    }

    uint64_t format_task_status(char *buf, uint64_t cap, uint64_t pid) {
        if (!buf || cap == 0) return 0;
        uint64_t pos = 0;
        buf[0] = 0;
        task *t = get_task_by_id(pid);
        if (!t) {
            appendf(buf, cap, &pos, "pid %d not found\n", pid);
            return pos;
        }
        uint64_t ppid = t->parent ? t->parent->id : 0;
        uint64_t age = task_age_ticks(t);
        appendf(buf, cap, &pos,
                "pid: %d\nppid: %d\ncpu: %d\nstate: %s\ntype: %s\npriority: %d\nquantum: %d\nenqueue_tick: %d\nage_ticks: %d\nwakeup_tick: %d\nexit_code: %d\ncr3: %p\nkernel_stack: %p\ncontext: %p\nprogram_break: %p\nmmap_hint: %p\nfd_capacity: %d\n",
                t->id, ppid, t->last_cpu_index, state_name(t->state), type_name(t->type),
                (uint64_t)t->priority, t->quantum, t->enqueue_tick, age, t->wakeup_time,
                (uint64_t)t->exit_code, t->cr3, t->kernel_stack, t->context,
                t->program_break, t->mmap_hint, t->fd_capacity);
        return pos;
    }

    uint64_t format_task_maps(char *buf, uint64_t cap, uint64_t pid) {
        if (!buf || cap == 0) return 0;
        uint64_t pos = 0;
        buf[0] = 0;
        task *t = get_task_by_id(pid);
        if (!t) {
            appendf(buf, cap, &pos, "pid %d not found\n", pid);
            return pos;
        }
        append(buf, cap, &pos, "start end committed flags type\n");
        for (vm_area *vma = t->vma_list; vma; vma = vma->next) {
            appendf(buf, cap, &pos, "%p %p %p %c%c%c%c %s\n",
                    vma->start, vma->end, vma->committed_start,
                    (vma->flags & VMA_READ) ? 'r' : '-',
                    (vma->flags & VMA_WRITE) ? 'w' : '-',
                    (vma->flags & VMA_EXEC) ? 'x' : '-',
                    (vma->flags & VMA_GROWSDOWN) ? 'g' : '-',
                    vma_type_name(vma->type));
        }
        return pos;
    }

    uint64_t format_task_fds(char *buf, uint64_t cap, uint64_t pid) {
        if (!buf || cap == 0) return 0;
        uint64_t pos = 0;
        buf[0] = 0;
        task *t = get_task_by_id(pid);
        if (!t) {
            appendf(buf, cap, &pos, "pid %d not found\n", pid);
            return pos;
        }
        append(buf, cap, &pos, "fd type offset refs name\n");
        for (uint64_t i = 0; i < t->fd_capacity; i++) {
            vfs::open_file *of = t->fd_table ? t->fd_table[i] : nullptr;
            if (!of || !of->node) continue;
            appendf(buf, cap, &pos, "%d %d %d %d %s\n", i, (uint64_t)of->node->type,
                    of->offset, (uint64_t)of->ref_count, of->node->name ? of->node->name : "");
        }
        return pos;
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
        console::printf("          prio=%d quantum=%d enqueue_tick=%d wakeup=%d exit=%d brk=%p mmap_hint=%p fd_capacity=%d\n",
                        (uint64_t)t->priority, t->quantum, t->enqueue_tick, t->wakeup_time,
                        (uint64_t)t->exit_code, t->program_break, t->mmap_hint, t->fd_capacity);
        for (vm_area *vma = t->vma_list; vma; vma = vma->next) {
            console::printf("          vma %s [%p..%p) committed=%p flags=%x\n",
                            vma_type_name(vma->type), vma->start, vma->end,
                            vma->committed_start, (uint64_t)vma->flags);
        }
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