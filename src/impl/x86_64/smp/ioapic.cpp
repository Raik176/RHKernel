#include "acpi.h"
#include "console.h"
#include "memory/heap.h"
#include "smp/apic.h"
#include "string.h"

// TODO: map as Uncachable or similiar
namespace ioapic {
    struct controller {
        uintptr_t phys_addr;
        uint32_t gsi_base;
        uint32_t max_entries;
    };

    struct iso {
        uint8_t irq;
        uint32_t gsi;
    };

    static iso *overrides = nullptr;
    static size_t iso_count = 0;

    static controller *controllers = nullptr;
    static size_t count = 0;

    static void write(uintptr_t base, uint8_t reg, uint32_t val) {
        *(volatile uint32_t *)(p2v(base)) = reg;
        *(volatile uint32_t *)((uintptr_t)p2v(base) + 0x10) = val;
    }

    static uint32_t read(uintptr_t base, uint8_t reg) {
        *(volatile uint32_t *)(p2v(base)) = reg;
        return *(volatile uint32_t *)((uintptr_t)p2v(base) + 0x10);
    }

    uint32_t resolve_gsi(uint8_t irq) {
        for (size_t i = 0; i < iso_count; i++) {
            if (overrides[i].irq == irq) return overrides[i].gsi;
        }
        return irq;
    }

    void init_ioapic(uintptr_t phys, uint32_t gsi_base) {
        controllers = (controller *)heap::krealloc(controllers, sizeof(controller) * (count + 1));
        auto &c = controllers[count++];
        c.phys_addr = phys;
        c.gsi_base = gsi_base;

        uint32_t ver = read(phys, 0x01);
        c.max_entries = (ver >> 16) & 0xFF;

        for (uint32_t i = 0; i <= c.max_entries; i++) {
            write(phys, 0x10 + i * 2, (1 << 16));  // Mask all
        }
    }

    void init() {
        auto *madt_ptr = (acpi::MADT *)acpi::find_table("APIC");
        if (!madt_ptr) return;

        uintptr_t entry_ptr = (uintptr_t)madt_ptr + sizeof(acpi::MADT);
        uintptr_t end = (uintptr_t)madt_ptr + madt_ptr->header.length;

        while (entry_ptr < end) {
            auto *header = (acpi::MADTEntryHeader *)entry_ptr;

            if (header->type == 1) {  // IOAPIC
                struct IOAPIC_Entry {
                    acpi::MADTEntryHeader h;
                    uint8_t id;
                    uint8_t res;
                    uint32_t addr;
                    uint32_t gsi_base;
                } __attribute__((packed)) *io = (IOAPIC_Entry *)entry_ptr;
                init_ioapic(io->addr, io->gsi_base);
            } else if (header->type == 2) {  // ISO
                struct ISO_Entry {
                    acpi::MADTEntryHeader h;
                    uint8_t bus;
                    uint8_t source;
                    uint32_t gsi;
                    uint16_t flags;
                } __attribute__((packed)) *iso_ptr = (ISO_Entry *)entry_ptr;

                overrides = (iso *)heap::krealloc(overrides, sizeof(iso) * (iso_count + 1));
                overrides[iso_count++] = {iso_ptr->source, iso_ptr->gsi};
            }
            entry_ptr += header->length;
        }
    }

    void set_redirection(uint32_t gsi, uint8_t vector, uint32_t lapic_id, bool mask) {
        for (size_t i = 0; i < count; i++) {
            auto &c = controllers[i];
            if (gsi >= c.gsi_base && gsi <= (c.gsi_base + c.max_entries)) {
                uint32_t reg = 0x10 + (gsi - c.gsi_base) * 2;
                uint32_t low = vector | (mask ? (1 << 16) : 0);
                write(c.phys_addr, reg, low);
                write(c.phys_addr, reg + 1, lapic_id << 24);
                return;
            }
        }
    }
}  // namespace ioapic