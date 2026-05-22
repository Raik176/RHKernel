#include "power.h"

#include "acpi.h"

#include <stddef.h>
#include "console.h"
#include "portio.h"
#include "string.h"
#include "util.h"

namespace power {

struct GenericAddressStructure {
    uint8_t address_space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

struct FADT {
    SDTHeader header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_control;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    GenericAddressStructure reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t fadt_minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    GenericAddressStructure x_pm1a_evt_blk;
    GenericAddressStructure x_pm1b_evt_blk;
    GenericAddressStructure x_pm1a_cnt_blk;
    GenericAddressStructure x_pm1b_cnt_blk;
} __attribute__((packed));

static constexpr uint16_t ACPI_PM1_SCI_EN = 1u;
static constexpr uint16_t ACPI_PM1_SLP_EN = 1u << 13;
static constexpr uint16_t ACPI_PM1_SLP_TYP_SHIFT = 10;

[[noreturn]] static inline void halt_forever() {
    asm volatile("cli");
    for (;;) asm volatile("hlt");
}

static uint32_t decode_pkg_length(const uint8_t *p, const uint8_t *end, uint32_t *used) {
    if (!p || p >= end || !used) return 0;
    uint8_t lead = *p;
    uint8_t count = lead >> 6;
    uint32_t value = lead & 0x3F;
    *used = 1;
    if (count == 0) return value;
    value = lead & 0x0F;
    for (uint8_t i = 0; i < count; ++i) {
        if (p + 1 + i >= end) return 0;
        value |= ((uint32_t)p[1 + i]) << (4 + 8 * i);
    }
    *used = 1 + count;
    return value;
}

static bool aml_read_integer(const uint8_t **cursor, const uint8_t *end, uint64_t *out) {
    if (!cursor || !*cursor || *cursor >= end || !out) return false;
    const uint8_t *p = *cursor;

    switch (*p++) {
        case 0x00: *out = 0; *cursor = p; return true;               // ZeroOp
        case 0x01: *out = 1; *cursor = p; return true;               // OneOp
        case 0x0A:                                                    // BytePrefix
            if (p + 1 > end) return false;
            *out = p[0]; *cursor = p + 1; return true;
        case 0x0B:                                                    // WordPrefix
            if (p + 2 > end) return false;
            *out = (uint16_t)(p[0] | (p[1] << 8)); *cursor = p + 2; return true;
        case 0x0C:                                                    // DWordPrefix
            if (p + 4 > end) return false;
            *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 24);
            *cursor = p + 4;
            return true;
        default:
            // Some firmware emits raw small integers inside simple packages.
            p--;
            if (*p <= 0x3F) {
                *out = *p;
                *cursor = p + 1;
                return true;
            }
            return false;
    }
}

static bool find_s5_package(SDTHeader *dsdt, uint8_t *slp_typa, uint8_t *slp_typb) {
    if (!dsdt || !slp_typa || !slp_typb || dsdt->length <= sizeof(SDTHeader)) return false;

    const uint8_t *base = (const uint8_t *)dsdt;
    const uint8_t *start = base + sizeof(SDTHeader);
    const uint8_t *end = base + dsdt->length;

    for (const uint8_t *p = start; p + 8 < end; ++p) {
        const uint8_t *name = nullptr;
        if (p[0] == '_' && p[1] == 'S' && p[2] == '5' && p[3] == '_') {
            name = p;
        } else if (p[0] == '\\' && p + 5 < end && p[1] == '_' && p[2] == 'S' && p[3] == '5' && p[4] == '_') {
            name = p + 1;
        } else {
            continue;
        }

        const uint8_t *q = name + 4;
        while (q < end && (*q == 0x08 || *q == '\\')) q++; // NameOp/root prefix tolerance
        if (q >= end || *q != 0x12) {                       // PackageOp
            if (name > start && name[-1] == 0x08) {
                q = name + 4;
                if (q >= end || *q != 0x12) continue;
            } else {
                continue;
            }
        }
        q++;

        uint32_t pkg_len_used = 0;
        (void)decode_pkg_length(q, end, &pkg_len_used);
        if (pkg_len_used == 0 || q + pkg_len_used >= end) continue;
        q += pkg_len_used;

        if (q >= end) continue;
        uint8_t element_count = *q++;
        if (element_count < 2) continue;

        uint64_t a = 0, b = 0;
        if (!aml_read_integer(&q, end, &a)) continue;
        if (!aml_read_integer(&q, end, &b)) continue;

        *slp_typa = (uint8_t)(a & 0x7);
        *slp_typb = (uint8_t)(b & 0x7);
        return true;
    }

    return false;
}

static uint16_t read_pm1_control(const GenericAddressStructure *gas, uint32_t legacy_port) {
    if (gas && gas->address) {
        if (gas->address_space == 1) return inw((uint16_t)gas->address);
        if (gas->address_space == 0) return *(volatile uint16_t *)(uintptr_t)gas->address;
    }
    if (legacy_port) return inw((uint16_t)legacy_port);
    return 0;
}

static void write_pm1_control(const GenericAddressStructure *gas, uint32_t legacy_port, uint16_t value) {
    if (gas && gas->address) {
        if (gas->address_space == 1) {
            outw((uint16_t)gas->address, value);
            return;
        }
        if (gas->address_space == 0) {
            *(volatile uint16_t *)(uintptr_t)gas->address = value;
            return;
        }
    }
    if (legacy_port) outw((uint16_t)legacy_port, value);
}

static bool gas_present(const GenericAddressStructure *gas) {
    return gas && gas->address != 0 && (gas->address_space == 0 || gas->address_space == 1);
}

static bool fadt_has_x_fields(FADT *fadt) {
    return fadt && fadt->header.length >= offsetof(FADT, x_pm1b_cnt_blk) + sizeof(GenericAddressStructure);
}


static bool write_gas_integer(const GenericAddressStructure *gas, uint64_t value) {
    if (!gas || gas->address == 0) return false;

    uint8_t width = gas->bit_width ? gas->bit_width : (uint8_t)(gas->access_size * 8);
    if (width == 0) width = 8;

    if (gas->address_space == 1) { // System I/O
        if (width <= 8) { outb((uint16_t)gas->address, (uint8_t)value); return true; }
        if (width <= 16) { outw((uint16_t)gas->address, (uint16_t)value); return true; }
        outl((uint16_t)gas->address, (uint32_t)value);
        return true;
    }

    if (gas->address_space == 0) { // System memory, physical address
        void *addr = p2v(gas->address);
        if (width <= 8) { *(volatile uint8_t *)addr = (uint8_t)value; return true; }
        if (width <= 16) { *(volatile uint16_t *)addr = (uint16_t)value; return true; }
        *(volatile uint32_t *)addr = (uint32_t)value;
        return true;
    }

    return false;
}

static bool acpi_restart() {
    FADT *fadt = (FADT *)acpi::find_table("FACP");
    if (!fadt) {
        console::printf("restart: ACPI FADT/FACP not found\n");
        return false;
    }

    if (fadt->header.length < offsetof(FADT, reset_value) + sizeof(uint8_t)) {
        console::printf("restart: FADT has no reset register\n");
        return false;
    }

    if (!gas_present(&fadt->reset_reg)) {
        console::printf("restart: FADT reset register is absent or unsupported\n");
        return false;
    }

    console::printf("restart: using ACPI reset register\n");
    if (!write_gas_integer(&fadt->reset_reg, fadt->reset_value)) return false;

    for (uint64_t i = 0; i < 10000000; ++i) asm volatile("pause");
    return false;
}

static void restart_fallbacks() {
    console::printf("restart: trying reset fallback ports\n");

    // PCI reset control register used by many chipsets/emulators.
    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);
    io_wait();

