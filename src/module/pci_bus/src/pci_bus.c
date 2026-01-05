#include "pci_bus.h"

#include "mod/acpi.h"
#include "mod/device.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "mod/vmm.h"
#include "portio.h"
#include "smp/lock.h"
#include "string.h"
#include "symbol.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static struct MCFGEntry *mcfg_entries = NULL;
static size_t mcfg_entry_count = 0;
static void **mcfg_mapped_bases = NULL;
static spinlock_t pci_legacy_lock;

static inline bool pci_id_is_end(const struct pci_device_id *id) {
    return id->vendor_id == 0 && id->device_id == 0 && id->class_code == 0 && id->subclass == 0 &&
           id->prog_if == 0;
}

static bool pci_device_id_match(const struct pci_device *dev, const struct pci_device_id *id) {
    if (id->vendor_id != PCI_ANY_ID && id->vendor_id != dev->vendor_id) return false;

    if (id->device_id != PCI_ANY_ID && id->device_id != dev->device_id) return false;

    if (id->class_code != PCI_ANY_CLASS && id->class_code != dev->class_code) return false;

    if (id->subclass != PCI_ANY_CLASS && id->subclass != dev->subclass) return false;

    if (id->prog_if != PCI_ANY_CLASS && id->prog_if != dev->prog_if) return false;

    return true;
}

static bool pci_bus_match(struct device *dev, struct driver *drv) {
    struct pci_device *pdev = dev->bus_data;
    const struct pci_device_id *ids = drv->id_table;

    if (!ids) return false;

    for (size_t i = 0; !pci_id_is_end(&ids[i]); i++) {
        if (pci_device_id_match(pdev, &ids[i])) return true;
    }
    return false;
}

static struct bus pci_bus_type = {.name = "pci", .match = pci_bus_match, .next = NULL};

static inline void pci_mmio_flush(volatile void *addr) { (void)*(volatile uint32_t *)addr; }

static volatile void *pci_get_ecam_addr(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func,
                                        uint16_t offset) {
    if (!mcfg_entries) return NULL;

    static size_t last_seg_idx = 0;

    struct MCFGEntry *cached = &mcfg_entries[last_seg_idx];
    if (seg == cached->pci_segment && bus >= cached->start_bus && bus <= cached->end_bus) {
        return (volatile void *)((uintptr_t)mcfg_mapped_bases[last_seg_idx] +
                                 (((uint32_t)(bus - cached->start_bus) << 20) |
                                  ((uint32_t)slot << 15) | ((uint32_t)func << 12) |
                                  (offset & 0xFFF)));
    }

    for (size_t i = 0; i < mcfg_entry_count; i++) {
        if (seg == mcfg_entries[i].pci_segment && bus >= mcfg_entries[i].start_bus &&
            bus <= mcfg_entries[i].end_bus) {
            last_seg_idx = i;
            return (volatile void *)((uintptr_t)mcfg_mapped_bases[i] +
                                     (((uint32_t)(bus - mcfg_entries[i].start_bus) << 20) |
                                      ((uint32_t)slot << 15) | ((uint32_t)func << 12) |
                                      (offset & 0xFFF)));
        }
    }

    return NULL;
}

