/**
 * @file gdt.cpp
 * @brief Implementation of the Global Descriptor Table (GDT)
 *
 * Sets up the standard kernel GDT entries and loads the GDT into the CPU.
 * Supports null, kernel code, and kernel data segments.
 */

#include "gdt.h"

#include "idt.h"
#include "smp/smp.h"

namespace gdt {
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
    static void set_entry(gdt_entry *entry, uint32_t base, uint32_t limit, uint8_t access,
                          uint8_t gran) {
        entry->limit_low = limit & 0xFFFF;
        entry->base_low = base & 0xFFFF;
        entry->base_middle = (base >> 16) & 0xFF;
        entry->access = access;
        entry->granularity = (limit >> 16) & 0x0F;
        entry->granularity |= gran & 0xF0;
        entry->base_high = (base >> 24) & 0xFF;
    }

    /**
     * @brief Initialize the GDT
     *
     * Sets up the standard null, kernel code, and kernel data segments,
     * constructs the GDT pointer, and loads it using `lgdt`.
     */
    void init_early() {
        static gdt_entry gdt_entries[3];
        static gdt_pointer gdt_ptr;

        // Null descriptor
        set_entry(&gdt_entries[0], 0, 0, 0, 0);

        // Kernel code segment: 64-bit
        // Access: 0x9A = Present | Ring0 | Code | Executable | Readable
        // Gran: 0x20 = Long mode
        set_entry(&gdt_entries[1], 0, 0, 0x9A, 0x20);

        // Kernel data segment: 64-bit data
        // Access: 0x92 = Present | Ring0 | Data | Writable
        set_entry(&gdt_entries[2], 0, 0, 0x92, 0x00);

        // Setup GDT pointer
        gdt_ptr.limit = sizeof(gdt_entries) - 1;
        gdt_ptr.base = reinterpret_cast<uint64_t>(&gdt_entries);

        // Load the GDT
        gdt_load(reinterpret_cast<uint64_t>(&gdt_ptr));
    }

    void init_core() {
        smp::cpu_local *local = smp::get_cpu();

        if (!local || local->self != local) kpanic("GDT: CPU-local missing");
        if (!local->kernel_stack) kpanic("GDT: kernel stack missing");
        if (!local->panic_stack) kpanic("GDT: panic stack missing");

        local->tss_entry.iopb_offset = sizeof(tss);
        local->tss_entry.rsp0 = reinterpret_cast<uint64_t>(local->kernel_stack);
        local->tss_entry.ist1 = reinterpret_cast<uint64_t>(local->panic_stack);

        // Index 0: Null
        set_entry(&local->gdt_entries[0], 0, 0, 0, 0);

        // Index 1: Kernel Code (0x08)
        set_entry(&local->gdt_entries[1], 0, 0, 0x9A, 0x20);

        // Index 2: Kernel Data (0x10)
        set_entry(&local->gdt_entries[2], 0, 0, 0x92, 0x00);

        // Index 3: User Code 32 (0x1B) - Access: 0xFA, Gran: 0xCF (Limit 4GB, 32-bit)
        set_entry(&local->gdt_entries[3], 0, 0xFFFFF, 0xFA, 0xCF);

        // Index 4: User Data 32 (0x23) - Access: 0xF2, Gran: 0xCF (Limit 4GB)
        set_entry(&local->gdt_entries[4], 0, 0xFFFFF, 0xF2, 0xCF);

        // Index 5: User Data 64 (0x2B)
        set_entry(&local->gdt_entries[5], 0, 0, 0xF2, 0x00);

        // Index 6: User Code 64 (0x33) - Gran: 0x20 (Long Mode)
        set_entry(&local->gdt_entries[6], 0, 0, 0xFA, 0x20);

        // Index 7 & 8: TSS is 16 bytes in 64-bit mode
        uintptr_t tss_addr = reinterpret_cast<uintptr_t>(&local->tss_entry);
        uint32_t tss_limit = sizeof(tss) - 1;

        local->gdt_entries[7].limit_low = tss_limit & 0xFFFF;
        local->gdt_entries[7].base_low = tss_addr & 0xFFFF;
        local->gdt_entries[7].base_middle = (tss_addr >> 16) & 0xFF;
        local->gdt_entries[7].access = 0x89;  // Present, Executable, Accessed (TSS Type)
        local->gdt_entries[7].granularity = (tss_limit >> 16) & 0x0F;
        local->gdt_entries[7].base_high = (tss_addr >> 24) & 0xFF;

        uint32_t *high_part = reinterpret_cast<uint32_t *>(&local->gdt_entries[8]);
        high_part[0] = (tss_addr >> 32) & 0xFFFFFFFF;
        high_part[1] = 0;

        local->gdt_ptr.limit = (sizeof(gdt_entry) * MAX_ENTRIES) - 1;
        local->gdt_ptr.base = reinterpret_cast<uint64_t>(local->gdt_entries);

        gdt_load(reinterpret_cast<uint64_t>(&local->gdt_ptr));

        asm volatile("ltr %0" : : "a"(static_cast<uint16_t>(TSS_SEL)) : "memory");
    }

    void set_panic_ist(void *stack_top) {
        smp::cpu_local *local = smp::get_cpu();
        if (!local || local->self != local) return;
        local->panic_stack = stack_top;
        local->tss_entry.ist1 = reinterpret_cast<uint64_t>(stack_top);
    }

}  // namespace gdt