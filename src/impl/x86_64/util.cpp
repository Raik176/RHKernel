#include "util.h"

#include "console.h"
#include "file/module_loader.h"
#include "smp/apic.h"
#include "smp/scheduler.h"
#include "smp/smp.h"
#include "string.h"
#include "symbol/ksym.h"

extern "C" {
extern uint8_t higher_stack_top[];
extern uint8_t higher_stack_bottom[];
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

    smp::cpu_local *cpu = smp::get_cpu();
    if (cpu && cpu->self == cpu && cpu->current_task && cpu->current_task->kernel_stack) {
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
    smp::cpu_local *cpu = smp::get_cpu();
    if (!cpu || cpu->self != cpu) {
        console::printf("  CPU   : <not initialized>\n");
        return;
    }

    console::printf("  CPU   : #%d (LAPIC %d) ticks=%d\n", (uint64_t)cpu->cpu_index, (uint64_t)cpu->lapic_id, cpu->ticks);
    if (cpu->current_task) { scheduler::dump_task(cpu->current_task); }
}

void __attribute__((noreturn)) kpanic(const char *message, struct regs *r) {
    kpanic_at(message, nullptr, 0, nullptr, r);
}

void __attribute__((noreturn)) kpanic_at(const char *message, const char *file, int line,
                                         const char *function, struct regs *r) {
    asm volatile("cli");

    regs captured_regs;
    if (r == nullptr) {
        fill_regs(&captured_regs);
        r = &captured_regs;
    }

    smp::send_halt_mail(-1);
    smp::flush_mail(-1);

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
        console::printf("\n--- PAGE FAULT ---\n");
        console::printf("  Faulting virtual address: %p\n", read_cr2());
        dump_page_fault_error(r->err_code);
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

    for (;;) asm volatile("hlt");
}