    // i8042 keyboard-controller reset. Wait briefly for input-buffer empty.
    for (uint32_t i = 0; i < 100000; ++i) {
        if ((inb(0x64) & 0x02) == 0) break;
        io_wait();
    }
    outb(0x64, 0xFE);
    io_wait();
}

static void triple_fault() {
    struct Idtr { uint16_t limit; uint64_t base; } __attribute__((packed));
    Idtr idtr = {0, 0};
    asm volatile("lidt %0" : : "m"(idtr));
    asm volatile("int3");
}

static bool acpi_poweroff() {
    FADT *fadt = (FADT *)acpi::find_table("FACP");
    if (!fadt) {
        console::printf("shutdown: ACPI FADT/FACP not found\n");
        return false;
    }

    uint64_t dsdt_phys = 0;
    if (fadt->header.length >= offsetof(FADT, x_dsdt) + sizeof(uint64_t) && fadt->x_dsdt) {
        dsdt_phys = fadt->x_dsdt;
    } else {
        dsdt_phys = fadt->dsdt;
    }
    if (!dsdt_phys) {
        console::printf("shutdown: FADT does not point to a DSDT\n");
        return false;
    }

    SDTHeader *dsdt = (SDTHeader *)p2v(dsdt_phys);
    uint8_t slp_typa = 0, slp_typb = 0;
    if (!find_s5_package(dsdt, &slp_typa, &slp_typb)) {
        console::printf("shutdown: ACPI _S5 package not found in DSDT\n");
        return false;
    }

    bool have_x = fadt_has_x_fields(fadt);
    const GenericAddressStructure *pm1a_gas = (have_x && gas_present(&fadt->x_pm1a_cnt_blk)) ? &fadt->x_pm1a_cnt_blk : nullptr;
    const GenericAddressStructure *pm1b_gas = (have_x && gas_present(&fadt->x_pm1b_cnt_blk)) ? &fadt->x_pm1b_cnt_blk : nullptr;

    if (!pm1a_gas && fadt->pm1a_cnt_blk == 0) {
        console::printf("shutdown: no PM1a control block in FADT\n");
        return false;
    }

    if (fadt->smi_cmd && fadt->acpi_enable) {
        uint16_t pm1 = read_pm1_control(pm1a_gas, fadt->pm1a_cnt_blk);
        if ((pm1 & ACPI_PM1_SCI_EN) == 0) {
            outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);
            for (uint32_t i = 0; i < 100000; ++i) {
                io_wait();
                pm1 = read_pm1_control(pm1a_gas, fadt->pm1a_cnt_blk);
                if (pm1 & ACPI_PM1_SCI_EN) break;
            }
        }
    }

    console::printf("shutdown: ACPI S5 slp_typa=%d slp_typb=%d\n", slp_typa, slp_typb);

    uint16_t value_a = (uint16_t)((slp_typa << ACPI_PM1_SLP_TYP_SHIFT) | ACPI_PM1_SLP_EN);
    uint16_t value_b = (uint16_t)((slp_typb << ACPI_PM1_SLP_TYP_SHIFT) | ACPI_PM1_SLP_EN);
    write_pm1_control(pm1a_gas, fadt->pm1a_cnt_blk, value_a);
    if (pm1b_gas || fadt->pm1b_cnt_blk) write_pm1_control(pm1b_gas, fadt->pm1b_cnt_blk, value_b);

    for (uint64_t i = 0; i < 10000000; ++i) asm volatile("pause");
    return false;
}

