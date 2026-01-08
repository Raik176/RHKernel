#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PCI_BAR_TYPE_NONE = 0,
    PCI_BAR_TYPE_MMIO32,
    PCI_BAR_TYPE_MMIO64,
    PCI_BAR_TYPE_IO
} pci_bar_type_t;

struct pci_resource {
    uint64_t base;
    uint64_t size;
    pci_bar_type_t type;
    bool prefetchable;
};

struct pci_device {
    uint16_t segment;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;

    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;

    struct pci_resource bars[6];
};

struct pci_device_id {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
};

#define PCI_ANY_ID ((uint16_t)0xFFFF)
#define PCI_ANY_CLASS ((uint8_t)0xFF)

uint32_t pci_read32(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset);
uint16_t pci_read16(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset);
uint8_t pci_read8(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset);

void pci_write32(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset,
                 uint32_t val);
void pci_write16(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset,
                 uint16_t val);
void pci_write8(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset,
                uint8_t val);

uint16_t pci_find_capability(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id);
uint16_t pci_find_extended_capability(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func,
                                      uint16_t cap_id);

void pci_enable_bus_mastering(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func);
bool pci_check_and_clear_errors(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func);
void pci_scan_bus(uint16_t seg, uint8_t bus);
void pci_enumerate();