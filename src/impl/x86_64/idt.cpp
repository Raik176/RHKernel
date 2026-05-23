/**
 * @file idt.cpp
 * @brief Implementation of the Interrupt Descriptor Table (IDT)
 *
 * Sets up the IDT, default exception handlers, and provides
 * infrastructure for handling CPU interrupts and exceptions.
 */

#include "idt.h"

#include "console.h"
#include "memory/vmm.h"
#include "mod/interrupt.h"
#include "smp/apic.h"
#include "smp/scheduler.h"
#include "smp/smp.h"
#include "string.h"

extern "C" {

extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();

extern void isr32();
extern void isr33();
extern void isr34();
extern void isr35();
extern void isr36();
extern void isr37();
extern void isr38();
extern void isr39();
extern void isr40();
extern void isr41();
extern void isr42();
extern void isr43();
extern void isr44();
extern void isr45();
extern void isr46();
extern void isr47();

extern void isr254();

extern "C" void dispatch_irq(struct regs *r);

void handle_mailbox_ipi(struct regs *r) {
    (void)r;

    auto *cpu = smp::get_cpu();

    for (;;) {
        uint64_t flags;
        cpu->mail_lock.acquire(flags);

        if (cpu->mail_head == cpu->mail_tail) {
            cpu->mail_lock.release(flags);
            break;
        }

        smp::mail *msg = cpu->mailbox[cpu->mail_head];
        cpu->mailbox[cpu->mail_head] = nullptr;
        cpu->mail_head = (cpu->mail_head + 1) % smp::MAILBOX_SIZE;
        cpu->mail_lock.release(flags);

        switch (msg->type) {
            case smp::mail_type::HALT:
                for (;;) __asm__ volatile("cli; hlt");
                break;
            case smp::mail_type::TLB_SHOOTDOWN: {
                uint64_t cr3;
                asm volatile("mov %%cr3, %0" : "=r"(cr3));
                if (msg->tlb.cr3 == 0 || msg->tlb.cr3 == cr3) {
                    if (msg->tlb.pages == 0) {
                        asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
                    } else {
                        for (uint32_t i = 0; i < msg->tlb.pages; i++) {
                            uint64_t addr = msg->tlb.addr + (uint64_t)i * 4096ULL;
                            asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
                        }
                    }
                }
                break;
            }
            default:
                kpanic("Unhandled mail type!");
                break;
        }

        if (msg->handled) { *(msg->handled) = true; }
    }

    apic::eoi();
}

uint64_t idt_handler(struct regs *r) {
    if (r->int_no <= 31) {
        if (r->int_no == 14) {
            uint64_t faulting_address;
            asm volatile("mov %%cr2, %0" : "=r"(faulting_address));

            if (vmm::handle_fault(faulting_address, r->err_code, r)) { return (uint64_t)r; }
        }

        static const char exception_messages[32][30] = {"Division By Zero",
                                                        "Debug",
                                                        "Non Maskable Interrupt",
                                                        "Breakpoint",
                                                        "Into Detected Overflow",
                                                        "Out of Bounds",
                                                        "Invalid Opcode",
                                                        "No Coprocessor",
                                                        "Double Fault",
                                                        "Coprocessor Segment Overrun",
                                                        "Bad TSS",
                                                        "Segment not Present",
                                                        "Stack Fault",
                                                        "General Protection Fault",
                                                        "Page Fault",
                                                        "Unknown Interrupt",
                                                        "Coprocessor fault",
                                                        "Alignment Check",
                                                        "Machine Check",
                                                        "Reserved",
                                                        "Reserved",
                                                        "Reserved",
                                                        "Reserved",
                                                        "Reserved",
                                                        "Reserved",
                                                        "Reserved",
                                                        "Reserved",
                                                        "Reserved",
                                                        "Reserved",
                                                        "Reserved",
                                                        "Reserved"};

        KPANIC_REGS(exception_messages[r->int_no], r);
    }

    if (r->int_no >= 32 && r->int_no <= 255) {
        if (r->int_no == idt::MAILBOX_VECTOR) {
            handle_mailbox_ipi(r);
            return (uint64_t)r;
        }
        dispatch_irq(r);
        if (r->int_no == 32) {
            apic::tick();

            smp::cpu_local *cpu = smp::get_cpu();
            bool from_user = (r->cs & 3) == 3;
            bool from_idle = cpu && cpu->current_task == cpu->idle_task;
            if (from_user || from_idle) {
                scheduler::schedule(r, true);
            }
        }

        return (uint64_t)r;
    }

    return (uint64_t)r;
}
}

namespace idt {
    struct idt_entry idt[256];  ///< IDT table
    struct idt_ptr idtp;        ///< Pointer structure used by lidt

    /**
     * @internal Load the IDT into the CPU
     *
     * Wraps the `lidt` instruction.
     */
    static inline void load(void) { __asm__ volatile("lidt %0" : : "m"(idtp)); }