static void emulator_poweroff_fallbacks() {
    // Common emulator/firmware fallbacks. They are harmless on normal hardware
    // unless a platform explicitly decodes these debug/shutdown ports.
    console::printf("shutdown: trying emulator fallback ports\n");
    outw(0x604, 0x2000);       // QEMU/Bochs ACPI poweroff commonly used by hobby kernels
    io_wait();
    outw(0xB004, 0x2000);      // Bochs/QEMU older examples
    io_wait();
    outw(0x4004, 0x3400);      // VirtualBox
    io_wait();
    outl(0x501, 0x00000000);   // Some emulator debug-exit variants
    io_wait();
}

[[noreturn]] void shutdown() {
    console::printf("shutdown: syncing and powering off\n");
    asm volatile("cli");

    if (!acpi_poweroff()) emulator_poweroff_fallbacks();

    console::printf("shutdown: poweroff did not complete; CPU halted\n");
    halt_forever();
}

[[noreturn]] void restart() {
    console::printf("restart: syncing and rebooting\n");
    asm volatile("cli");

    if (!acpi_restart()) restart_fallbacks();

    console::printf("restart: reset did not complete; trying triple fault\n");
    triple_fault();

    console::printf("restart: triple fault returned; CPU halted\n");
    halt_forever();
}

} // namespace power
