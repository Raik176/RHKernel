#pragma once
#include <stdint.h>

namespace gdt {

    /**
     * @brief Represents a single Global Descriptor Table (GDT) entry.
     *
     * Each entry defines a memory segment with a base, limit, and access flags.
     */
    struct GDTEntry {
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
    struct GDTPtr {
        uint16_t limit;  ///< Size of the GDT minus 1
        uint64_t base;   ///< Address of the first GDT entry
    } __attribute__((packed));

    /**
     * @brief Predefined segment selectors in the GDT.
     */
    enum class Segment : uint16_t {
        NULL_SEG = 0,     ///< Null segment
        KERNEL_CODE = 1,  ///< Kernel code segment
        KERNEL_DATA = 2,  ///< Kernel data segment
    };

    /**
     * @brief Initialize the Global Descriptor Table (GDT)
     *
     * Sets up the standard kernel code and data segments and loads the GDT.
     */
    void init();

    /**
     * @brief Retrieve the pointer to the current GDT
     *
     * @return GDTPtr structure containing the base address and limit of the GDT
     */
    GDTPtr get_gdt_ptr();

}  // namespace gdt