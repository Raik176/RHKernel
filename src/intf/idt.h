#pragma once
#include <stdint.h>

#include "util.h"

namespace idt {
    static constexpr uint32_t YIELD_VECTOR = 0x81;
    static constexpr uint32_t MAILBOX_VECTOR = 0xFE;

    /**
     * @brief Entry in the Interrupt Descriptor Table (IDT)
     *
     * Defines the offset, selector, flags, and IST for an interrupt handler.
     */
    struct idt_entry {
        uint16_t offset_low;   ///< Lower 16 bits of handler address
        uint16_t selector;     ///< Code segment selector in GDT
        uint8_t ist;           ///< Interrupt Stack Table index
        uint8_t flags;         ///< Type and attributes
        uint16_t offset_mid;   ///< Middle 16 bits of handler address
        uint32_t offset_high;  ///< Upper 32 bits of handler address
        uint32_t zero;         ///< Reserved, must be 0
    } __attribute__((packed));

    /**
     * @brief Pointer structure for loading the IDT
     *
     * Passed to the `lidt` instruction to tell the CPU where the IDT resides.
     */
    struct idt_ptr {
        uint16_t limit;  ///< Size of the IDT minus 1
        uint64_t base;   ///< Address of the first IDT entry
    } __attribute__((packed));

    /**
     * @brief Set an entry in the IDT
     *
     * Configures a single interrupt gate.
     *
     * @param num Interrupt number (0-255)
     * @param base Address of the ISR handler
     * @param sel Code segment selector from GDT
     * @param flags Type and attribute flags for the entry
     */
    void set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

    /**
     * @brief Initialize the IDT
     *
     * Sets up default entries and prepares the IDT for loading.
     */
    void init(void);

    void init_ap(void);

    uint8_t get_unused_vector();

}  // namespace idt