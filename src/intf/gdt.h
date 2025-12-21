#pragma once
#include <stdint.h>

namespace gdt {
    struct GDTEntry {
        uint16_t limit_low;       // Lower 16 bits of limit
        uint16_t base_low;        // Lower 16 bits of base
        uint8_t  base_middle;     // Next 8 bits of base
        uint8_t  access;          // Access flags
        uint8_t  granularity;     // Granularity flags + limit bits
        uint8_t  base_high;       // Last 8 bits of base
    } __attribute__((packed));

    struct GDTPtr {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed));

    enum class Segment : uint16_t {
        NULL_SEG = 0,
        KERNEL_CODE = 1,
        KERNEL_DATA = 2,
    };

    void init();
    GDTPtr get_gdt_ptr();

} // namespace gdt