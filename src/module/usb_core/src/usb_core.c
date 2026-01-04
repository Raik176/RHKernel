#include "usb_core.h"

#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "string.h"
#include "symbol.h"

static uint8_t next_usb_addr = 1;

void usb_register_hcd(struct usb_hcd *hcd) {
    klog(LOG_INFO, "USB: Registered new Host Controller\n");
}

int usb_enumerate_device(struct usb_hcd *hcd, uint8_t port) {
    struct usb_device *udev = kmalloc(sizeof(struct usb_device));
    memset(udev, 0, sizeof(struct usb_device));
    udev->hcd = hcd;
    udev->address = 0;  // Start at default address

    // 1. Get partial device descriptor to find Max Packet Size
    struct usb_setup_packet setup = {.request_type = 0x80,  // Device to Host
                                     .request = USB_REQ_GET_DESCRIPTOR,
                                     .value = (USB_DESC_DEVICE << 8),
                                     .index = 0,
                                     .length = 8};

    if (hcd->control_msg(hcd, 0, 0, &setup, &udev->descriptor, 8) < 0) {
        kfree(udev);
        return -1;
    }

    // 2. Set Address
    uint8_t new_addr = next_usb_addr++;
    setup = (struct usb_setup_packet){.request_type = 0x00,  // Host to Device
                                      .request = USB_REQ_SET_ADDRESS,
                                      .value = new_addr,
                                      .index = 0,
                                      .length = 0};

    if (hcd->control_msg(hcd, 0, 0, &setup, NULL, 0) < 0) {
        klog(LOG_ERR, "USB: Failed to set address\n");
        return -1;
    }
    udev->address = new_addr;

    // Small delay for address to stabilize
    for (volatile int i = 0; i < 100000; i++);

    // 3. Get Full Device Descriptor
    setup.request_type = 0x80;
    setup.request = USB_REQ_GET_DESCRIPTOR;
    setup.value = (USB_DESC_DEVICE << 8);
    setup.length = sizeof(struct usb_device_descriptor);

    hcd->control_msg(hcd, udev->address, 0, &setup, &udev->descriptor, setup.length);

    klog(LOG_INFO, "USB: Device Found! Addr: %d, ID: %04x:%04x, Class: %02x\n", udev->address,
         udev->descriptor.id_vendor, udev->descriptor.id_product, udev->descriptor.device_class);

    return 0;
}

static int usb_core_init() { return 0; }

MODULE_INFO("usb_core", usb_core_init, NULL);

KEXPORT(usb_register_hcd);
KEXPORT(usb_enumerate_device);