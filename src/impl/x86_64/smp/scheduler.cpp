#include "smp/scheduler.h"

#include "console.h"
#include "file/elf.h"
#include "file/fd.h"
#include "file/device.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "security/random.h"
#include "smp/lock.h"
#include "smp/smp.h"
#include "string.h"

extern "C" void scheduler_yield_context();
extern "C" void syscall_set_kernel_stack(uint64_t stack_top);

extern "C" __attribute__((noreturn, no_stack_protector)) void scheduler_schedule_from_context(regs *current_state) {
    scheduler::schedule(current_state, false);
}

namespace scheduler {
    static constexpr uint32_t IA32_FS_BASE = 0xC0000100;
    static constexpr uint32_t IA32_KERNEL_GS_BASE = 0xC0000102;

    static uint64_t next_tid = 2;
    static task *init_task = nullptr;
    static task *all_tasks_head = nullptr;
    static spinlock_t process_lock;
    static bool use_xsave = false;
    static uint64_t xsave_mask = 0x3;
    static uint32_t fpu_state_size = 512;
    static constexpr uint32_t STEAL_MIN_REMOTE_READY = 1;

    static void enqueue_ready_on_best_cpu(smp::cpu_local *preferred, task *t);

    static constexpr uint64_t LAUNCH_SEARCH_MAX_ENTRIES = 32;
    static constexpr uint64_t LAUNCH_SEARCH_MAX_TEXT = 4096;
    static constexpr uint64_t LAUNCH_SEARCH_MAX_PATH = 4096;
    static constexpr uint64_t LAUNCH_SEARCH_MAX_NAME = 255;
    static const char *const DEFAULT_LAUNCH_SEARCH[] = {"/bin"};

    static void enqueue(smp::cpu_local *cpu, task *t);

    static inline uint32_t read_pkru() {
        uint32_t eax = 0, edx = 0;
        asm volatile("rdpkru" : "=a"(eax), "=d"(edx) : "c"(0));
        return eax;
    }

    static inline void write_pkru(uint32_t value) {
        asm volatile("wrpkru" : : "a"(value), "c"(0), "d"(0) : "memory");
    }

    static inline void save_pkru(smp::cpu_local *cpu, task *t) {
        if (!cpu || !t || !cpu->cpu_features.pku) return;
        t->pkru = t->type == task_type::USER ? read_pkru() : 0;
    }

    static inline void restore_pkru(smp::cpu_local *cpu, task *t) {
        if (!cpu || !t || !cpu->cpu_features.pku) return;
        write_pkru(t->type == task_type::USER ? t->pkru : 0);
    }

    static inline void sched_unlock_keep_interrupts_disabled(smp::cpu_local *cpu) {
        __atomic_clear(&cpu->sched_lock.locked, __ATOMIC_RELEASE);
        asm volatile("" ::: "memory");
    }

    static inline void restore_flags(uint64_t flags) {
        asm volatile("push %0; popf" : : "r"(flags) : "memory", "cc");
    }

    static inline void save_user_segment_bases(smp::cpu_local *cpu, task *t) {
        if (!cpu || !t || t->type != task_type::USER) return;

        t->fs_base = apic::rdmsr(IA32_FS_BASE);
        t->gs_base = apic::rdmsr(IA32_KERNEL_GS_BASE);
    }

    static inline void restore_segment_bases(smp::cpu_local *cpu, task *t) {
        if (!cpu || !t) return;

        if (t->type == task_type::USER) {
            apic::wrmsr(IA32_FS_BASE, t->fs_base);
            apic::wrmsr(IA32_KERNEL_GS_BASE, t->gs_base);
        } else {
            apic::wrmsr(IA32_FS_BASE, 0);
            apic::wrmsr(IA32_KERNEL_GS_BASE, (uintptr_t)cpu);
        }
    }

    static const char *state_name(task_state state) {
        switch (state) {
            case task_state::READY: return "READY";
            case task_state::RUNNING: return "RUNNING";
            case task_state::SLEEPING: return "SLEEPING";
            case task_state::WAITING: return "WAITING";
            case task_state::ZOMBIE: return "ZOMBIE";
            case task_state::BLOCKED: return "BLOCKED";
            case task_state::FROZEN: return "FROZEN";
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
            case vma_type::DEVICE: return "DEVICE";
        }
        return "UNKNOWN";
    }

