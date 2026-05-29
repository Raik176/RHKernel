#include "util.h"

#include "console.h"
#include "file/module_loader.h"
#include "memory/vmm.h"
#include "smp/apic.h"
#include "smp/scheduler.h"
#include "smp/smp.h"
#include "string.h"
#include "symbol/ksym.h"

extern "C" {
extern uint8_t higher_stack_top[];
extern uint8_t higher_stack_bottom[];
}

namespace {
    static uint64_t cached_paging_phys_map_base = PHYS_MAP_BASE_4L;
    static uint64_t cached_paging_phys_direct_map_size = PHYS_DIRECT_MAP_SIZE_4L;
    static uint64_t cached_paging_mode_la57 = 0;
    static uint64_t cached_paging_user_top = 0x0000800000000000ULL;
    static bool paging_values_cached = false;
    static volatile uint32_t panic_started = 0;
    static volatile uint32_t panic_owner = UINT32_MAX;
    static volatile uint32_t panic_depth = 0;

    static inline uint64_t read_early_phys_map_base() {
        uint64_t *ptr;
        asm volatile("movabsq $paging_phys_map_base, %0" : "=r"(ptr));
        return *ptr;
    }

    static inline uint64_t read_early_phys_direct_map_size() {
        uint64_t *ptr;
        asm volatile("movabsq $paging_phys_direct_map_size, %0" : "=r"(ptr));
        return *ptr;
    }

    static inline uint64_t read_early_mode_la57() {
        uint64_t *ptr;
        asm volatile("movabsq $paging_mode_la57, %0" : "=r"(ptr));
        return *ptr;
    }

    static inline uint64_t read_early_user_top() {
        uint64_t *ptr;
        asm volatile("movabsq $paging_user_top, %0" : "=r"(ptr));
        return *ptr;
    }
}

static inline void cpu_halt_loop() {
    asm volatile("cli" ::: "memory");
    for (;;) { asm volatile("hlt" ::: "memory"); }
}

void __attribute__((noreturn)) panic_halt_forever() {
    cpu_halt_loop();
    __builtin_unreachable();
}

static smp::cpu_local *panic_cpu_local() {
    uint32_t lo = 0, hi = 0;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(apic::MSR_GS_BASE));
    uint64_t gs_base = ((uint64_t)hi << 32) | lo;
    if (!gs_base) return nullptr;

    smp::cpu_local *cpu = (smp::cpu_local *)gs_base;
    if (cpu->self != cpu) return nullptr;
    return cpu;
}

static uint32_t panic_cpu_id() {
    smp::cpu_local *cpu = panic_cpu_local();
    return cpu ? cpu->cpu_index : UINT32_MAX - 1;
}

extern "C" void paging_runtime_cache_values() {
    cached_paging_phys_map_base = read_early_phys_map_base();
    cached_paging_phys_direct_map_size = read_early_phys_direct_map_size();
    cached_paging_mode_la57 = read_early_mode_la57();
    cached_paging_user_top = read_early_user_top();
    paging_values_cached = true;
}

extern "C" uint64_t paging_phys_map_base_value() {
    return paging_values_cached ? cached_paging_phys_map_base : read_early_phys_map_base();
}

extern "C" uint64_t paging_phys_direct_map_size_value() {
    return paging_values_cached ? cached_paging_phys_direct_map_size : read_early_phys_direct_map_size();
}

extern "C" uint64_t paging_mode_la57_value() {
    return paging_values_cached ? cached_paging_mode_la57 : read_early_mode_la57();
}

extern "C" uint64_t paging_user_top_value() {
    return paging_values_cached ? cached_paging_user_top : read_early_user_top();
}

struct panic_origin {
    const char *kind;
    const char *name;
    bool executable;
};

static panic_origin classify_code_address(uint64_t address) {
    const module_loader::LoadedModule *m = module_loader::find_module_containing(address);
    if (m) return {"module", m->name, module_loader::address_in_module_text(m, address)};
    if (module_loader::address_in_kernel(address)) {
        return {"kernel", "kernel", module_loader::address_in_kernel_text(address)};
    }
    return {"unknown", "unknown", false};
}


void busy_sleep(uint64_t ms) {
    uint32_t scale = apic::get_tick_scale();
    if (scale == 0) scale = 1;

    uint64_t start = apic::get_ticks();
    uint64_t ticks_to_wait = ms / scale;
    if (ms != 0 && ticks_to_wait == 0) ticks_to_wait = 1;
    while ((apic::get_ticks() - start) < ticks_to_wait) { asm volatile("pause"); }
}

