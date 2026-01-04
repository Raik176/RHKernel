#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "pci_bus.h"
#include "portio.h"
#include "string.h"
#include "usb_core.h"

#define UHCI_USBCMD 0x00
#define UHCI_USBSTS 0x02
#define UHCI_USBINTR 0x04
#define UHCI_FRNUM 0x06
#define UHCI_FRBASE 0x08
#define UHCI_SOFMOD 0x0C
#define UHCI_PORTSC1 0x10
#define UHCI_PORTSC2 0x12

#define TD_STATUS_ACTIVE (1 << 23)
#define TD_CONTROL_IOC (1 << 24)
#define TD_PACKET_SETUP 0x2D
#define TD_PACKET_IN 0x69
#define TD_PACKET_OUT 0xE1

struct uhci_td {
    uint32_t link;
    uint32_t control;
    uint32_t token;
    uint32_t buffer;
    uint32_t reserved[4];  // Align to 32 bytes
} __attribute__((aligned(16)));

struct uhci_qh {
    uint32_t head;
    uint32_t element;
    uint32_t reserved[6];
} __attribute__((aligned(16)));

struct uhci_private {
    uint16_t io_base;
    uint32_t *frame_list;
    struct uhci_qh *control_qh;
};

// Helper for physical addresses (assuming identity map for simplicity)
static inline uint32_t phys(void *ptr) { return (uint32_t)(uintptr_t)ptr; }

static int uhci_control_msg(struct usb_hcd *hcd, uint8_t addr, uint8_t ep,
                            struct usb_setup_packet *setup, void *data, uint16_t len) {
    struct uhci_private *priv = hcd->priv;

    // 1. Setup Phase TD
    struct uhci_td *td_setup = kmalloc(sizeof(struct uhci_td));
    memset(td_setup, 0, sizeof(struct uhci_td));
    td_setup->control = TD_STATUS_ACTIVE | (3 << 27);  // 3 Retries
    td_setup->token = (7 << 21) | (ep << 15) | (addr << 8) | TD_PACKET_SETUP;
    td_setup->buffer = phys(setup);

    // 2. Data Phase TD (Simplified: assuming len <= 8 for now)
    struct uhci_td *td_data = NULL;
    if (len > 0) {
        td_data = kmalloc(sizeof(struct uhci_td));
        memset(td_data, 0, sizeof(struct uhci_td));
        td_data->control = TD_STATUS_ACTIVE | (3 << 27);
        uint8_t toggle = 1;
        td_data->token = ((len - 1) << 21) | (toggle << 19) | (ep << 15) | (addr << 8) |
                         ((setup->request_type & 0x80) ? TD_PACKET_IN : TD_PACKET_OUT);
        td_data->buffer = phys(data);
        td_setup->link = phys(td_data) | 4;  // Vertical link
    }

    // 3. Status Phase TD
    struct uhci_td *td_status = kmalloc(sizeof(struct uhci_td));
    memset(td_status, 0, sizeof(struct uhci_td));
    td_status->link = 1;  // Terminate
    td_status->control = TD_STATUS_ACTIVE | (3 << 27);
    td_status->token = (0x7FF << 21) | (1 << 19) | (ep << 15) | (addr << 8) |
                       ((setup->request_type & 0x80) ? TD_PACKET_OUT : TD_PACKET_IN);

    if (td_data)
        td_data->link = phys(td_status) | 4;
    else
        td_setup->link = phys(td_status) | 4;

    // 4. Attach to QH
    priv->control_qh->element = phys(td_setup);

    // 5. Poll for completion
    int timeout = 1000000;
    while ((td_status->control & TD_STATUS_ACTIVE) && timeout--) {
        for (volatile int i = 0; i < 100; i++);
    }

    int result = (timeout > 0) ? 0 : -1;

    // Cleanup
    priv->control_qh->element = 1;
    kfree(td_setup);
    if (td_data) kfree(td_data);
    kfree(td_status);

    return result;
}

static int uhci_probe(struct device *dev) {
    struct pci_device *pdev = (struct pci_device *)dev->bus_data;
    uint16_t io_base = 0;

    // Find I/O BAR (usually BAR4)
    for (int i = 0; i < 6; i++) {
        if (pdev->bars[i].type == PCI_BAR_TYPE_IO) {
            io_base = pdev->bars[i].base;
            break;
        }
    }

    if (io_base == 0) return -1;
    klog(LOG_INFO, "UHCI: Controller found at IO %04x\n", io_base);

    struct uhci_private *priv = kmalloc(sizeof(struct uhci_private));
    priv->io_base = io_base;

    // 1. Global Reset
    outw(io_base + UHCI_USBCMD, 0x0004);
    for (volatile int i = 0; i < 100000; i++);
    outw(io_base + UHCI_USBCMD, 0x0000);

    // 2. Setup Frame List
    priv->frame_list = kmalloc(4096 + 4096);  // Ensure 4KB align
    priv->frame_list = (uint32_t *)(((uintptr_t)priv->frame_list + 4095) & ~4095);

    priv->control_qh = kmalloc(sizeof(struct uhci_qh));
    memset(priv->control_qh, 0, sizeof(struct uhci_qh));
    priv->control_qh->head = 1;  // Terminate
    priv->control_qh->element = 1;

    for (int i = 0; i < 1024; i++) {
        priv->frame_list[i] = phys(priv->control_qh) | 2;  // QH link
    }

    // 3. Set Hardware Registers
    outl(io_base + UHCI_FRBASE, phys(priv->frame_list));
    outw(io_base + UHCI_FRNUM, 0);
    outw(io_base + UHCI_USBCMD, 0x0001);  // Run

    struct usb_hcd *hcd = kmalloc(sizeof(struct usb_hcd));
    hcd->pci_dev = dev;
    hcd->priv = priv;
    hcd->control_msg = uhci_control_msg;

    usb_register_hcd(hcd);

    // 4. Root Hub Polling
    for (int p = 0; p < 2; p++) {
        uint16_t port_reg = io_base + UHCI_PORTSC1 + (p * 2);
        uint16_t status = inw(port_reg);
        if (status & 0x0001) {  // Current Connect Status
            klog(LOG_INFO, "UHCI: Device detected on port %d, resetting...\n", p);
            outw(port_reg, status | 0x0200);  // Port Reset
            for (volatile int i = 0; i < 1000000; i++);
            outw(port_reg, status & ~0x0200);
            for (volatile int i = 0; i < 1000000; i++);
            outw(port_reg, inw(port_reg) | 0x0004);  // Enable Port

            usb_enumerate_device(hcd, p);
        }
    }

    return 0;
}

static struct pci_device_id uhci_ids[] = {{PCI_ANY_ID, PCI_ANY_ID, 0x0C, 0x03, 0x00},  // UHCI
                                          {0}};

static struct driver uhci_driver = {.name = "uhci_hcd", .id_table = uhci_ids, .probe = uhci_probe};

static int uhci_init() {
    uhci_driver.bus = find_bus("pci");
    driver_register(&uhci_driver);
    return 0;
}

MODULE_INFO("uhci", uhci_init, NULL);