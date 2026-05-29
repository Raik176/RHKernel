#include "mod/device.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "pci_bus.h"

#define PCI_BRIDGE_PRIMARY_BUS 0x18
#define PCI_BRIDGE_SECONDARY_BUS 0x19
#define PCI_BRIDGE_SUBORDINATE_BUS 0x1A
#define PCI_BRIDGE_CONTROL 0x3E

// ID Table: Matches Class 06 (Bridges), Subclass 04 (PCI-to-PCI)
static struct pci_device_id bridge_ids[] = {{PCI_ANY_ID, PCI_ANY_ID, 0x06, 0x04, PCI_ANY_CLASS},
                                            {0}};

int pci_bridge_probe(struct device *dev) {
    struct pci_device *pdev = (struct pci_device *)dev->bus_data;

    uint8_t secondary_bus =
        pci_read8(pdev->segment, pdev->bus, pdev->slot, pdev->func, PCI_BRIDGE_SECONDARY_BUS);
    if (secondary_bus == 0) {
        klog(LOG_WARN, "PCI: Bridge %02x:%02x.%d has no secondary bus assigned!\n", pdev->bus,
             pdev->slot, pdev->func);
        return -1;
    }

    // 2. Enable Bridge Forwarding (I/O, Memory, and Master)
    uint16_t cmd = pci_read16(pdev->segment, pdev->bus, pdev->slot, pdev->func, 0x04);
    cmd |= (1 << 0) | (1 << 1) | (1 << 2);  // IO, MMIO, Master
    pci_write16(pdev->segment, pdev->bus, pdev->slot, pdev->func, 0x04, cmd);

    pci_scan_bus(pdev->segment, secondary_bus);

    return 0;
}

static struct driver pci_bridge_driver = {
    .name = "pci_bridge_drv", .id_table = bridge_ids, .probe = pci_bridge_probe, .remove = NULL};

static int pci_bridge_init() {
    pci_bridge_driver.bus = find_bus("pci");
    if (!pci_bridge_driver.bus) { return -1; }

    driver_register(&pci_bridge_driver);

    return 0;
}

MODULE_INFO("pci_bridge", pci_bridge_init, 0, NULL);