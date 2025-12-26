#pragma once
#include <stdint.h>

namespace gdt {
    static constexpr uint16_t MAX_ENTRIES = 8;

    /**
     * @brief Represents a single Global Descriptor Table (GDT) entry.
     *
     * Each entry defines a memory segment with a base, limit, and access flags.
     */
    struct gdt_entry {
        uint16_t limit_low;   ///< Lower 16 bits of the segment limit
        uint16_t base_low;    ///< Lower 16 bits of the segment base address
        uint8_t base_middle;  ///< Next 8 bits of the segment base address
        uint8_t access;       ///< Access flags defining segment type and permissions
        uint8_t granularity;  ///< Granularity flags and upper 4 bits of the segment limit
        uint8_t base_high;    ///< Last 8 bits of the segment base address
    } __attribute__((packed));

    /**
     * @brief Pointer structure used to load the GDT.
     *
     * Passed to the `lgdt` instruction.
     */
    struct gdt_pointer {
        uint16_t limit;  ///< Size of the GDT minus 1
        uint64_t base;   ///< Address of the first GDT entry
    } __attribute__((packed));

    struct tss {
        uint32_t reserved0;
        uint64_t rsp0;  // Stack pointer for Ring 0
        uint64_t rsp1;  // Stack pointer for Ring 1
        uint64_t rsp2;  // Stack pointer for Ring 2
        uint64_t reserved1;
        uint64_t ist1;  // Interrupt Stack Table 1
        uint64_t ist2;  // Interrupt Stack Table 2
        uint64_t ist3;  // Interrupt Stack Table 3
        uint64_t ist4;  // Interrupt Stack Table 4
        uint64_t ist5;  // Interrupt Stack Table 5
        uint64_t ist6;  // Interrupt Stack Table 6
        uint64_t ist7;  // Interrupt Stack Table 7
        uint64_t reserved2;
        uint16_t reserved3;
        uint16_t iopb_offset;  // Offset from start of TSS to I/O Permission Bit Map
    } __attribute__((packed));

    /**
     * @brief Initialize the Global Descriptor Table (GDT)
     *
     * Sets up the standard kernel code and data segments and loads the GDT.
     */
    void init_early();

    void init_core();

    extern "C" void gdt_load(uint64_t gdt_ptr_addr);

}  // namespace gdt