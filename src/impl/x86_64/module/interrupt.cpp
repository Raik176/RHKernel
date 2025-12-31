#include "mod/interrupt.h"

#include "memory/heap.h"
#include "smp/apic.h"
#include "smp/ioapic.h"
#include "smp/lock.h"  // Assuming you have a spinlock implementation
#include "symbol/ksym.h"

namespace interrupt_manager {
    struct irq_node {
        irq_handler_t handler;
        void *priv;
        cpu_affinity_t affinity;
        irq_node *next;
    };

    // Array of linked lists for vectors 32-255
    static irq_node *handler_chains[256] = {nullptr};

    // This is called by idt_handler in idt.cpp
    extern "C" void dispatch_irq(struct regs *r) {
        irq_node *node = handler_chains[r->int_no];

        // Potential optimization: check affinity here if specific
        // per-core routing isn't fully handled by IOAPIC

        while (node) {
            enum irq_return ret = node->handler(node->priv);
            if (ret == IRQ_HANDLED) break;
            node = node->next;
        }

        apic::eoi();
    }
}  // namespace interrupt_manager

extern "C" int request_irq(uint8_t irq, irq_handler_t handler, cpu_affinity_t affinity,
                           void *priv) {
    // 1. Resolve Global System Interrupt to Vector
    // Note: Usually we map IRQ 0..15 to Vectors 32..47
    uint32_t gsi = ioapic::resolve_gsi(irq);
    uint8_t vector = 32 + irq;  // Basic mapping, can be more dynamic

    // 2. Create chain node
    auto *new_node =
        (interrupt_manager::irq_node *)heap::kmalloc(sizeof(interrupt_manager::irq_node));
    new_node->handler = handler;
    new_node->priv = priv;
    new_node->affinity = affinity;
    new_node->next = interrupt_manager::handler_chains[vector];

    // 3. Update chain
    interrupt_manager::handler_chains[vector] = new_node;

    // 4. Configure IOAPIC Redirection
    // We target the first CPU in the affinity mask for now
    uint32_t target_lapic = 0;
    for (int i = 0; i < 64; i++) {
        if (affinity & (1ULL << i)) {
            target_lapic = i;  // Simplified: assumes LAPIC ID == CPU Index
            break;
        }
    }

    ioapic::set_redirection(gsi, vector, target_lapic, false);

    return 0;
}

extern "C" void free_irq(uint8_t irq, irq_handler_t handler) {
    uint8_t vector = 32 + irq;
    interrupt_manager::irq_node **curr = &interrupt_manager::handler_chains[vector];

    while (*curr) {
        if ((*curr)->handler == handler) {
            interrupt_manager::irq_node *to_free = *curr;
            *curr = (*curr)->next;
            heap::kfree(to_free);
            break;
        }
        curr = &((*curr)->next);
    }

    // If no handlers left, mask the IRQ in IOAPIC
    if (interrupt_manager::handler_chains[vector] == nullptr) {
        uint32_t gsi = ioapic::resolve_gsi(irq);
        ioapic::set_redirection(gsi, vector, 0, true);
    }
}

KEXPORT(request_irq);
KEXPORT(free_irq);