    static bool is_page_range(uint64_t start, uint64_t end) {
        return start != 0 && end > start && (start % pmm::PAGE_SIZE) == 0 &&
               (end % pmm::PAGE_SIZE) == 0 && end < vmm::user_top();
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
        area->pkey = 0;
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
            copy->pkey = area->pkey;
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

    static uint64_t random_user_stack_top(uint64_t user_top) {
        uint64_t max_pages = USER_STACK_ASLR_WINDOW / pmm::PAGE_SIZE;
        uint64_t guard_pages = MAX_USER_STACK_SIZE / pmm::PAGE_SIZE;
        uint64_t slide = random_page_offset(max_pages - guard_pages);
        return user_top - slide;
    }


    static uint64_t initial_user_stack_resident_size(uint64_t required) {
        if (required > INITIAL_USER_STACK_SIZE) return 0;
        uint64_t size = required < INITIAL_USER_STACK_RESIDENT_MIN ?
                        INITIAL_USER_STACK_RESIDENT_MIN : required;
        return align_up(size, pmm::PAGE_SIZE);
    }

    static uint64_t random_mmap_base(task_abi abi = task_abi::USER64) {
        uint64_t base = abi == task_abi::USER32 ? USER32_MMAP_BASE_MIN : vmm::user_mmap_base_min();
        uint64_t window = abi == task_abi::USER32 ? USER32_MMAP_ASLR_WINDOW : vmm::user_mmap_aslr_window();
        uint64_t pages = window / pmm::PAGE_SIZE;
        return base + random_page_offset(pages);
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

    static task *find_init_task() { return init_task; }

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


    static void unlink_from_parent(task *t) {
        if (!t || !t->parent) return;
        task **link = &t->parent->first_child;
        while (*link) {
            if (*link == t) {
                *link = t->next_sibling;
                t->next_sibling = nullptr;
                t->parent = nullptr;
                return;
            }
            link = &(*link)->next_sibling;
        }
        t->parent = nullptr;
    }

    static bool is_plain_exec_name(const char *name, uint64_t *len_out) {
        if (!name || !name[0]) return false;
        uint64_t len = 0;
        for (; name[len]; len++) {
            if (len >= LAUNCH_SEARCH_MAX_NAME || name[len] == '/') return false;
        }
        if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return false;
        if (len_out) *len_out = len;
        return true;
    }

    static bool valid_launch_search_path(const char *path, uint64_t len) {
        if (!path || len == 0 || len >= LAUNCH_SEARCH_MAX_PATH || path[0] != '/') return false;
        if (len == 1) return true;
        if (path[len - 1] == '/') return false;

        uint64_t comp_start = 1;
        uint64_t comp_len = 0;
        for (uint64_t i = 1; i < len; i++) {
            char c = path[i];
            if (c == 0 || c == '\n' || c == '\r') return false;
            if (c == '/') {
                if (comp_len == 0 || comp_len > 255) return false;
                if ((comp_len == 1 && path[comp_start] == '.') ||
                    (comp_len == 2 && path[comp_start] == '.' && path[comp_start + 1] == '.')) return false;
                comp_start = i + 1;
                comp_len = 0;
            } else {
                comp_len++;
            }
        }
        if (comp_len == 0 || comp_len > 255) return false;
        if ((comp_len == 1 && path[comp_start] == '.') ||
            (comp_len == 2 && path[comp_start] == '.' && path[comp_start + 1] == '.')) return false;
        return true;
    }

    static void free_launch_search(task *t) {
        if (!t || !t->launch_search) return;
        for (uint64_t i = 0; i < t->launch_search_count; i++) heap::kfree(t->launch_search[i]);
        heap::kfree(t->launch_search);
        t->launch_search = nullptr;
        t->launch_search_count = 0;
    }

    static bool install_launch_search(task *t, char **entries, uint64_t count) {
        if (!t || (!entries && count)) return false;
        free_launch_search(t);
        t->launch_search = entries;
        t->launch_search_count = count;
        return true;
    }

    static void free_launch_search_array(char **entries, uint64_t count) {
        if (!entries) return;
        for (uint64_t i = 0; i < count; i++) heap::kfree(entries[i]);
        heap::kfree(entries);
    }

    static bool duplicate_launch_search(const char *const *src, uint64_t count, char ***out) {
        if (!out || (!src && count) || count > LAUNCH_SEARCH_MAX_ENTRIES) return false;
        *out = nullptr;
        char **entries = (char **)heap::kcalloc(count ? count : 1, sizeof(char *));
        if (!entries) return false;
        for (uint64_t i = 0; i < count; i++) {
            uint64_t len = strlen(src[i]);
            if (!valid_launch_search_path(src[i], len)) {
                free_launch_search_array(entries, i);
                return false;
            }
            entries[i] = (char *)heap::kmalloc(len + 1);
            if (!entries[i]) {
                free_launch_search_array(entries, i);
                return false;
            }
            memcpy(entries[i], src[i], len + 1);
        }
        *out = entries;
        return true;
    }

    static bool inherit_launch_search(task *child, const task *parent) {
        const char *const *src = DEFAULT_LAUNCH_SEARCH;
        uint64_t count = sizeof(DEFAULT_LAUNCH_SEARCH) / sizeof(DEFAULT_LAUNCH_SEARCH[0]);
        if (parent && parent->launch_search && parent->launch_search_count) {
            src = (const char *const *)parent->launch_search;
            count = parent->launch_search_count;
        }
        char **entries = nullptr;
        if (!duplicate_launch_search(src, count, &entries)) return false;
        return install_launch_search(child, entries, count);
    }

    static task *task_for_launch_file(uint64_t pid) {
        if (pid == UINT64_MAX) {
            smp::cpu_local *cpu = smp::get_cpu();
            return cpu ? cpu->current_task : nullptr;
        }
        return get_task_by_id(pid);
    }



    static task *current_task() {
        smp::cpu_local *cpu = smp::get_cpu();
        return cpu ? cpu->current_task : nullptr;
    }

    static task *task_for_event_file(uint64_t pid) {
        if (pid == UINT64_MAX) return current_task();
        return get_task_by_id(pid);
    }

    static bool valid_user_event_type(uint32_t type) {
        return type == TASK_EVENT_QUIT || type == TASK_EVENT_FORCE_KILL || type == TASK_EVENT_USER;
    }

    static bool fault_event_type(uint32_t type) {
        switch (type) {
            case TASK_EVENT_PAGE_FAULT:
            case TASK_EVENT_GENERAL_FAULT:
            case TASK_EVENT_DIVIDE_BY_ZERO:
            case TASK_EVENT_INVALID_OPCODE:
            case TASK_EVENT_BREAKPOINT:
            case TASK_EVENT_OVERFLOW:
            case TASK_EVENT_BOUNDS:
            case TASK_EVENT_STACK_FAULT:
            case TASK_EVENT_FPU_FAULT:
            case TASK_EVENT_ALIGNMENT_FAULT:
            case TASK_EVENT_USER_FAULT:
                return true;
            default:
                return false;
        }
    }

    static int fatal_code_for(uint32_t type) {
        return 0x10000 | (int)type;
    }

    static void regs_to_fault_regs(const regs *src, task_fault_regs *dst) {
        if (!src || !dst) return;
        dst->r15 = src->r15; dst->r14 = src->r14; dst->r13 = src->r13; dst->r12 = src->r12;
        dst->r11 = src->r11; dst->r10 = src->r10; dst->r9 = src->r9; dst->r8 = src->r8;
        dst->rbp = src->rbp; dst->rdi = src->rdi; dst->rsi = src->rsi; dst->rdx = src->rdx;
        dst->rcx = src->rcx; dst->rbx = src->rbx; dst->rax = src->rax; dst->rip = src->rip;
        dst->rflags = src->rflags; dst->rsp = src->rsp;
    }


    static bool enqueue_event_locked(task *t, const task_event &ev) {
        if (!t || t->state == task_state::ZOMBIE) return false;
        event_queue &q = t->events;
        if (q.count >= TASK_EVENT_QUEUE_SIZE) {
            if (q.dropped != UINT32_MAX) q.dropped++;
            return false;
        }
        uint32_t pos = (q.head + q.count) % TASK_EVENT_QUEUE_SIZE;
        q.entries[pos] = ev;
        if (q.dropped) {
            q.entries[pos].flags |= TASK_EVENT_F_DROPPED;
            q.dropped = 0;
        }
        q.count++;
        return true;
    }

    static void wake_blocked_or_waiting(task *t) {
        if (!t || (t->state != task_state::WAITING && t->state != task_state::BLOCKED)) return;
        smp::cpu_local *cpu = smp::get_cpu_by_index(t->last_cpu_index);
        if (!cpu) return;
        uint64_t flags;
        cpu->sched_lock.acquire(flags);
        if (t->state == task_state::WAITING || t->state == task_state::BLOCKED) {
            t->state = task_state::READY;
            enqueue_ready_on_best_cpu(cpu, t);
        }
        cpu->sched_lock.release(flags);
    }


    static void notify_parent_exit_locked(task *child, uint32_t type, int code, uint64_t detail) {
        if (!child || !child->parent) return;
        task_event ev{};
        ev.type = type;
        ev.source = child->id;
        ev.target = child->parent->id;
        ev.code = code;
        ev.detail = detail;
        enqueue_event_locked(child->parent, ev);
    }

    static void terminate_current_locked(task *current, int code, uint32_t parent_type, uint64_t detail) {
        current->exit_code = code;
        current->state = task_state::ZOMBIE;
        current->fault.active = false;
        current->fault.return_pending = false;
        reparent_children(current);
        notify_parent_exit_locked(current, parent_type, code, detail);
    }

    static uint64_t fd_bitmap_words(uint64_t capacity) {
        return (capacity + 63) / 64;
    }

    static void release_fd_table_refs(task *t) {
        if (!t || !t->fd_table) return;
        for (uint32_t i = 0; i < t->fd_capacity; i++) {
            vfs::open_file *file = t->fd_table[i];
            if (!file) continue;
            uint64_t flags;
            spinlock_acquire(&file->lock, &flags);
            if (file->ref_count > 0) file->ref_count--;
            uint32_t refs = file->ref_count;
            spinlock_release(&file->lock, flags);
            if (refs == 0) {
                devfs_close_file(file);
                vfs::put_node(file->node);
                heap::kfree(file);
            }
            t->fd_table[i] = nullptr;
        }
    }

    static void free_failed_clone(task *child, bool owns_address_space) {
        if (!child) return;
        unlink_from_parent(child);
        release_fd_table_refs(child);
        if (child->fd_table) { heap::kfree(child->fd_table); child->fd_table = nullptr; }
        if (child->fd_bitmap) { heap::kfree(child->fd_bitmap); child->fd_bitmap = nullptr; }
        if (child->syscall_iobuf) { heap::kfree(child->syscall_iobuf); child->syscall_iobuf = nullptr; }
        if (owns_address_space && child->cr3 && child->cr3 != vmm::get_kernel_pagemap()) {
            vmm::destroy_user_address_space(child->cr3);
        }
        clear_vmas(child);
        if (child->kernel_stack) vmm::free_kernel_stack(child->kernel_stack, KERNEL_STACK_SIZE);
        if (child->fpu_storage) heap::kfree(child->fpu_storage);
        if (child->cwd_path) heap::kfree(child->cwd_path);
        free_launch_search(child);
        heap::kfree(child);
    }

    static void free_task_resources(task *t) {
        if (!t) return;

        if (t->type == task_type::USER && t->cr3 != 0 && t->cr3 != vmm::get_kernel_pagemap() &&
            !address_space_used_by_other(t->cr3, t)) {
            vmm::destroy_user_address_space(t->cr3);
        }
        clear_vmas(t);

        if (t->kernel_stack) vmm::free_kernel_stack(t->kernel_stack, KERNEL_STACK_SIZE);
        if (t->fpu_storage) heap::kfree(t->fpu_storage);
        if (t->fd_table) heap::kfree(t->fd_table);
        if (t->fd_bitmap) heap::kfree(t->fd_bitmap);
        if (t->syscall_iobuf) heap::kfree(t->syscall_iobuf);
        if (t->cwd) { vfs::put_node(t->cwd); t->cwd = nullptr; }
        if (t->cwd_path) heap::kfree(t->cwd_path);
        free_launch_search(t);
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

    static uint64_t queue_oldest_tick(smp::cpu_local *cpu, int p) {
        return cpu->task_queues_head[p] ? cpu->task_queues_head[p]->enqueue_tick : 0;
    }

    static void refresh_queue_oldest(smp::cpu_local *cpu, int p) {
        cpu->task_queue_oldest_tick[p] = queue_oldest_tick(cpu, p);
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
        if (cpu->task_queue_count[p] == 0 || cpu->runnable_count == 0) {
            kpanic("scheduler queue count underflow");
        }
        cpu->task_queue_count[p]--;
        cpu->runnable_count--;
        refresh_queue_oldest(cpu, p);
    }

    static void enqueue(smp::cpu_local *cpu, task *t) {
        if (!cpu || !t) kpanic("scheduler enqueue null");
        if (t->state != task_state::READY) {
            console::printf("[SCHED] Tried to enqueue not-ready task %d", (uint64_t)t->id);
        }

        int p = t->priority;
        if (p < 0 || p >= MAX_QUEUES) kpanic("scheduler enqueue priority corrupt");
        if (t->next || t->prev) kpanic("scheduler enqueue linked task");
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
        cpu->task_queue_count[p]++;
        cpu->runnable_count++;
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

    static smp::cpu_local *select_enqueue_cpu(smp::cpu_local *preferred) {
        if (!preferred) return nullptr;
        uint64_t cores = smp::get_core_count();
        if (cores <= 1) return preferred;

        smp::cpu_local *best = preferred;
        uint32_t best_load = preferred->runnable_count;
        uint32_t preferred_load = best_load;

        for (uint64_t i = 0; i < cores; i++) {
            smp::cpu_local *candidate = smp::get_cpu_by_index(i);
            if (!candidate || candidate == preferred) continue;
            uint64_t flags;
            if (!candidate->sched_lock.try_acquire(flags)) continue;
            uint32_t load = candidate->runnable_count;
            candidate->sched_lock.release(flags);
            if (load + 1 < best_load) {
                best = candidate;
                best_load = load;
            }
        }

        return preferred_load > best_load + 1 ? best : preferred;
    }

    static void enqueue_ready_on_best_cpu(smp::cpu_local *preferred, task *t) {
        smp::cpu_local *target = select_enqueue_cpu(preferred);
        if (!target || target == preferred) {
            enqueue(preferred, t);
            return;
        }

        uint64_t target_flags;
        if (target->sched_lock.try_acquire(target_flags)) {
            enqueue(target, t);
            target->sched_lock.release(target_flags);
            smp::send_reschedule_mail(target->cpu_index);
        } else {
            enqueue(preferred, t);
        }
    }

    static task *steal_one_from_locked_victim(smp::cpu_local *self, smp::cpu_local *victim) {
        if (!self || !victim || self == victim) return nullptr;
        if (victim->runnable_count < STEAL_MIN_REMOTE_READY) return nullptr;

        for (int p = MAX_QUEUES - 1; p >= 0; p--) {
            task *t = victim->task_queues_tail[p];
            if (!t) continue;
            unlink_queued(victim, t, p);
            t->last_cpu_index = self->cpu_index;
            return t;
        }
        return nullptr;
    }

    static task *steal_task(smp::cpu_local *self) {
        uint64_t cores = smp::get_core_count();
        if (!self || cores <= 1) return nullptr;

        self->steal_attempts++;
        uint64_t start = (self->cpu_index + 1) % cores;
        smp::cpu_local *best = nullptr;
        uint64_t best_flags = 0;
        uint32_t best_load = 0;

        for (uint64_t n = 0; n < cores; n++) {
            smp::cpu_local *victim = smp::get_cpu_by_index((start + n) % cores);
            if (!victim || victim == self) continue;

            uint64_t flags;
            if (!victim->sched_lock.try_acquire(flags)) {
                self->steal_locked++;
                continue;
            }

            uint32_t load = victim->runnable_count;
            if (load > best_load) {
                if (best) best->sched_lock.release(best_flags);
                best = victim;
                best_flags = flags;
                best_load = load;
            } else {
                victim->sched_lock.release(flags);
            }
        }

        if (!best || best_load < STEAL_MIN_REMOTE_READY) {
            if (best) best->sched_lock.release(best_flags);
            self->steal_empty++;
            return nullptr;
        }

        task *t = steal_one_from_locked_victim(self, best);
        best->sched_lock.release(best_flags);
        if (!t) {
            self->steal_empty++;
            return nullptr;
        }

        t->enqueue_tick = self->ticks;
        self->steal_successes++;
        return t;
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

        task *stolen = steal_task(cpu);
        if (stolen) return stolen;
        return cpu->idle_task;
    }

    void init_core() {
        smp::cpu_local *cpu = smp::get_cpu();
        if (cpu->cpu_features.xsave) {
            uint64_t xcr0 = xgetbv(0);
            use_xsave = true;
            xsave_mask = xcr0 & 0xE7;
            if ((xsave_mask & 0x3) != 0x3) kpanic("Invalid XCR0 base state");
        }
        detect_fpu_state_size();

        task *idle = (task *)heap::kzalloc(sizeof(task));
        if (!idle) kpanic("Idle task allocation failed");

        idle->id = 0;
        idle->priority = MAX_QUEUES - 1;
        idle->last_cpu_index = cpu->cpu_index;
        idle->cr3 = vmm::get_kernel_pagemap();
        idle->state = task_state::RUNNING;
        idle->type = task_type::KERNEL;
        idle->abi = task_abi::KERNEL;
        idle->kernel_stack = vmm::alloc_kernel_stack(KERNEL_STACK_SIZE);
        if (!idle->kernel_stack) kpanic("Idle kernel stack allocation failed");
        idle->cwd = vfs::get_node(vfs::get_root());
        if (!idle->cwd) kpanic("Idle cwd allocation failed");
        idle->cwd_path = strdup("/");
        idle->cwd_path_len = 1;
        idle->pkru = 0;
        init_fpu_state(idle);

        uint64_t process_flags;
        spinlock_acquire(&process_lock, &process_flags);
        link_global(idle);
        spinlock_release(&process_lock, process_flags);

        cpu->idle_task = idle;
        cpu->current_task = idle;
        cpu->kernel_stack = (void *)((uintptr_t)idle->kernel_stack + KERNEL_STACK_SIZE);
        cpu->tss_entry.rsp0 = (uint64_t)cpu->kernel_stack;
        syscall_set_kernel_stack((uint64_t)cpu->kernel_stack);
    }

    static uint64_t allocate_task_id_locked() {
        uint64_t id = next_tid++;
        if (id == 1) id = next_tid++;
        return id;
    }

    static void install_init_stdio(task *t) {
        vfs::vfs_node *con = vfs::open("/dev/console");
        if (!con || !vfs::get_node(con)) return;

        vfs::open_file *file = (vfs::open_file *)heap::kzalloc(sizeof(vfs::open_file));
        if (!file) {
            vfs::put_node(con);
            kpanic("console fd allocation failed");
        }
        file->node = con;
        file->offset = 0;
        file->ref_count = 2;
        spinlock_init(&file->lock);

        t->fd_table[0] = nullptr;
        t->fd_table[1] = file;
        t->fd_table[2] = file;
        if (t->fd_bitmap) t->fd_bitmap[0] |= (1ULL << 1) | (1ULL << 2);
        if (t->next_fd_hint < 3) t->next_fd_hint = 3;
    }

    static task *spawn_internal(task_type type, void (*entry_point)(), uint64_t pml4,
                                uint64_t heap_start, const elf::elf_info *image,
                                task_state initial_state, uint64_t forced_id,
                                bool init_stdio) {
        if (type == task_type::USER && pml4 == 0) { kpanic("User task requires a valid PML4"); }
        if (initial_state != task_state::READY && initial_state != task_state::FROZEN) {
            kpanic("invalid initial task state");
        }
        if (forced_id == 1 && init_task) kpanic("init task already exists");
        if (forced_id != 0 && forced_id != 1) kpanic("invalid forced pid");

        smp::cpu_local *cpu = smp::get_cpu();
        task *t = (task *)heap::kzalloc(sizeof(task));
        if (!t) kpanic("Task allocation failed");

        void *kstack = vmm::alloc_kernel_stack(KERNEL_STACK_SIZE);
        if (!kstack) kpanic("Kernel stack allocation failed");
        t->kernel_stack = kstack;

        regs *r = (regs *)((uintptr_t)kstack + KERNEL_STACK_SIZE - sizeof(regs));
        memset(r, 0, sizeof(regs));

        t->type = type;
        t->abi = type == task_type::USER ? (image && image->is_32bit ? task_abi::USER32 : task_abi::USER64)
                                         : task_abi::KERNEL;
        uint64_t id_flags;
        spinlock_acquire(&process_lock, &id_flags);
        t->id = forced_id ? forced_id : allocate_task_id_locked();
        spinlock_release(&process_lock, id_flags);
        t->context = r;

        t->program_break = heap_start;
        t->mmap_hint = type == task_type::USER ? random_mmap_base(t->abi) : 0;

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

        t->fd_capacity = INITIAL_FD_CAPACITY;
        t->next_fd_hint = 0;
        t->fd_table = (vfs::open_file **)heap::kcalloc(t->fd_capacity, sizeof(vfs::open_file *));
        t->fd_bitmap = (uint64_t *)heap::kcalloc(fd_bitmap_words(t->fd_capacity), sizeof(uint64_t));
        if (!t->fd_table || !t->fd_bitmap) kpanic("fd table allocation failed");
        t->cwd = vfs::get_node(t->parent && t->parent->cwd ? t->parent->cwd : vfs::get_root());
        if (!t->cwd) kpanic("cwd node allocation failed");
        t->cwd_path = strdup(t->parent && t->parent->cwd_path ? t->parent->cwd_path : "/");
        if (!t->cwd_path) kpanic("cwd allocation failed");
        t->cwd_path_len = strlen(t->cwd_path);
        if (!inherit_launch_search(t, t->parent)) kpanic("launch search allocation failed");

        if (init_stdio) install_init_stdio(t);

        if (type == task_type::USER) {
            uint64_t stack_resident_size = initial_user_stack_resident_size(256);
            if (!stack_resident_size) kpanic("User stack resident size invalid");
            uint64_t stack_phys = pmm::alloc(stack_resident_size);
            if (!stack_phys) kpanic("User stack allocation failed");

            uint64_t stack_top = random_user_stack_top(t->abi == task_abi::USER32 ? USER32_TOP : vmm::user_stack_top());
            uint64_t stack_virt = stack_top - stack_resident_size;

            uint64_t stack_low = stack_top - MAX_USER_STACK_SIZE;
            vmm::map_guard_page(stack_low, vmm::PageFlags::User | vmm::PageFlags::NX, pml4);
            vmm::map_range(stack_virt, stack_phys, stack_resident_size,
                           vmm::PageFlags::Write | vmm::PageFlags::User | vmm::PageFlags::NX, pml4);
            memset(p2v(stack_phys), 0, stack_resident_size);

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
            auto push_u32 = [&](uint32_t value) {
                stack_ptr -= sizeof(uint32_t);
                *(uint32_t *)((uintptr_t)p2v(stack_phys) + (stack_ptr - stack_virt)) = value;
            };

            if (t->abi == task_abi::USER32) {
                stack_ptr &= ~0xFULL;
                push_u32(0);
                push_u32(0);
            } else {
                stack_ptr &= ~0xFULL;
                push_u64(0);
                push_u64(0);
                push_u64(at_random_ptr);
                push_u64(25);
                push_u64(0);
                uint64_t auxv_ptr = stack_ptr;
                push_u64(0);
                r->rsi = stack_ptr;
                r->rdx = auxv_ptr;
            }

            t->stack_vma = add_vma(t, stack_low, stack_top, stack_low + pmm::PAGE_SIZE,
                                   VMA_READ | VMA_WRITE | VMA_GROWSDOWN, vma_type::STACK);
            if (!t->stack_vma) kpanic("Failed to create stack VMA");
            r->rip = (uint64_t)entry_point;
            r->rsp = stack_ptr;
            r->rdi = 0;
            r->cs = t->abi == task_abi::USER32 ? gdt::selectors::UCODE32_SEL : gdt::selectors::UCODE64_SEL;
            r->ss = t->abi == task_abi::USER32 ? gdt::selectors::UDATA32_SEL : gdt::selectors::UDATA64_SEL;
            t->cr3 = pml4;
        } else {
            r->rip = (uint64_t)entry_point;
            r->rsp = (uint64_t)kstack + KERNEL_STACK_SIZE;
            r->cs = gdt::selectors::KCODE_SEL;
            r->ss = gdt::selectors::KDATA_SEL;
            t->cr3 = pml4 == 0 ? vmm::get_kernel_pagemap() : pml4;
        }

        r->rflags = 0x202;
        t->state = initial_state;
        t->priority = 0;
        t->quantum = TIME_QUANTUMS[0];
        t->last_cpu_index = cpu->cpu_index;

        t->pkru = 0;
        init_fpu_state(t);

        uint64_t flags;
        cpu->sched_lock.acquire(flags);
        uint64_t process_flags;
        spinlock_acquire(&process_lock, &process_flags);
        if (forced_id == 1) init_task = t;
        if (t->parent) {
            t->next_sibling = t->parent->first_child;
            t->parent->first_child = t;
        }
        link_global(t);
        if (t->state == task_state::READY) enqueue_ready_on_best_cpu(cpu, t);
        spinlock_release(&process_lock, process_flags);
        cpu->sched_lock.release(flags);

        return t;
    }

    task *spawn(task_type type, void (*entry_point)(), uint64_t pml4, uint64_t heap_start,
                const elf::elf_info *image) {
        return spawn_internal(type, entry_point, pml4, heap_start, image, task_state::READY, 0, false);
    }

    task *spawn_init(void (*entry_point)(), uint64_t pml4, uint64_t heap_start,
                     const elf::elf_info *image) {
        return spawn_internal(task_type::USER, entry_point, pml4, heap_start, image,
                              task_state::FROZEN, 1, true);
    }

    int clone(uint64_t flags, void *child_stack, regs *current_regs) {
        smp::cpu_local *cpu = smp::get_cpu();
        task *parent = cpu->current_task;
        save_user_segment_bases(cpu, parent);
        save_pkru(cpu, parent);

        task *child = (task *)heap::kmalloc(sizeof(task));
        if (!child) return -1;
        memcpy(child, parent, sizeof(task));

        uint64_t child_id_flags;
        spinlock_acquire(&process_lock, &child_id_flags);
        child->id = allocate_task_id_locked();
        spinlock_release(&process_lock, child_id_flags);
        child->parent = parent;
        child->first_child = nullptr;
        child->state = task_state::READY;

        child->next = nullptr;
        child->prev = nullptr;
        child->global_next = nullptr;
        child->global_prev = nullptr;
        child->next_sibling = nullptr;
        child->fd_table = nullptr;
        child->fd_bitmap = nullptr;
        child->fd_capacity = 0;
        child->next_fd_hint = 0;
        child->syscall_iobuf = nullptr;
        memset(&child->events, 0, sizeof(child->events));
        memset(&child->fault.frame, 0, sizeof(child->fault.frame));
        memset(&child->fault.result, 0, sizeof(child->fault.result));
        memset(&child->fault.saved_regs, 0, sizeof(child->fault.saved_regs));
        child->fault.active = false;
        child->fault.return_pending = false;
        child->fault.depth = 0;
        child->kill_pending = false;
        child->kill_code = 0;
        child->kill_source = 0;
        child->launch_search = nullptr;
        child->launch_search_count = 0;
        child->kernel_stack = nullptr;
        child->context = nullptr;

        child->vma_list = nullptr;
        child->heap_vma = nullptr;
        child->stack_vma = nullptr;
        child->fpu_area = nullptr;
        child->fpu_storage = nullptr;
        child->cwd = vfs::get_node(parent->cwd ? parent->cwd : vfs::get_root());
        if (!child->cwd) {
            heap::kfree(child);
            return -1;
        }
        child->cwd_path = strdup(parent->cwd_path ? parent->cwd_path : "/");
        if (!child->cwd_path) {
            vfs::put_node(child->cwd);
            heap::kfree(child);
            return -1;
        }
        child->cwd_path_len = strlen(child->cwd_path);
        if (!inherit_launch_search(child, parent)) {
            free_failed_clone(child, false);
            return -1;
        }
        if (!clone_vmas(child, parent)) {
            free_failed_clone(child, false);
            return -1;
        }

        // 2. Handle Address Space (CoW vs Shared)
        if (flags & 0x1) {  // CLONE_VM
            child->cr3 = parent->cr3;
        } else {
            child->cr3 = vmm::clone_address_space(parent->cr3);
            if (!child->cr3) {
                free_failed_clone(child, false);
                return -1;
            }
        }
        bool child_owns_address_space = (child->cr3 != parent->cr3);

        // 3. Handle File Descriptors. CLONE_FILES is deliberately not shared until
        // descriptor-table refcounts exist. Sharing the raw table corrupts the parent
        // on child exit.
        child->fd_table = (vfs::open_file **)heap::kcalloc(parent->fd_capacity, sizeof(vfs::open_file *));
        child->fd_bitmap = (uint64_t *)heap::kmalloc_array(fd_bitmap_words(parent->fd_capacity), sizeof(uint64_t));
        child->fd_capacity = parent->fd_capacity;
        child->next_fd_hint = parent->next_fd_hint;
        if (!child->fd_table || !child->fd_bitmap) {
            free_failed_clone(child, child_owns_address_space);
            return -1;
        }
        memcpy(child->fd_bitmap, parent->fd_bitmap, sizeof(uint64_t) * fd_bitmap_words(child->fd_capacity));
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
        child->kernel_stack = vmm::alloc_kernel_stack(KERNEL_STACK_SIZE);
        if (!child->kernel_stack) {
            free_failed_clone(child, child_owns_address_space);
            return -1;
        }

        // We need to find where the regs struct sits relative to the parent's stack base
        uint64_t stack_offset = (uintptr_t)current_regs - (uintptr_t)parent->kernel_stack;
        child->context = (regs *)((uintptr_t)child->kernel_stack + stack_offset);

        // Copy the registers (the snapshot of the CPU state)
        memcpy(child->context, current_regs, sizeof(regs));

        // 5. Adjust for the Child
        if (child_stack) { child->context->rsp = (uint64_t)child_stack; }

        // The child must return 0 from the syscall
        child->context->rax = 0;

        // 6. Copy FPU state
        child->fpu_storage = heap::kmalloc(fpu_state_size + 63);
        if (!child->fpu_storage) {
            free_failed_clone(child, child_owns_address_space);
            return -1;
        }
        child->fpu_area = (void *)(((uintptr_t)child->fpu_storage + 63U) & ~63ULL);
        memcpy(child->fpu_area, parent->fpu_area, fpu_state_size);

        // 7. Enqueue child
        uint64_t f;
        cpu->sched_lock.acquire(f);
        uint64_t process_flags;
        spinlock_acquire(&process_lock, &process_flags);
        child->next_sibling = parent->first_child;
        parent->first_child = child;
        link_global(child);
        enqueue_ready_on_best_cpu(cpu, child);
        spinlock_release(&process_lock, process_flags);
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

        exec_arg *args = (exec_arg *)heap::kcalloc(argc, sizeof(exec_arg));
        if (argc != 0 && !args) return false;

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

    static void *user_stack_to_kernel(uint64_t stack_phys, uint64_t stack_base, uint64_t stack_size,
                                      uint64_t addr, uint64_t len) {
        if (addr < stack_base || len > stack_size) return nullptr;
        uint64_t off = addr - stack_base;
        if (off > stack_size || len > stack_size - off) return nullptr;
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

        uint64_t initial_stack_bytes = 16 + 16;
        for (int i = 0; i < argc; i++) {
            if (args[i].len > INITIAL_USER_STACK_SIZE - initial_stack_bytes) {
                free_exec_args(args, argc);
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }
            initial_stack_bytes += args[i].len;
        }
        initial_stack_bytes += info.is_32bit ? (uint64_t)(argc + 3) * sizeof(uint32_t)
                                             : (uint64_t)(argc + 6) * sizeof(uint64_t);

        uint64_t stack_resident_size = initial_user_stack_resident_size(initial_stack_bytes);
        if (!stack_resident_size) {
            free_exec_args(args, argc);
            vmm::destroy_user_address_space(info.pml4);
            return -1;
        }

        uint64_t stack_phys = pmm::alloc(stack_resident_size);
        if (!stack_phys) {
            free_exec_args(args, argc);
            vmm::destroy_user_address_space(info.pml4);
            return -1;
        }
        memset(p2v(stack_phys), 0, stack_resident_size);

        uint64_t stack_top = random_user_stack_top(info.is_32bit ? USER32_TOP : vmm::user_stack_top());
        uint64_t stack_virt = stack_top - stack_resident_size;
        uint64_t initial_stack_low = stack_top - INITIAL_USER_STACK_SIZE;
        vmm::map_range(stack_virt, stack_phys, stack_resident_size,
                       vmm::PageFlags::Write | vmm::PageFlags::User | vmm::PageFlags::NX, info.pml4);

        constexpr int MAX_EXEC_ARGC = 64;
        uint64_t stack_ptr = stack_top;
        uint64_t user_argv_ptrs[MAX_EXEC_ARGC + 1];
        memset(user_argv_ptrs, 0, sizeof(user_argv_ptrs));

        for (int i = argc - 1; i >= 0; i--) {
            size_t len = args[i].len;
            if (len == 0 || len > stack_ptr - initial_stack_low) {
                free_exec_args(args, argc);
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }

            stack_ptr -= len;
            void *dst = user_stack_to_kernel(stack_phys, stack_virt, stack_resident_size, stack_ptr, len);
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
        void *random_slot = user_stack_to_kernel(stack_phys, stack_virt, stack_resident_size, stack_ptr,
                                                 sizeof(random_bytes));
        if (!random_slot) {
            vmm::destroy_user_address_space(info.pml4);
            return -1;
        }
        memcpy(random_slot, random_bytes, sizeof(random_bytes));
        uint64_t at_random_ptr = stack_ptr;

        auto push_u64 = [&](uint64_t value) -> bool {
            stack_ptr -= sizeof(uint64_t);
            void *slot = user_stack_to_kernel(stack_phys, stack_virt, stack_resident_size, stack_ptr,
                                              sizeof(uint64_t));
            if (!slot) return false;
            *(uint64_t *)slot = value;
            return true;
        };
        auto push_u32 = [&](uint32_t value) -> bool {
            stack_ptr -= sizeof(uint32_t);
            void *slot = user_stack_to_kernel(stack_phys, stack_virt, stack_resident_size, stack_ptr,
                                              sizeof(uint32_t));
            if (!slot) return false;
            *(uint32_t *)slot = value;
            return true;
        };

        uint64_t auxv_ptr_for_rdx = 0;
        uint64_t argv_ptr_for_rdi = 0;
        if (info.is_32bit) {
            stack_ptr &= ~0xFULL;
            if (!push_u32(0)) {
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }
            for (int i = argc; i >= 0; i--) {
                if (user_argv_ptrs[i] > UINT32_MAX || !push_u32((uint32_t)user_argv_ptrs[i])) {
                    vmm::destroy_user_address_space(info.pml4);
                    return -1;
                }
            }
            if (!push_u32((uint32_t)argc)) {
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }
        } else {
            stack_ptr &= ~0xFULL;
            if (!push_u64(0) || !push_u64(0) || !push_u64(at_random_ptr) || !push_u64(25) ||
                !push_u64(0)) {
                vmm::destroy_user_address_space(info.pml4);
                return -1;
            }
            auxv_ptr_for_rdx = stack_ptr;

            for (int i = argc; i >= 0; i--) {
                if (!push_u64(user_argv_ptrs[i])) {
                    vmm::destroy_user_address_space(info.pml4);
                    return -1;
                }
            }
            argv_ptr_for_rdi = stack_ptr;
        }

        task new_vm;
        memset(&new_vm, 0, sizeof(new_vm));
        new_vm.program_break = info.heap_start;
        new_vm.mmap_hint = random_mmap_base(info.is_32bit ? task_abi::USER32 : task_abi::USER64);

        uint64_t stack_low = stack_top - MAX_USER_STACK_SIZE;
        vmm::map_guard_page(stack_low, vmm::PageFlags::User | vmm::PageFlags::NX, info.pml4);

        if (info.load_base == 0 || info.load_end <= info.load_base ||
            !add_elf_image_vmas(&new_vm, info) ||
            !(new_vm.heap_vma = add_vma(&new_vm, info.heap_start, info.heap_start + pmm::PAGE_SIZE,
                                        info.heap_start, VMA_READ | VMA_WRITE, vma_type::HEAP)) ||
            !(new_vm.stack_vma = add_vma(&new_vm, stack_low, stack_top,
                                         stack_low + pmm::PAGE_SIZE,
                                         VMA_READ | VMA_WRITE | VMA_GROWSDOWN,
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
        current->abi = info.is_32bit ? task_abi::USER32 : task_abi::USER64;

        regs *r = return_frame;
        current->context = r;
        memset(r, 0, sizeof(regs));

        r->rip = info.entry;
        r->rsp = stack_ptr;
        r->rdi = info.is_32bit ? 0 : (uint64_t)argc;
        r->rsi = info.is_32bit ? 0 : argv_ptr_for_rdi;
        r->rdx = info.is_32bit ? 0 : auxv_ptr_for_rdx;
        r->rax = 0;
        r->rflags = 0x202;
        r->cs = info.is_32bit ? gdt::selectors::UCODE32_SEL : gdt::selectors::UCODE64_SEL;
        r->ss = info.is_32bit ? gdt::selectors::UDATA32_SEL : gdt::selectors::UDATA64_SEL;
        current->fs_base = 0;
        current->gs_base = 0;
        current->pkru = 0;
        memset(&current->events, 0, sizeof(current->events));
        memset(&current->fault, 0, sizeof(current->fault));
        current->kill_pending = false;
        current->kill_code = 0;
        current->kill_source = 0;
        restore_pkru(cpu, current);

        init_fpu_state(current);
        asm volatile("mov %0, %%cr3" : : "r"(current->cr3) : "memory");

        if (old_cr3 != vmm::get_kernel_pagemap() && !address_space_used_by_other(old_cr3, current)) {
            vmm::destroy_user_address_space(old_cr3);
        }

        return 0;
    }

    __attribute__((noreturn, no_stack_protector)) void schedule(regs *current_state, bool is_timer_tick) {
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
                    task *next_sleep = sleep_item->next;
                    if (prev_sleep)
                        prev_sleep->next = next_sleep;
                    else
                        cpu->sleep_list_head = next_sleep;

                    sleep_item = next_sleep;
                    to_wake->next = nullptr;
                    to_wake->prev = nullptr;
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
            save_user_segment_bases(cpu, current);
            save_pkru(cpu, current);
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
                    sched_unlock_keep_interrupts_disabled(cpu);
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

        restore_segment_bases(cpu, next);
        restore_pkru(cpu, next);
        restore_fpu(next);

        uint64_t kstack_top = (uint64_t)next->kernel_stack + KERNEL_STACK_SIZE;
        cpu->tss_entry.rsp0 = kstack_top;
        cpu->kernel_stack = (void *)kstack_top;
        syscall_set_kernel_stack(kstack_top);

        if (next->cr3 != 0) { asm volatile("mov %0, %%cr3" : : "r"(next->cr3) : "memory"); }

        sched_unlock_keep_interrupts_disabled(cpu);

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

    uint64_t format_launch_search(char *buf, uint64_t cap, uint64_t pid) {
        if (!buf || cap == 0) return 0;
        buf[0] = 0;
        task *t = task_for_launch_file(pid);
        if (!t) return 0;

        uint64_t flags;
        spinlock_acquire(&process_lock, &flags);
        uint64_t pos = 0;
        for (uint64_t i = 0; i < t->launch_search_count; i++) {
            const char *path = t->launch_search[i];
            uint64_t len = path ? strlen(path) : 0;
            if (pos < cap) {
                uint64_t n = len;
                if (n > cap - pos) n = cap - pos;
                if (n) memcpy(buf + pos, path, n);
            }
            pos += len;
            if (pos < cap) buf[pos] = '\n';
            pos++;
        }
        if (cap) buf[pos < cap ? pos : cap - 1] = 0;
        spinlock_release(&process_lock, flags);
        return pos;
    }

    bool set_launch_search_from_text(uint64_t pid, const char *text, uint64_t size) {
        if (!text || size == 0 || size > LAUNCH_SEARCH_MAX_TEXT) return false;
        task *t = task_for_launch_file(pid);
        smp::cpu_local *cpu = smp::get_cpu();
        task *caller = cpu ? cpu->current_task : nullptr;
        if (!t || !caller || t->type != task_type::USER) return false;
        if (pid != UINT64_MAX && caller->id != t->id && caller->id != 1) return false;

        char **entries = (char **)heap::kcalloc(LAUNCH_SEARCH_MAX_ENTRIES, sizeof(char *));
        if (!entries) return false;

        uint64_t count = 0;
        uint64_t line_start = 0;
        while (line_start < size) {
            uint64_t line_end = line_start;
            while (line_end < size && text[line_end] != '\n') line_end++;
            if (line_end > line_start && text[line_end - 1] == '\r') goto fail;
            uint64_t len = line_end - line_start;
            if (len == 0) goto fail;
            if (count >= LAUNCH_SEARCH_MAX_ENTRIES) goto fail;
            if (!valid_launch_search_path(text + line_start, len)) goto fail;
            for (uint64_t i = 0; i < count; i++) {
                if (strlen(entries[i]) == len && memcmp(entries[i], text + line_start, len) == 0) goto fail;
            }
            entries[count] = (char *)heap::kmalloc(len + 1);
            if (!entries[count]) goto fail;
            memcpy(entries[count], text + line_start, len);
            entries[count][len] = 0;
            count++;
            line_start = line_end + 1;
            if (line_end == size) break;
            if (line_start == size) break;
        }
        if (count == 0) goto fail;

        {
            uint64_t flags;
            spinlock_acquire(&process_lock, &flags);
            install_launch_search(t, entries, count);
            spinlock_release(&process_lock, flags);
        }
        return true;

    fail:
        free_launch_search_array(entries, LAUNCH_SEARCH_MAX_ENTRIES);
        return false;
    }

    char *resolve_launch_exec(task *t, const char *name) {
        uint64_t name_len = 0;
        if (!t || !is_plain_exec_name(name, &name_len)) return nullptr;

        char **dirs = nullptr;
        uint64_t count = 0;
        uint64_t flags;
        spinlock_acquire(&process_lock, &flags);
        count = t->launch_search_count;
        if (count == 0 || count > LAUNCH_SEARCH_MAX_ENTRIES) {
            spinlock_release(&process_lock, flags);
            return nullptr;
        }
        dirs = (char **)heap::kcalloc(count, sizeof(char *));
        if (!dirs) {
            spinlock_release(&process_lock, flags);
            return nullptr;
        }
        for (uint64_t i = 0; i < count; i++) {
            uint64_t len = strlen(t->launch_search[i]);
            dirs[i] = (char *)heap::kmalloc(len + 1);
            if (!dirs[i]) {
                spinlock_release(&process_lock, flags);
                free_launch_search_array(dirs, count);
                return nullptr;
            }
            memcpy(dirs[i], t->launch_search[i], len + 1);
        }
        spinlock_release(&process_lock, flags);

        for (uint64_t i = 0; i < count; i++) {
            uint64_t dir_len = strlen(dirs[i]);
            if (dir_len + 1 + name_len + 1 > LAUNCH_SEARCH_MAX_PATH) continue;
            char *candidate = (char *)heap::kmalloc(dir_len + 1 + name_len + 1);
            if (!candidate) continue;
            memcpy(candidate, dirs[i], dir_len);
            candidate[dir_len] = '/';
            memcpy(candidate + dir_len + 1, name, name_len);
            candidate[dir_len + 1 + name_len] = 0;
            vfs::vfs_node *node = vfs::open(candidate);
            if (node && node->type == vfs::VfsType::VFS_FILE) {
                free_launch_search_array(dirs, count);
                return candidate;
            }
            heap::kfree(candidate);
        }

        free_launch_search_array(dirs, count);
        return nullptr;
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



    uint64_t read_event(uint64_t pid, task_event *out, uint64_t count, bool block) {
        if (!out || count == 0) return 0;
        task *self = current_task();
        task *t = task_for_event_file(pid);
        if (!self || !t || self != t) return 0;

        for (;;) {
            uint64_t flags;
            spinlock_acquire(&process_lock, &flags);
            if (t->events.count != 0) {
                uint64_t n = count;
                if (n > t->events.count) n = t->events.count;
                for (uint64_t i = 0; i < n; i++) {
                    out[i] = t->events.entries[t->events.head];
                    t->events.head = (t->events.head + 1) % TASK_EVENT_QUEUE_SIZE;
                    t->events.count--;
                }
                spinlock_release(&process_lock, flags);
                return n;
            }
            if (!block) {
                spinlock_release(&process_lock, flags);
                return 0;
            }
            block_current(task_state::WAITING, &process_lock, flags);
        }
    }

    uint64_t send_event(uint64_t pid, const task_event *events, uint64_t count) {
        if (!events || count == 0) return 0;
        task *sender = current_task();
        task *target = task_for_event_file(pid);
        if (!sender || !target || target->type != task_type::USER) return 0;

        uint64_t accepted = 0;
        bool need_wake = false;
        uint64_t flags;
        spinlock_acquire(&process_lock, &flags);
        for (uint64_t i = 0; i < count; i++) {
            task_event ev = events[i];
            if (!valid_user_event_type(ev.type)) break;
            if (ev.data_len > TASK_EVENT_DATA_SIZE) break;
            ev.source = sender->id;
            ev.target = target->id;
            ev.flags &= TASK_EVENT_F_TRUNCATED;
            if (ev.type == TASK_EVENT_FORCE_KILL) {
                if (sender != target && target->parent != sender) break;
                target->kill_pending = true;
                target->kill_code = ev.code ? (int)ev.code : fatal_code_for(TASK_EVENT_FORCE_KILL);
                target->kill_source = sender->id;
                need_wake = true;
                accepted++;
                continue;
            }
            bool queued = enqueue_event_locked(target, ev);
            if (!queued) break;
            need_wake = true;
            accepted++;
        }
        spinlock_release(&process_lock, flags);
        if (need_wake) wake_blocked_or_waiting(target);
        return accepted;
    }

    bool read_faultctl(uint64_t pid, task_faultctl *out) {
        task *self = current_task();
        task *t = task_for_event_file(pid);
        if (!self || !t || self != t || !out) return false;
        uint64_t flags;
        spinlock_acquire(&process_lock, &flags);
        *out = t->fault.ctl;
        spinlock_release(&process_lock, flags);
        return true;
    }

    bool write_faultctl(uint64_t pid, const task_faultctl *ctl) {
        task *self = current_task();
        task *t = task_for_event_file(pid);
        if (!self || !t || self != t || !ctl) return false;
        if (ctl->version != 1) return false;
        if ((ctl->flags & ~(TASK_FAULTCTL_F_ENABLED | TASK_FAULTCTL_F_ONESHOT)) != 0) return false;
        if ((ctl->flags & TASK_FAULTCTL_F_ENABLED) && ctl->handler_ip == 0) return false;
        if (ctl->max_depth > 8) return false;
        if ((ctl->flags & TASK_FAULTCTL_F_ENABLED) &&
            !vmm::user_range_mapped(ctl->handler_ip, 1, false, t->cr3)) return false;
        if (ctl->handler_sp &&
            (ctl->handler_sp < 16 || !vmm::user_range_mapped(ctl->handler_sp - 16, 16, true, t->cr3))) {
            return false;
        }

        task_faultctl next = *ctl;
        next.event_mask &= (TASK_EVENT_MASK(TASK_EVENT_PAGE_FAULT) |
                            TASK_EVENT_MASK(TASK_EVENT_GENERAL_FAULT) |
                            TASK_EVENT_MASK(TASK_EVENT_DIVIDE_BY_ZERO) |
                            TASK_EVENT_MASK(TASK_EVENT_INVALID_OPCODE) |
                            TASK_EVENT_MASK(TASK_EVENT_BREAKPOINT) |
                            TASK_EVENT_MASK(TASK_EVENT_OVERFLOW) |
                            TASK_EVENT_MASK(TASK_EVENT_BOUNDS) |
                            TASK_EVENT_MASK(TASK_EVENT_STACK_FAULT) |
                            TASK_EVENT_MASK(TASK_EVENT_FPU_FAULT) |
                            TASK_EVENT_MASK(TASK_EVENT_ALIGNMENT_FAULT) |
                            TASK_EVENT_MASK(TASK_EVENT_USER_FAULT));
        uint64_t flags;
        spinlock_acquire(&process_lock, &flags);
        if (!t->fault.active) t->fault.ctl = next;
        bool ok = !t->fault.active;
        spinlock_release(&process_lock, flags);
        return ok;
    }

    uint64_t read_fault(uint64_t pid, task_fault_frame *out, uint64_t count) {
        task *self = current_task();
        task *t = task_for_event_file(pid);
        if (!self || !t || self != t || !out || count == 0) return 0;
        uint64_t flags;
        spinlock_acquire(&process_lock, &flags);
        if (!t->fault.active) {
            spinlock_release(&process_lock, flags);
            return 0;
        }
        *out = t->fault.frame;
        spinlock_release(&process_lock, flags);
        return 1;
    }

    bool write_fault_return(uint64_t pid, const task_fault_return *result) {
        task *self = current_task();
        task *t = task_for_event_file(pid);
        if (!self || !t || self != t || !result) return false;
        if (result->action > TASK_FAULT_RETURN_EXIT) return false;

        uint64_t flags;
        spinlock_acquire(&process_lock, &flags);
        bool ok = t->fault.active && !t->fault.return_pending;
        if (ok) {
            t->fault.result = *result;
            t->fault.return_pending = true;
        }
        spinlock_release(&process_lock, flags);
        return ok;
    }

    bool handle_user_exception(regs *r, uint32_t type, uint64_t detail) {
        if (!r || !fault_event_type(type)) return false;
        task *current = current_task();
        if (!current || current->type != task_type::USER) return false;

        uint64_t flags;
        spinlock_acquire(&process_lock, &flags);

        bool enabled = (current->fault.ctl.flags & TASK_FAULTCTL_F_ENABLED) != 0;
        bool wanted = (current->fault.ctl.event_mask & TASK_EVENT_MASK(type)) != 0;
        uint32_t max_depth = current->fault.ctl.max_depth ? current->fault.ctl.max_depth : 1;
        if (enabled && wanted && !current->fault.active && current->fault.depth < max_depth) {
            current->fault.active = true;
            current->fault.return_pending = false;
            current->fault.depth++;
            current->fault.saved_regs = *r;
            memset(&current->fault.frame, 0, sizeof(current->fault.frame));
            current->fault.frame.event.type = type;
            current->fault.frame.event.flags = TASK_EVENT_F_SYNCHRONOUS | TASK_EVENT_F_RECOVERABLE;
            current->fault.frame.event.source = current->id;
            current->fault.frame.event.target = current->id;
            current->fault.frame.event.code = fatal_code_for(type);
            current->fault.frame.event.detail = detail;
            regs_to_fault_regs(r, &current->fault.frame.regs);
            current->fault.frame.fault_address = type == TASK_EVENT_PAGE_FAULT ? detail : 0;
            current->fault.frame.cpu_error = r->err_code;
            current->fault.frame.arch_vector = r->int_no;
            current->fault.frame.depth = current->fault.depth;
            r->rip = current->fault.ctl.handler_ip;
            if (current->fault.ctl.handler_sp) r->rsp = current->fault.ctl.handler_sp;
            r->rdi = type;
            r->rsi = detail;
            r->rdx = r->err_code;
            if (current->fault.ctl.flags & TASK_FAULTCTL_F_ONESHOT) current->fault.ctl.flags &= ~TASK_FAULTCTL_F_ENABLED;
            spinlock_release(&process_lock, flags);
            return true;
        }

        int code = fatal_code_for(type);
        task *parent = current->parent;
        terminate_current_locked(current, code, TASK_EVENT_FAULTED, detail);
        for (uint64_t i = 0; i < current->fd_capacity; i++) { fd_manager::close_fd(i, current); }
        spinlock_release(&process_lock, flags);
        wake_blocked_or_waiting(parent);
        yield();
        kpanic("faulted zombie task resumed");
    }

    bool complete_fault_return_if_pending(regs *r) {
        if (!r) return false;
        task *current = current_task();
        if (!current || current->type != task_type::USER) return false;

        uint64_t flags;
        spinlock_acquire(&process_lock, &flags);
        if (!current->fault.active || !current->fault.return_pending) {
            spinlock_release(&process_lock, flags);
            return false;
        }

        task_fault_return result = current->fault.result;
        uint32_t cause = current->fault.frame.event.type;
        uint64_t detail = current->fault.frame.event.detail;
        regs saved = current->fault.saved_regs;
        current->fault.active = false;
        current->fault.return_pending = false;
        if (current->fault.depth) current->fault.depth--;
        spinlock_release(&process_lock, flags);

        if (result.action == TASK_FAULT_RETURN_RESUME) {
            *r = saved;
            return true;
        }
        if (result.action == TASK_FAULT_RETURN_RESUME_AT) {
            if (result.resume_ip) saved.rip = result.resume_ip;
            if (result.resume_sp) saved.rsp = result.resume_sp;
            *r = saved;
            return true;
        }

        int code = result.action == TASK_FAULT_RETURN_EXIT && result.code ? (int)result.code : fatal_code_for(cause);
        spinlock_acquire(&process_lock, &flags);
        task *parent = current->parent;
        terminate_current_locked(current, code, TASK_EVENT_FAULTED, detail);
        for (uint64_t i = 0; i < current->fd_capacity; i++) { fd_manager::close_fd(i, current); }
        spinlock_release(&process_lock, flags);
        wake_blocked_or_waiting(parent);
        yield();
        kpanic("fault-return zombie task resumed");
    }

    bool apply_pending_kill(regs *) {
        task *current = current_task();
        if (!current || current->type != task_type::USER || !current->kill_pending) return false;
        int code = current->kill_code ? current->kill_code : fatal_code_for(TASK_EVENT_FORCE_KILL);
        uint64_t flags;
        spinlock_acquire(&process_lock, &flags);
        current->kill_pending = false;
        uint64_t source = current->kill_source;
        current->kill_source = 0;
        task *parent = current->parent;
        terminate_current_locked(current, code, TASK_EVENT_KILLED, source);
        for (uint64_t i = 0; i < current->fd_capacity; i++) { fd_manager::close_fd(i, current); }
        spinlock_release(&process_lock, flags);
        wake_blocked_or_waiting(parent);
        yield();
        kpanic("killed zombie task resumed");
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

    bool block_current(task_state state, spinlock_t *release_lock, uint64_t release_flags) {
        if (state == task_state::READY || state == task_state::RUNNING || state == task_state::ZOMBIE) return false;

        smp::cpu_local *cpu = smp::get_cpu();
        if (!cpu || !cpu->current_task || cpu->current_task == cpu->idle_task) return false;

        uint64_t flags;
        cpu->sched_lock.acquire(flags);
        cpu->current_task->state = state;
        sched_unlock_keep_interrupts_disabled(cpu);

        if (release_lock) {
            spinlock_release(release_lock, release_flags);
        }

        yield();

        if (!release_lock) restore_flags(flags);
        return true;
    }

    bool wake(task *t) {
        if (!t || t->state != task_state::WAITING) return false;

        smp::cpu_local *cpu = smp::get_cpu_by_index(t->last_cpu_index);
        if (!cpu) return false;

        uint64_t flags;
        cpu->sched_lock.acquire(flags);
        if (t->state == task_state::WAITING) {
            t->state = task_state::READY;
            enqueue_ready_on_best_cpu(cpu, t);
            cpu->sched_lock.release(flags);
            return true;
        }
        cpu->sched_lock.release(flags);
        return false;
    }

    bool unfreeze(task *t) {
        if (!t) return false;

        smp::cpu_local *cpu = smp::get_cpu_by_index(t->last_cpu_index);
        if (!cpu) return false;

        uint64_t flags;
        cpu->sched_lock.acquire(flags);
        if (t->state == task_state::FROZEN) {
            t->state = task_state::READY;
            enqueue_ready_on_best_cpu(cpu, t);
            cpu->sched_lock.release(flags);
            return true;
        }
        cpu->sched_lock.release(flags);
        return false;
    }

    void sleep(uint64_t ticks) {
        smp::cpu_local *cpu = smp::get_cpu();
        uint64_t flags;
        cpu->sched_lock.acquire(flags);

        task *current = cpu->current_task;
        current->state = task_state::SLEEPING;
        current->wakeup_time = cpu->ticks + ticks;
        current->next = cpu->sleep_list_head;
        current->prev = nullptr;
        cpu->sleep_list_head = current;

        sched_unlock_keep_interrupts_disabled(cpu);
        yield();
        restore_flags(flags);
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

        uint64_t process_flags;
        spinlock_acquire(&process_lock, &process_flags);

        terminate_current_locked(current, code, TASK_EVENT_EXIT, 0);

        if (current->parent && (current->parent->state == task_state::BLOCKED ||
                                current->parent->state == task_state::WAITING)) {
            current->parent->state = task_state::READY;
            enqueue_ready_on_best_cpu(cpu, current->parent);
        }

        devfs_release_task_resources(current->id);
        for (uint64_t i = 0; i < current->fd_capacity; i++) { fd_manager::close_fd(i, current); }

        spinlock_release(&process_lock, process_flags);
        sched_unlock_keep_interrupts_disabled(cpu);

        yield();
        kpanic("zombie task resumed after exit");
    }

    int wait(int *status) {
        smp::cpu_local *cpu = smp::get_cpu();
        task *current = cpu->current_task;

        while (true) {
            uint64_t flags;
            cpu->sched_lock.acquire(flags);

            uint64_t process_flags;
            spinlock_acquire(&process_lock, &process_flags);

            task *prev_sibling = nullptr;
            task *child = current->first_child;

            if (!child) {
                spinlock_release(&process_lock, process_flags);
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

                    spinlock_release(&process_lock, process_flags);
                    cpu->sched_lock.release(flags);
                    free_task_resources(child);
                    return child_id;
                }
                prev_sibling = child;
                child = child->next_sibling;
            }

            current->state = task_state::BLOCKED;
            spinlock_release(&process_lock, process_flags);
            sched_unlock_keep_interrupts_disabled(cpu);

            yield();
            restore_flags(flags);
        }
    }

}  // namespace scheduler