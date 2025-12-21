/**
 * @file gdt.cpp
 * @brief Implementation of the Global Descriptor Table (GDT)
 *
 * Sets up the standard kernel GDT entries and loads the GDT into the CPU.
 * Supports null, kernel code, and kernel data segments.
 */

#include "gdt.h"

namespace gdt {
    /** @internal Array of GDT entries */
    static GDTEntry gdt_entries[3];
    /** @internal GDT pointer passed to lgdt instruction */
    static GDTPtr gdt_ptr;

    /**
     * @internal
     * Set a single GDT entry.
     *
     * @param idx Index of the GDT entry to set
     * @param base Base address of the segment
     * @param limit Limit of the segment
     * @param access Access flags (present, ring, type)
     * @param gran Granularity flags (size, long mode, upper limit bits)
     */
    static void set_entry(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
        gdt_entries[idx].limit_low    = limit & 0xFFFF;
        gdt_entries[idx].base_low     = base & 0xFFFF;
        gdt_entries[idx].base_middle  = (base >> 16) & 0xFF;
        gdt_entries[idx].access       = access;
        gdt_entries[idx].granularity  = (limit >> 16) & 0x0F;
        gdt_entries[idx].granularity |= gran & 0xF0;
        gdt_entries[idx].base_high    = (base >> 24) & 0xFF;
    }

    /** @internal Assembly routine to load the GDT pointer into the CPU */
    extern "C" void gdt_load(uint64_t gdt_ptr_addr);

    /**
     * @brief Initialize the GDT
     *
     * Sets up the standard null, kernel code, and kernel data segments,
     * constructs the GDT pointer, and loads it using `lgdt`.
     */
    void init() {
        // Null descriptor
        set_entry(0, 0, 0, 0, 0);

        // Kernel code segment: 64-bit
        // Access: 0x9A = Present | Ring0 | Code | Executable | Readable
        // Gran: 0x20 = Long mode
        set_entry(1, 0, 0, 0x9A, 0x20);

        // Kernel data segment: 64-bit data
        // Access: 0x92 = Present | Ring0 | Data | Writable
        set_entry(2, 0, 0, 0x92, 0x00);

        // Setup GDT pointer
        gdt_ptr.limit = sizeof(gdt_entries) - 1;
        gdt_ptr.base  = reinterpret_cast<uint64_t>(&gdt_entries);

        // Load the GDT
        gdt_load(reinterpret_cast<uint64_t>(&gdt_ptr));
    }

    /**
     * @brief Retrieve the current GDT pointer
     *
     * @return GDTPtr containing base address and limit of the GDT
     */
    GDTPtr get_gdt_ptr() {
        return gdt_ptr;
    }
} // namespace gdt