uint32_t pci_read32(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    volatile void *addr = pci_get_ecam_addr(seg, bus, slot, func, offset);
    if (addr) { return *(volatile uint32_t *)addr; }
    if (offset > 0xFC) return 0xFFFFFFFF;

    uint64_t flags;
    spinlock_acquire(&pci_legacy_lock, &flags);
    uint32_t address =
        (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    uint32_t data = inl(PCI_CONFIG_DATA);
    spinlock_release(&pci_legacy_lock, flags);
    return data;
}

uint16_t pci_read16(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    volatile void *addr = pci_get_ecam_addr(seg, bus, slot, func, offset);
    if (addr) { return *(volatile uint16_t *)addr; }
    if (offset > 0xFE) return 0xFFFF;

    uint32_t val = pci_read32(seg, bus, slot, func, offset);
    return (uint16_t)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read8(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    volatile void *addr = pci_get_ecam_addr(seg, bus, slot, func, offset);
    if (addr) { return *(volatile uint8_t *)addr; }
    if (offset > 0xFF) return 0xFF;

    uint32_t val = pci_read32(seg, bus, slot, func, offset);
    return (uint8_t)((val >> ((offset & 3) * 8)) & 0xFF);
}

void pci_write32(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset,
                 uint32_t val) {
    volatile void *addr = pci_get_ecam_addr(seg, bus, slot, func, offset);
    if (addr) {
        *(volatile uint32_t *)addr = val;
        pci_mmio_flush(addr);
        return;
    }
    if (offset > 0xFF) return;

    uint64_t flags;
    spinlock_acquire(&pci_legacy_lock, &flags);
    uint32_t address =
        (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, val);
    spinlock_release(&pci_legacy_lock, flags);
}

void pci_write16(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset,
                 uint16_t val) {
    volatile void *addr = pci_get_ecam_addr(seg, bus, slot, func, offset);
    if (addr) {
        *(volatile uint16_t *)addr = val;
        pci_mmio_flush(addr);
        return;
    }
    if (offset > 0xFF) return;

    uint32_t old_val = pci_read32(seg, bus, slot, func, offset);
    uint32_t mask = 0xFFFF << ((offset & 2) * 8);
    uint32_t new_val = (old_val & ~mask) | ((uint32_t)val << ((offset & 2) * 8));
    pci_write32(seg, bus, slot, func, offset, new_val);
}

void pci_write8(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset,
                uint8_t val) {
    volatile void *addr = pci_get_ecam_addr(seg, bus, slot, func, offset);
    if (addr) {
        *(volatile uint8_t *)addr = val;
        pci_mmio_flush(addr);
        return;
    }
    if (offset > 0xFF) return;

    uint32_t old_val = pci_read32(seg, bus, slot, func, offset);
    uint32_t mask = 0xFF << ((offset & 3) * 8);
    uint32_t new_val = (old_val & ~mask) | ((uint32_t)val << ((offset & 3) * 8));
    pci_write32(seg, bus, slot, func, offset, new_val);
}

uint16_t pci_find_capability(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t cap_id) {
    uint16_t status = pci_read16(seg, bus, slot, func, 0x06);
    if (!(status & (1 << 4))) return 0;

    uint8_t cap_ptr = pci_read8(seg, bus, slot, func, 0x34);
    while (cap_ptr != 0) {
        uint8_t id = pci_read8(seg, bus, slot, func, cap_ptr);
        if (id == cap_id) return cap_ptr;
        cap_ptr = pci_read8(seg, bus, slot, func, cap_ptr + 1);
    }
    return 0;
}

uint16_t pci_find_extended_capability(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func,
                                      uint16_t cap_id) {
    uint16_t ptr = 0x100;
    uint32_t header = pci_read32(seg, bus, slot, func, ptr);
    if (header == 0 || header == 0xFFFFFFFF) return 0;

    while (ptr != 0) {
        header = pci_read32(seg, bus, slot, func, ptr);
        if ((header & 0xFFFF) == cap_id) return ptr;
        ptr = (header >> 20) & 0xFFF;
        if (ptr < 0x100) break;
    }
    return 0;
}

void pci_enable_bus_mastering(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t cmd = pci_read16(seg, bus, slot, func, 0x04);
    cmd |= (1 << 2) | (1 << 1);
    pci_write16(seg, bus, slot, func, 0x04, cmd);
}

bool pci_check_and_clear_errors(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t status = pci_read16(seg, bus, slot, func, 0x06);
    if (status & 0xF800) {
        pci_write16(seg, bus, slot, func, 0x06, status & 0xF800);
        return true;
    }
    return false;
}

static void pci_parse_bars(struct pci_device *dev) {
    for (int i = 0; i < 6; i++) {
        uint8_t offset = 0x10 + (i * 4);
        uint32_t old_val = pci_read32(dev->segment, dev->bus, dev->slot, dev->func, offset);

        pci_write32(dev->segment, dev->bus, dev->slot, dev->func, offset, 0xFFFFFFFF);
        uint32_t mask = pci_read32(dev->segment, dev->bus, dev->slot, dev->func, offset);

        pci_write32(dev->segment, dev->bus, dev->slot, dev->func, offset, old_val);

        if (mask == 0) continue;

        if (old_val & 0x1) {  // I/O Space
            dev->bars[i].type = PCI_BAR_TYPE_IO;
            dev->bars[i].base = old_val & ~0x3;
            dev->bars[i].size = ~(mask & ~0x3) + 1;
        } else {  // Memory Space
            uint8_t type = (old_val >> 1) & 0x3;
            dev->bars[i].prefetchable = (old_val >> 3) & 0x1;

            if (type == 0x2) {  // 64-bit MMIO
                uint32_t old_hi =
                    pci_read32(dev->segment, dev->bus, dev->slot, dev->func, offset + 4);
                pci_write32(dev->segment, dev->bus, dev->slot, dev->func, offset + 4, 0xFFFFFFFF);
                uint32_t mask_hi =
                    pci_read32(dev->segment, dev->bus, dev->slot, dev->func, offset + 4);
                pci_write32(dev->segment, dev->bus, dev->slot, dev->func, offset + 4, old_hi);

                dev->bars[i].type = PCI_BAR_TYPE_MMIO64;
                dev->bars[i].base = ((uint64_t)old_hi << 32) | (old_val & ~0xF);
                uint64_t full_mask = ((uint64_t)mask_hi << 32) | (mask & ~0xF);
                dev->bars[i].size = ~full_mask + 1;
                i++;
            } else {  // 32-bit MMIO
                dev->bars[i].type = PCI_BAR_TYPE_MMIO32;
                dev->bars[i].base = old_val & ~0xF;
                dev->bars[i].size = (uint32_t)(~(mask & ~0xF) + 1);
            }
        }
    }
}

static void pci_check_function(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = pci_read16(seg, bus, slot, func, 0);
    if (vendor == 0xFFFF) return;

    struct pci_device *pdev = (struct pci_device *)kmalloc(sizeof(struct pci_device));
    memset(pdev, 0, sizeof(struct pci_device));

    pdev->segment = seg;
    pdev->bus = bus;
    pdev->slot = slot;
    pdev->func = func;
    pdev->vendor_id = vendor;
    pdev->device_id = pci_read16(seg, bus, slot, func, 2);
    pdev->class_code = pci_read8(seg, bus, slot, func, 0x0B);
    pdev->subclass = pci_read8(seg, bus, slot, func, 0x0A);
    pdev->prog_if = pci_read8(seg, bus, slot, func, 0x09);

    pci_parse_bars(pdev);

    struct device *gdev = (struct device *)kmalloc(sizeof(struct device));
    memset(gdev, 0, sizeof(struct device));

    gdev->name = "pci_device";  // TODO: meaningful name
    gdev->bus = &pci_bus_type;
    gdev->bus_data = (void *)pdev;

    device_register(gdev);

    klog(LOG_INFO, "PCI: Device %04x:%04x class %02x:%02x rev %02x\n",
         pdev->vendor_id, pdev->device_id, pdev->class_code, pdev->subclass, pdev->prog_if);
}

void pci_scan_bus(uint16_t seg, uint8_t bus) {
    for (uint8_t s = 0; s < 32; s++) {
        if (pci_read16(seg, bus, s, 0, 0) == 0xFFFF) continue;

        pci_check_function(seg, bus, s, 0);

        if (pci_read8(seg, bus, s, 0, 0x0E) & 0x80) {
            for (uint8_t f = 1; f < 8; f++) { pci_check_function(seg, bus, s, f); }
        }
    }
}

void pci_enumerate() {
    if (mcfg_entry_count > 0) {
        for (size_t i = 0; i < mcfg_entry_count; i++) {
            pci_scan_bus(mcfg_entries[i].pci_segment, mcfg_entries[i].start_bus);
        }
    } else {
        pci_scan_bus(0, 0);
    }
}

static void pci_init_ecam() {
    struct SDTHeader *header = acpi_find_table("MCFG");
    if (!header) return;

    struct MCFGTable *mcfg = (struct MCFGTable *)header;
    mcfg_entry_count = (mcfg->header.length - sizeof(struct MCFGTable)) / sizeof(struct MCFGEntry);
    mcfg_entries = (struct MCFGEntry *)kmalloc(sizeof(struct MCFGEntry) * mcfg_entry_count);
    mcfg_mapped_bases = (void **)kmalloc(sizeof(void *) * mcfg_entry_count);
    memcpy(mcfg_entries, mcfg->entries, sizeof(struct MCFGEntry) * mcfg_entry_count);

    for (size_t i = 0; i < mcfg_entry_count; i++) {
        uint32_t bus_range = mcfg_entries[i].end_bus - mcfg_entries[i].start_bus + 1;
        uint64_t size = (uint64_t)bus_range << 20;
        mcfg_mapped_bases[i] = vmm_mmio_map(mcfg_entries[i].base_address, size);
    }
}

static int pci_module_init() {
    spinlock_init(&pci_legacy_lock);
    bus_register(&pci_bus_type);
    pci_init_ecam();
    pci_enumerate();
    return 0;
}

static void pci_module_exit() {
    if (mcfg_mapped_bases) {
        for (size_t i = 0; i < mcfg_entry_count; i++) {
            uint32_t bus_range = mcfg_entries[i].end_bus - mcfg_entries[i].start_bus + 1;
            vmm_mmio_unmap(mcfg_mapped_bases[i], (uint64_t)bus_range << 20);
        }
        kfree(mcfg_mapped_bases);
        kfree(mcfg_entries);
    }
}

KEXPORT(pci_read8)
KEXPORT(pci_read16)
KEXPORT(pci_read32)
KEXPORT(pci_write8)
KEXPORT(pci_write16)
KEXPORT(pci_write32)
KEXPORT(pci_find_capability)
KEXPORT(pci_find_extended_capability)
KEXPORT(pci_scan_bus)
KEXPORT(pci_enable_bus_mastering)

MODULE_INFO("pci_bus", pci_module_init, pci_module_exit);