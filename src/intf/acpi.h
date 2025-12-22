#pragma once

#include <stdint.h>
#include <stddef.h>

namespace acpi {

    struct SDTHeader {
        char signature[4];
        uint32_t length;
        uint8_t revision;
        uint8_t checksum;
        char oem_id[6];
        char oem_table_id[8];
        uint32_t oem_revision;
        uint32_t creator_id;
        uint32_t creator_revision;
    } __attribute__((packed));

    struct RSDP {
        char signature[8];
        uint8_t checksum;
        char oem_id[6];
        uint8_t revision;
        uint32_t rsdt_address;
    } __attribute__((packed));

    struct XSDP {
        RSDP rsdp_v1;
        uint32_t length;
        uint64_t xsdt_address;
        uint8_t extended_checksum;
        uint8_t reserved[3];
    } __attribute__((packed));

    struct MADT {
        SDTHeader header;
        uint32_t lapic_addr;
        uint32_t flags;
    } __attribute__((packed));

    struct MADTEntryHeader {
        uint8_t type;
        uint8_t length;
    } __attribute__((packed));

    struct MADTEntryLAPIC {
        MADTEntryHeader header;
        uint8_t processor_id;
        uint8_t lapic_id;
        uint32_t flags;
    } __attribute__((packed));

    // Public API
    void init(uint64_t mb_phys_addr);
    SDTHeader* find_table(const char* signature);

} // namespace acpi