#pragma once
#include <stdint.h>

/**
 * @brief CPU register state structure
 *
 * Represents the state of registers pushed to the stack during
 * an interrupt or exception.
 */
struct regs {
    uint64_t r15; ///< General-purpose register R15
    uint64_t r14; ///< General-purpose register R14
    uint64_t r13; ///< General-purpose register R13
    uint64_t r12; ///< General-purpose register R12
    uint64_t r11; ///< General-purpose register R11
    uint64_t r10; ///< General-purpose register R10
    uint64_t r9;  ///< General-purpose register R9
    uint64_t r8;  ///< General-purpose register R8
    uint64_t rbp; ///< Base pointer register
    uint64_t rdi; ///< Destination index register
    uint64_t rsi; ///< Source index register
    uint64_t rdx; ///< Data register
    uint64_t rcx; ///< Counter register
    uint64_t rbx; ///< Base register
    uint64_t rax; ///< Accumulator register

    uint64_t int_no;   ///< Interrupt number
    uint64_t err_code; ///< Error code, if applicable

    uint64_t rip;    ///< Instruction pointer
    uint64_t cs;     ///< Code segment
    uint64_t rflags; ///< CPU flags register
    uint64_t rsp;    ///< Stack pointer
    uint64_t ss;     ///< Stack segment
} __attribute__((packed));

namespace idt {

/**
 * @brief Entry in the Interrupt Descriptor Table (IDT)
 *
 * Defines the offset, selector, flags, and IST for an interrupt handler.
 */
struct idt_entry {
    uint16_t offset_low;  ///< Lower 16 bits of handler address
    uint16_t selector;    ///< Code segment selector in GDT
    uint8_t  ist;         ///< Interrupt Stack Table index
    uint8_t  flags;       ///< Type and attributes
    uint16_t offset_mid;  ///< Middle 16 bits of handler address
    uint32_t offset_high; ///< Upper 32 bits of handler address
    uint32_t zero;        ///< Reserved, must be 0
} __attribute__((packed));

/**
 * @brief Pointer structure for loading the IDT
 *
 * Passed to the `lidt` instruction to tell the CPU where the IDT resides.
 */
struct idt_ptr {
    uint16_t limit; ///< Size of the IDT minus 1
    uint64_t base;  ///< Address of the first IDT entry
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

/**
 * @brief Load the IDT into the CPU
 *
 * Wraps the `lidt` instruction.
 */
inline void load(void);

} // namespace idt