    /**
     * @brief Set an entry in the IDT
     *
     * Configures a single interrupt gate with the specified handler, selector, and flags.
     *
     * @param num Interrupt number (0-255)
     * @param base Address of the ISR handler
     * @param sel Code segment selector from GDT
     * @param flags Type and attribute flags for the entry
     */
    void set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
        idt[num] = {.offset_low = static_cast<uint16_t>(base & 0xFFFF),
                    .selector = sel,
                    .ist = 0,
                    .flags = flags,
                    .offset_mid = static_cast<uint16_t>((base >> 16) & 0xFFFF),
                    .offset_high = static_cast<uint32_t>(base >> 32),
                    .zero = 0};
    }

    /**
     * @brief Initialize the IDT
     *
     * Clears the IDT, sets up default exception handlers (ISRs 0-31),
     * and loads the IDT into the CPU.
     */
    void init(void) {
        idtp.limit = sizeof(idt) - 1;
        idtp.base = reinterpret_cast<uint64_t>(&idt);

        memset(&idt, 0, sizeof(idt));

        set_gate(0, reinterpret_cast<uint64_t>(isr0), 0x08, 0x8E);
        set_gate(1, reinterpret_cast<uint64_t>(isr1), 0x08, 0x8E);
        set_gate(2, reinterpret_cast<uint64_t>(isr2), 0x08, 0x8E);
        set_gate(3, reinterpret_cast<uint64_t>(isr3), 0x08, 0x8E);
        set_gate(4, reinterpret_cast<uint64_t>(isr4), 0x08, 0x8E);
        set_gate(5, reinterpret_cast<uint64_t>(isr5), 0x08, 0x8E);
        set_gate(6, reinterpret_cast<uint64_t>(isr6), 0x08, 0x8E);
        set_gate(7, reinterpret_cast<uint64_t>(isr7), 0x08, 0x8E);
        set_gate(8, reinterpret_cast<uint64_t>(isr8), 0x08, 0x8E);
        set_gate(9, reinterpret_cast<uint64_t>(isr9), 0x08, 0x8E);
        set_gate(10, reinterpret_cast<uint64_t>(isr10), 0x08, 0x8E);
        set_gate(11, reinterpret_cast<uint64_t>(isr11), 0x08, 0x8E);
        set_gate(12, reinterpret_cast<uint64_t>(isr12), 0x08, 0x8E);
        set_gate(13, reinterpret_cast<uint64_t>(isr13), 0x08, 0x8E);
        set_gate(14, reinterpret_cast<uint64_t>(isr14), 0x08, 0x8E);
        set_gate(15, reinterpret_cast<uint64_t>(isr15), 0x08, 0x8E);
        set_gate(16, reinterpret_cast<uint64_t>(isr16), 0x08, 0x8E);
        set_gate(17, reinterpret_cast<uint64_t>(isr17), 0x08, 0x8E);
        set_gate(18, reinterpret_cast<uint64_t>(isr18), 0x08, 0x8E);
        set_gate(19, reinterpret_cast<uint64_t>(isr19), 0x08, 0x8E);
        set_gate(20, reinterpret_cast<uint64_t>(isr20), 0x08, 0x8E);
        set_gate(21, reinterpret_cast<uint64_t>(isr21), 0x08, 0x8E);
        set_gate(22, reinterpret_cast<uint64_t>(isr22), 0x08, 0x8E);
        set_gate(23, reinterpret_cast<uint64_t>(isr23), 0x08, 0x8E);
        set_gate(24, reinterpret_cast<uint64_t>(isr24), 0x08, 0x8E);
        set_gate(25, reinterpret_cast<uint64_t>(isr25), 0x08, 0x8E);
        set_gate(26, reinterpret_cast<uint64_t>(isr26), 0x08, 0x8E);
        set_gate(27, reinterpret_cast<uint64_t>(isr27), 0x08, 0x8E);
        set_gate(28, reinterpret_cast<uint64_t>(isr28), 0x08, 0x8E);
        set_gate(29, reinterpret_cast<uint64_t>(isr29), 0x08, 0x8E);
        set_gate(30, reinterpret_cast<uint64_t>(isr30), 0x08, 0x8E);
        set_gate(31, reinterpret_cast<uint64_t>(isr31), 0x08, 0x8E);

        set_gate(32, reinterpret_cast<uint64_t>(isr32), 0x08, 0x8E);
        set_gate(33, reinterpret_cast<uint64_t>(isr33), 0x08, 0x8E);
        set_gate(34, reinterpret_cast<uint64_t>(isr34), 0x08, 0x8E);
        set_gate(35, reinterpret_cast<uint64_t>(isr35), 0x08, 0x8E);
        set_gate(36, reinterpret_cast<uint64_t>(isr36), 0x08, 0x8E);
        set_gate(37, reinterpret_cast<uint64_t>(isr37), 0x08, 0x8E);
        set_gate(38, reinterpret_cast<uint64_t>(isr38), 0x08, 0x8E);
        set_gate(39, reinterpret_cast<uint64_t>(isr39), 0x08, 0x8E);
        set_gate(40, reinterpret_cast<uint64_t>(isr40), 0x08, 0x8E);
        set_gate(41, reinterpret_cast<uint64_t>(isr41), 0x08, 0x8E);
        set_gate(42, reinterpret_cast<uint64_t>(isr42), 0x08, 0x8E);
        set_gate(43, reinterpret_cast<uint64_t>(isr43), 0x08, 0x8E);
        set_gate(44, reinterpret_cast<uint64_t>(isr44), 0x08, 0x8E);
        set_gate(45, reinterpret_cast<uint64_t>(isr45), 0x08, 0x8E);
        set_gate(46, reinterpret_cast<uint64_t>(isr46), 0x08, 0x8E);
        set_gate(47, reinterpret_cast<uint64_t>(isr47), 0x08, 0x8E);

        set_gate(MAILBOX_VECTOR, reinterpret_cast<uint64_t>(isr254), 0x08, 0x8E);

        init_ap();
    }

    uint8_t get_unused_vector() {
        for (uint16_t i = 34; i <= 255; ++i) {
            if (i == MAILBOX_VECTOR) continue;

            if (idt[i].offset_low == 0 && idt[i].offset_mid == 0 && idt[i].offset_high == 0) {
                return i;
            }
        }
        return 0xFF;
    }

    void init_ap(void) { idt::load(); }
}  // namespace idt