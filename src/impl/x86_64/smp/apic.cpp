#include "smp/apic.h"

#include "console.h"
#include "memory/vmm.h"
#include "smp/smp.h"
#include "util.h"

namespace apic {
    static uintptr_t apic_base = 0;
    static bool x2apic_mode = false;
    static uint32_t ticks_per_ms = 0;
    static uint32_t tick_scale = 1;
    static uint32_t bsp_id = 0;
    static volatile uint64_t global_ticks = 0;
    static uint32_t pending_icr_destination = 0;

    static constexpr uint32_t X2APIC_MSR_BASE = 0x800;
    static constexpr uint32_t X2APIC_MSR_ICR = 0x830;
    static constexpr uint32_t ICR_DELIVERY_STATUS = 1u << 12;

    uint32_t read_reg(uint32_t reg) {
        if (x2apic_mode) {
            if (reg == Register::ICRHI) { return pending_icr_destination << 24; }
            return (uint32_t)rdmsr(X2APIC_MSR_BASE + (reg >> 4));
        }
        return *(volatile uint32_t *)(apic_base + reg);
    }

    void write_reg(uint32_t reg, uint32_t val) {
        if (x2apic_mode) {
            if (reg == Register::ICRHI) {
                pending_icr_destination = val >> 24;
                return;
            }
            if (reg == Register::ICRLO) {
                wrmsr(X2APIC_MSR_ICR, ((uint64_t)pending_icr_destination << 32) | val);
                return;
            }
            wrmsr(X2APIC_MSR_BASE + (reg >> 4), val);
        } else {
            *(volatile uint32_t *)(apic_base + reg) = val;
        }
    }

    static void wait_icr_idle() {
        if (x2apic_mode) return;

        for (uint32_t i = 0; i < 1000000; ++i) {
            if ((read_reg(Register::ICRLO) & ICR_DELIVERY_STATUS) == 0) return;
            asm volatile("pause");
        }

        kpanic("APIC: ICR delivery stuck");
    }

    void send_ipi(uint32_t lapic_id, uint32_t icr_low) {
        if (x2apic_mode) {
            wrmsr(X2APIC_MSR_ICR, ((uint64_t)lapic_id << 32) | icr_low);
            return;
        }

        if (lapic_id > 0xFF) kpanic("APIC: xAPIC destination too large");
        wait_icr_idle();
        write_reg(Register::ICRHI, lapic_id << 24);
        write_reg(Register::ICRLO, icr_low);
        wait_icr_idle();
    }

    void eoi() { write_reg(Register::EOI, 0); }

    void stop() {
        write_reg(Register::TICF, 0);
        write_reg(Register::TIMER, 1 << 16);  // Masked
    }

    static void calibrate_timer() {
        write_reg(Register::TDCR, 0x03);

        // PIT frequency is 1.193182 MHz. 10ms = 11931 ticks.
        uint16_t pit_ticks = 11931;
        outb(0x43, 0x30);  // Channel 0, lo/hi byte, mode 0
        outb(0x40, (uint8_t)(pit_ticks & 0xFF));
        outb(0x40, (uint8_t)(pit_ticks >> 8));

        write_reg(Register::TICF, 0xFFFFFFFF);

        while (true) {
            outb(0x43, 0x00);  // Latch
            uint8_t low = inb(0x40);
            uint8_t high = inb(0x40);
            if (((high << 8) | low) == 0) break;
            if (((high << 8) | low) > pit_ticks) break;
        }

        uint32_t current_apic = read_reg(Register::TCCF);
        uint32_t ticks_in_10ms = 0xFFFFFFFF - current_apic;

        ticks_per_ms = ticks_in_10ms / 10;
        stop();
    }

    void tick() {
        smp::cpu_local *cpu = smp::get_cpu();
        if (!cpu) return;
        cpu->ticks++;
        if (cpu->lapic_id == bsp_id) __atomic_add_fetch(&global_ticks, 1, __ATOMIC_RELAXED);
    }

    void init_ap() {
        uint64_t base_msr = rdmsr(0x1B);
        if (x2apic_mode) {
            base_msr |= (1 << 10) | (1 << 11);
        } else {
            base_msr |= (1 << 11);
        }
        wrmsr(0x1B, base_msr);

        write_reg(Register::SVR, read_reg(Register::SVR) | 0x1FF);

        write_reg(Register::LINT0, 0x10000);  // Masked
        write_reg(Register::LINT1, 0x400);    // NMI

        write_reg(Register::TIMER, 32 | (1 << 17));  // Vector 32, Periodic mode
        write_reg(Register::TDCR, 0x03);             // Divider 16
        write_reg(Register::TICF, ticks_per_ms * tick_scale);
    }

    void init() {
        outb(0x21, 0xFF);
        outb(0xA1, 0xFF);

        uint32_t eax, ebx, ecx, edx;
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        x2apic_mode = (ecx >> 21) & 1;

        uint64_t base_msr = rdmsr(0x1B);
        apic_base = (uintptr_t)vmm::mmio_map(base_msr & 0xFFFFF000, 0x1000);

        uint64_t new_msr = rdmsr(0x1B) | (1 << 11);
        if (x2apic_mode) { new_msr |= (1 << 10); }
        wrmsr(0x1B, new_msr);
        calibrate_timer();

        init_ap();

        bsp_id = get_id();
    }

    bool is_bsp() {
        return (rdmsr(0x1B) >> 8) & 1;  // IA32_APIC_BASE; Bit 8 is BSP Flag
    }

    uint32_t get_bsp_id() { return bsp_id; }

    uint32_t get_id() {
        uint32_t id = read_reg(Register::ID);
        return x2apic_mode ? id : (id >> 24);
    }

    uint32_t get_tick_scale() { return tick_scale; }
    uint64_t get_ticks() { return smp::get_cpu()->ticks; }
    uint64_t get_global_ticks() { return __atomic_load_n(&global_ticks, __ATOMIC_RELAXED); }

}  // namespace apic