static inline uint64_t read_cr0() {
    uint64_t value;
    asm volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

static inline uint64_t read_cr2() {
    uint64_t value;
    asm volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

static inline uint64_t read_cr3() {
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline uint64_t read_cr4() {
    uint64_t value;
    asm volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void fill_regs(struct regs *r) {
    memset(r, 0, sizeof(*r));

    asm volatile("mov %%rax, %0" : "=m"(r->rax));
    asm volatile("mov %%rbx, %0" : "=m"(r->rbx));
    asm volatile("mov %%rcx, %0" : "=m"(r->rcx));
    asm volatile("mov %%rdx, %0" : "=m"(r->rdx));
    asm volatile("mov %%rsi, %0" : "=m"(r->rsi));
    asm volatile("mov %%rdi, %0" : "=m"(r->rdi));
    asm volatile("mov %%r8,  %0" : "=m"(r->r8));
    asm volatile("mov %%r9,  %0" : "=m"(r->r9));
    asm volatile("mov %%r10, %0" : "=m"(r->r10));
    asm volatile("mov %%r11, %0" : "=m"(r->r11));
    asm volatile("mov %%r12, %0" : "=m"(r->r12));
    asm volatile("mov %%r13, %0" : "=m"(r->r13));
    asm volatile("mov %%r14, %0" : "=m"(r->r14));
    asm volatile("mov %%r15, %0" : "=m"(r->r15));

    asm volatile("lea 0(%%rip), %0" : "=r"(r->rip));
    asm volatile("pushfq; popq %0" : "=m"(r->rflags));

    asm volatile("mov %%cs, %0" : "=m"(r->cs));
    asm volatile("mov %%ss, %0" : "=m"(r->ss));

    asm volatile("mov %%rbp, %0" : "=r"(r->rbp));
    asm volatile("mov %%rsp, %0" : "=r"(r->rsp));
}

const char *symbolicate(uint64_t address, uintptr_t *offset) {
    uintptr_t local_offset = 0;
    const char *symbol = ksym::get_name(address, &local_offset);
    if (offset) { *offset = local_offset; }
    return symbol;
}

static void print_symbol_line(const char *label, uint64_t address) {
    uintptr_t offset = 0;
    const char *symbol = symbolicate(address, &offset);
    console::printf("  %s: %p <%s+%p>\n", label, address, symbol ? symbol : "unknown", offset);
}

static bool valid_kernel_stack_frame(uint64_t frame) {
    if ((frame & 0x7) != 0) { return false; }

    if (frame >= (uint64_t)higher_stack_bottom && frame < (uint64_t)higher_stack_top) {
        return true;
    }

    smp::cpu_local *cpu = panic_cpu_local();
    if (cpu && cpu->current_task && cpu->current_task->kernel_stack) {
        uint64_t bottom = (uint64_t)cpu->current_task->kernel_stack;
        uint64_t top = bottom + scheduler::KERNEL_STACK_SIZE;
        return frame >= bottom && frame < top;
    }

    return false;
}

void print_stacktrace_from(uint64_t rbp) {
    struct stack_frame {
        struct stack_frame *next;
        uint64_t return_address;
    };

    stack_frame *frame = (stack_frame *)rbp;
    for (uint64_t i = 0; i < 32 && frame; ++i) {
        if (!valid_kernel_stack_frame((uint64_t)frame)) {
            console::printf("  ... stopped at invalid frame %p\n", frame);
            break;
        }

        uintptr_t offset = 0;
        const char *symbol = symbolicate(frame->return_address, &offset);
        console::printf("  #%02d rbp=%p rip=%p <%s+%p>\n", i, frame, frame->return_address,
                        symbol ? symbol : "unknown", offset);

        if ((uint64_t)frame->next <= (uint64_t)frame) {
            console::printf("  ... stopped at non-increasing frame %p\n", frame->next);
            break;
        }
        frame = frame->next;
    }
}

void print_stacktrace() {
    uint64_t rbp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    print_stacktrace_from(rbp);
}

void dump_regs(struct regs *r) {
    if (!r) {
        console::printf("  <no register frame>\n");
        return;
    }

    console::set_color(console::Color::LightCyan);
    console::printf("RAX: %p  RBX: %p  RCX: %p  RDX: %p\n", r->rax, r->rbx, r->rcx, r->rdx);
    console::printf("RSI: %p  RDI: %p  RBP: %p  RSP: %p\n", r->rsi, r->rdi, r->rbp, r->rsp);
    console::printf("R8 : %p  R9 : %p  R10: %p  R11: %p\n", r->r8, r->r9, r->r10, r->r11);
    console::printf("R12: %p  R13: %p  R14: %p  R15: %p\n", r->r12, r->r13, r->r14, r->r15);
    console::printf("RIP: %p  CS : %p  SS : %p  FLG: %p\n", r->rip, r->cs, r->ss, r->rflags);
    console::printf("INT: %p  ERR: %p\n", r->int_no, r->err_code);
    console::set_color(console::Color::White);
}

void dump_control_regs() {
    console::printf("  CR0: %p  CR2: %p  CR3: %p  CR4: %p\n", read_cr0(), read_cr2(), read_cr3(),
                    read_cr4());
}

void dump_page_fault_error(uint64_t error_code) {
    console::printf("  Page fault bits: %s, %s, %s, %s, %s\n",
                    (error_code & (1ULL << 0)) ? "protection" : "not-present",
                    (error_code & (1ULL << 1)) ? "write" : "read",
                    (error_code & (1ULL << 2)) ? "user" : "supervisor",
                    (error_code & (1ULL << 3)) ? "reserved-bit" : "no-reserved-bit",
                    (error_code & (1ULL << 4)) ? "instruction-fetch" : "data-access");
}

static void dump_kernel_stack_fault(uint64_t fault_addr, const regs *r) {
    smp::cpu_local *cpu = panic_cpu_local();
    if (!cpu || !cpu->current_task || !cpu->current_task->kernel_stack) return;

    uint64_t base = (uint64_t)cpu->current_task->kernel_stack;
    uint64_t low_guard = 0;
    uint64_t top = 0;
    if (!vmm::kernel_stack_range(cpu->current_task->kernel_stack, scheduler::KERNEL_STACK_SIZE,
                                 &low_guard, &top)) {
        return;
    }

    bool low_guard_hit = fault_addr >= low_guard && fault_addr < base;
    bool high_guard_hit = fault_addr >= top && fault_addr < top + 4096;
    bool rsp_low = r && r->rsp < base;
    bool rsp_high = r && r->rsp > top;
    if (!low_guard_hit && !high_guard_hit && !rsp_low && !rsp_high) return;

    const char *kind = low_guard_hit ? "low guard" :
                       high_guard_hit ? "high guard" :
                       rsp_low ? "below stack" : "above stack";
    uint64_t used = 0;
    if (r && r->rsp >= base && r->rsp <= top) used = top - r->rsp;

    console::printf("\n--- KERNEL STACK OVERFLOW ---\n");
    console::printf("  Classification: %s hit for current task kernel stack\n", kind);
    console::printf("  Stack usable : [%p..%p) size=%d bytes\n", base, top, scheduler::KERNEL_STACK_SIZE);
    console::printf("  Guard pages  : low=[%p..%p) high=[%p..%p)\n", low_guard, base, top, top + 4096);
    if (r) {
        console::printf("  RSP          : %p", r->rsp);
        if (r->rsp >= base && r->rsp <= top) {
            console::printf(" used=%d bytes free=%d bytes", used, r->rsp - base);
        }
        console::printf("\n");
    }
    console::printf("  Fault address: %p\n", fault_addr);
}

void hexdump(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < len; i += 16) {
        console::printf("  %p: ", bytes + i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < len)
                console::printf("%02x ", (uint64_t)bytes[i + j]);
            else
                console::printf("   ");
        }
        console::printf(" | ");
        for (size_t j = 0; j < 16 && i + j < len; ++j) {
            char c = (char)bytes[i + j];
            console::putchar((c >= 32 && c <= 126) ? c : '.');
        }
        console::printf("\n");
    }
}

void dump_memory(const void *data, size_t len) { hexdump(data, len); }


static void print_address_origin(const char *label, uint64_t address) {
    panic_origin o = classify_code_address(address);
    uintptr_t offset = 0;
    const char *symbol = symbolicate(address, &offset);
    console::printf("  %s: %p %s:%s exec=%s <%s+%p>\n", label, address, o.kind, o.name,
                    o.executable ? "yes" : "no", symbol ? symbol : "unknown", offset);
}

static void dump_panic_origin(struct regs *r) {
    panic_origin rip = classify_code_address(r ? r->rip : 0);
    console::printf("  primary: %s:%s exec=%s\n", rip.kind, rip.name, rip.executable ? "yes" : "no");
    if (r) {
        print_address_origin("rip", r->rip);
        if (r->int_no == 14) print_address_origin("cr2", read_cr2());
    }

    const char *seen[16];
    uint64_t seen_count = 0;
    bool kernel_seen = false;
    uint64_t rbp = r ? r->rbp : 0;
    struct stack_frame {
        struct stack_frame *next;
        uint64_t return_address;
    };

    for (stack_frame *frame = (stack_frame *)rbp; frame && seen_count < 16;) {
        if (!valid_kernel_stack_frame((uint64_t)frame)) break;
        panic_origin o = classify_code_address(frame->return_address);
        if (strcmp(o.kind, "kernel") == 0) {
            kernel_seen = true;
        } else if (strcmp(o.kind, "module") == 0) {
            bool exists = false;
            for (uint64_t i = 0; i < seen_count; i++) {
                if (strcmp(seen[i], o.name) == 0) { exists = true; break; }
            }
            if (!exists) seen[seen_count++] = o.name;
        }
        if ((uint64_t)frame->next <= (uint64_t)frame) break;
        frame = frame->next;
    }

    console::printf("  stack: kernel=%s modules=", kernel_seen ? "yes" : "no");
    if (seen_count == 0) {
        console::printf("none\n");
        return;
    }
    for (uint64_t i = 0; i < seen_count; i++) {
        console::printf("%s%s", i ? "," : "", seen[i]);
    }
    console::printf("\n");
}

static void dump_current_task_summary() {
    smp::cpu_local *cpu = panic_cpu_local();
    if (!cpu) {
        console::printf("  CPU   : <not initialized>\n");
        return;
    }

    console::printf("  CPU   : #%d (LAPIC %d) ticks=%d\n", (uint64_t)cpu->cpu_index, (uint64_t)cpu->lapic_id, cpu->ticks);
    if (cpu->current_task) { scheduler::dump_task(cpu->current_task); }
}

void __attribute__((noreturn)) kpanic(const char *message, struct regs *r) {
    kpanic_at(message, nullptr, 0, nullptr, r);
}

void __attribute__((noreturn)) kfatal_at(const char *message, const char *file, int line,
                                          const char *function) {
    kpanic_at(message, file, line, function, nullptr);
}

void __attribute__((noreturn)) kpanic_at(const char *message, const char *file, int line,
                                         const char *function, struct regs *r) {
    asm volatile("cli" ::: "memory");

    uint32_t cpu_id = panic_cpu_id();
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&panic_started, &expected, 1, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&panic_owner, cpu_id, __ATOMIC_RELEASE);
        __atomic_store_n(&panic_depth, 1, __ATOMIC_RELEASE);
        if (panic_cpu_local()) smp::panic_stop_others();
        console::panic_unlock_output();
    } else {
        uint32_t owner = __atomic_load_n(&panic_owner, __ATOMIC_ACQUIRE);
        if (owner != cpu_id) panic_halt_forever();
        uint32_t depth = __atomic_add_fetch(&panic_depth, 1, __ATOMIC_ACQ_REL);
        if (depth > 1) {
            console::panic_unlock_output();
            console::printf("\nrecursive panic: %s\n", message ? message : "<null>");
            panic_halt_forever();
        }
    }

    regs captured_regs;
    if (r == nullptr) {
        fill_regs(&captured_regs);
        r = &captured_regs;
    }

    console::set_color(console::Color::Red);
    console::printf(
        "\n================================================================================\n");
    console::printf(
        "                                KERNEL PANIC                                    \n");
    console::printf(
        "================================================================================\n");
    console::set_color(console::Color::White);

    if (r->int_no <= 31 && r->int_no != 0) {
        console::printf("  REASON: %s (vector=%d error=%p)\n", message, r->int_no, r->err_code);
    } else {
        console::printf("  REASON: %s\n", message ? message : "<null>");
    }

    if (file) { console::printf("  WHERE : %s:%d in %s()\n", file, (uint64_t)line, function ? function : "?"); }

    dump_current_task_summary();

    console::printf("\n--- IMAGE SIZES ---\n");
    console::printf("  kernel_image_bytes: %d\n", module_loader::kernel_image_size());
    console::printf("  modules_mapped_bytes: %d\n", module_loader::module_total_mapped_size());
    for (const module_loader::LoadedModule *m = module_loader::first_module(); m; m = m->next) {
        console::printf("  module %s: base=%p end=%p mapped=%d image=%d symbols=%d\n",
                        m->name, m->base, m->end, m->mapped_size, m->image_size, m->symbol_count);
    }

    console::printf("\n--- PANIC ORIGIN ---\n");
    dump_panic_origin(r);

    console::printf("\n--- FAULT LOCATION ---\n");
    print_symbol_line("RIP", r->rip);
    print_symbol_line("RBP", r->rbp);

    console::printf("\n--- REGISTER STATE ---\n");
    dump_regs(r);

    console::printf("\n--- CONTROL REGISTERS ---\n");
    dump_control_regs();

    if (r->int_no == 14) {
        uint64_t fault_addr = read_cr2();
        console::printf("\n--- PAGE FAULT ---\n");
        console::printf("  Faulting virtual address: %p\n", fault_addr);
        dump_page_fault_error(r->err_code);
        dump_kernel_stack_fault(fault_addr, r);
    }

    console::printf("\n--- STACKTRACE ---\n");
    if (r->rbp) {
        print_stacktrace_from(r->rbp);
    } else {
        print_stacktrace();
    }

    console::printf(
        "\n================================================================================\n");
    console::printf("  SYSTEM HALTED.\n");

    panic_halt_forever();
}
