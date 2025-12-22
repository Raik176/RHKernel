#pragma once
#include <stdint.h>
#include "idt.h"

namespace apic {
    static constexpr uint32_t MSR_GS_BASE = 0xC0000101;

    enum Register {
        ID          = 0x0020,
        VERSION     = 0x0030,
        TPR         = 0x0080,
        EOI         = 0x00B0,
        LDR         = 0x00D0,
        DFR         = 0x00E0,
        SVR         = 0x00F0,
        ESR         = 0x0280,
        ICRLO       = 0x0300,
        ICRHI       = 0x0310,
        TIMER       = 0x0320,
        PCINT       = 0x0340,
        LINT0       = 0x0350,
        LINT1       = 0x0360,
        ERROR       = 0x0370,
        TICF        = 0x0380, // Timer Initial Count
        TCCF        = 0x0390, // Timer Current Count
        TDCR        = 0x03E0  // Timer Divide Configuration
    };

    void init();
    void init_ap();
    void eoi();
    void tick();
    uint32_t get_tick_scale();
    uint64_t get_ticks();
    void stop();
    bool is_bsp();
    uint32_t get_bsp_id();
    uint32_t get_id();

    static inline uint64_t rdmsr(uint32_t msr) {
        uint32_t low, high;
        asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
        return ((uint64_t)high << 32) | low;
    }

    static inline void wrmsr(uint32_t msr, uint64_t val) {
        asm volatile("wrmsr" : : "a"((uint32_t)val), "d"((uint32_t)(val >> 32)), "c"(msr));
    }

    uint32_t read_reg(uint32_t reg);
    void write_reg(uint32_t reg, uint32_t val);
}