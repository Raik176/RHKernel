#include "usb_core.h"
#include "kbd_core.h"

#include "input.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "string.h"

#define HID_BOOT_REPORT_LEN 8u

struct hid_kbd {
    struct usb_interrupt_pipe pipe;
    struct kbd_device *kbd;
    uint8_t *buffer;
    uint8_t last_report[HID_BOOT_REPORT_LEN];
};

static int report_has_key(const uint8_t *rep, uint8_t key) {
    for (uint32_t i = 2; i < HID_BOOT_REPORT_LEN; i++) if (rep[i] == key) return 1;
    return 0;
}

static uint32_t usage_to_set1(uint8_t usage) {
    switch (usage) {
        case 0x04: return 0x1e;
        case 0x05: return 0x30;
        case 0x06: return 0x2e;
        case 0x07: return 0x20;
        case 0x08: return 0x12;
        case 0x09: return 0x21;
        case 0x0a: return 0x22;
        case 0x0b: return 0x23;
        case 0x0c: return 0x17;
        case 0x0d: return 0x24;
        case 0x0e: return 0x25;
        case 0x0f: return 0x26;
        case 0x10: return 0x32;
        case 0x11: return 0x31;
        case 0x12: return 0x18;
        case 0x13: return 0x19;
        case 0x14: return 0x10;
        case 0x15: return 0x13;
        case 0x16: return 0x1f;
        case 0x17: return 0x14;
        case 0x18: return 0x16;
        case 0x19: return 0x2f;
        case 0x1a: return 0x11;
        case 0x1b: return 0x2d;
        case 0x1c: return 0x15;
        case 0x1d: return 0x2c;
        case 0x1e: return 0x02;
        case 0x1f: return 0x03;
        case 0x20: return 0x04;
        case 0x21: return 0x05;
        case 0x22: return 0x06;
        case 0x23: return 0x07;
        case 0x24: return 0x08;
        case 0x25: return 0x09;
        case 0x26: return 0x0a;
        case 0x27: return 0x0b;
        case 0x28: return 0x1c;
        case 0x29: return 0x01;
        case 0x2a: return 0x0e;
        case 0x2b: return 0x0f;
        case 0x2c: return 0x39;
        case 0x2d: return 0x0c;
        case 0x2e: return 0x0d;
        case 0x2f: return 0x1a;
        case 0x30: return 0x1b;
        case 0x31: return 0x2b;
        case 0x33: return 0x27;
        case 0x34: return 0x28;
        case 0x35: return 0x29;
        case 0x36: return 0x33;
        case 0x37: return 0x34;
        case 0x38: return 0x35;
        default: return 0x10000u | usage;
    }
}

static uint32_t mod_to_set1(uint32_t bit) {
    switch (bit) {
        case 0: return 0x1d;
        case 1: return 0x2a;
        case 2: return 0x38;
        case 4: return INPUT_KEY_EXTENDED | 0x1d;
        case 5: return 0x36;
        case 6: return INPUT_KEY_EXTENDED | 0x38;
        default: return 0x10000u | (0x80u + bit);
    }
}

static void hid_keyboard_callback(struct usb_interrupt_pipe *pipe, const void *data, uint32_t len,
                                  int status, void *cookie) {
    struct hid_kbd *kbd = (struct hid_kbd *)cookie;
    if (!pipe || !kbd || !kbd->kbd || status != 0) return;

    uint8_t short_report[HID_BOOT_REPORT_LEN];
    const uint8_t *rep = (const uint8_t *)data;
    if (!rep) len = 0;
    if (len < HID_BOOT_REPORT_LEN) {
        memset(short_report, 0, sizeof(short_report));
        if (rep && len) memcpy(short_report, rep, len);
        rep = short_report;
    }

    if (rep[2] == 1 && rep[3] == 1 && rep[4] == 1 && rep[5] == 1 && rep[6] == 1 && rep[7] == 1) return;

    for (uint32_t bit = 0; bit < 8; bit++) {
        uint8_t mask = (uint8_t)(1u << bit);
        uint8_t now = rep[0] & mask;
        uint8_t old = kbd->last_report[0] & mask;
        if (now != old) kbd_handle_key(kbd->kbd, mod_to_set1(bit), now ? 1 : 0, 0);
    }

    for (uint32_t i = 2; i < HID_BOOT_REPORT_LEN; i++) {
        uint8_t old = kbd->last_report[i];
        if (old && !report_has_key(rep, old)) kbd_handle_key(kbd->kbd, usage_to_set1(old), 0, 0);
    }
    for (uint32_t i = 2; i < HID_BOOT_REPORT_LEN; i++) {
        uint8_t key = rep[i];
        if (key && !report_has_key(kbd->last_report, key)) kbd_handle_key(kbd->kbd, usage_to_set1(key), 1, 0);
    }
    memcpy(kbd->last_report, rep, HID_BOOT_REPORT_LEN);
}

static const struct usb_endpoint_descriptor *find_intr_in(const struct usb_interface *intf) {
    for (uint32_t i = 0; i < intf->endpoint_count; i++) {
        const struct usb_endpoint_descriptor *ep = &intf->endpoints[i];
        if ((ep->bEndpointAddress & USB_DIR_IN) && ((ep->bmAttributes & 3u) == USB_ENDPOINT_XFER_INTR)) return ep;
    }
    return NULL;
}

static int hid_match(struct usb_interface *intf) {
    if (!intf) return 0;
    return intf->desc.bInterfaceClass == USB_CLASS_HID &&
           intf->desc.bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
           intf->desc.bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD &&
           find_intr_in(intf) != NULL;
}

static int hid_bind(struct usb_interface *intf) {
    const struct usb_endpoint_descriptor *ep = find_intr_in(intf);
    if (!intf || !intf->dev || !ep) return -1;

    struct hid_kbd *kbd = (struct hid_kbd *)kzalloc(sizeof(*kbd));
    if (!kbd) return -1;
    kbd->buffer = (uint8_t *)kmalloc(HID_BOOT_REPORT_LEN);
    if (!kbd->buffer) goto fail;

    char name[32];
    snprintf(name, sizeof(name), "usb-kbd%u.%u", intf->dev->id, intf->desc.bInterfaceNumber);
    kbd->kbd = kbd_register(name);
    if (!kbd->kbd) goto fail;

    (void)usb_control_transfer(intf->dev, USB_DIR_OUT | 0x21u, 0x0Au, 0, intf->desc.bInterfaceNumber, NULL, 0, 1000);
    (void)usb_control_transfer(intf->dev, USB_DIR_OUT | 0x21u, 0x0Bu, 0, intf->desc.bInterfaceNumber, NULL, 0, 1000);

    kbd->pipe.hcd = intf->dev->hcd;
    kbd->pipe.dev = intf->dev;
    kbd->pipe.endpoint = ep->bEndpointAddress;
    kbd->pipe.max_packet = ep->wMaxPacketSize & 0x7ffu;
    if (kbd->pipe.max_packet > HID_BOOT_REPORT_LEN) kbd->pipe.max_packet = HID_BOOT_REPORT_LEN;
    if (kbd->pipe.max_packet == 0) kbd->pipe.max_packet = HID_BOOT_REPORT_LEN;
    kbd->pipe.interval = ep->bInterval ? ep->bInterval : 8;
    kbd->pipe.buffer = kbd->buffer;
    kbd->pipe.buffer_len = HID_BOOT_REPORT_LEN;
    kbd->pipe.callback = hid_keyboard_callback;
    kbd->pipe.callback_priv = kbd;

    if (usb_register_interrupt_pipe(&kbd->pipe) != 0) goto fail;
    intf->driver_private = kbd;
    klog(LOG_INFO, "usb_hid_kbd: keyboard dev=%u iface=%u ep=%02x\n",
         intf->dev->id, intf->desc.bInterfaceNumber, ep->bEndpointAddress);
    return 0;

fail:
    if (kbd->kbd) kbd_unregister(kbd->kbd);
    if (kbd->buffer) kfree(kbd->buffer);
    kfree(kbd);
    return -1;
}

static void hid_unbind(struct usb_interface *intf) {
    if (!intf || !intf->driver_private) return;
    struct hid_kbd *kbd = (struct hid_kbd *)intf->driver_private;
    usb_unregister_interrupt_pipe(&kbd->pipe);
    if (kbd->kbd) kbd_unregister(kbd->kbd);
    if (kbd->buffer) kfree(kbd->buffer);
    kfree(kbd);
    intf->driver_private = NULL;
}

static struct usb_driver hid_driver = {
    .name = "usb_hid_kbd",
    .match = hid_match,
    .bind = hid_bind,
    .unbind = hid_unbind,
    .next = NULL,
};

static int usb_hid_kbd_init(void) {
    return usb_driver_register(&hid_driver);
}

static void usb_hid_kbd_exit(void) {
    usb_driver_unregister(&hid_driver);
}

MODULE_INFO("usb_hid_kbd", usb_hid_kbd_init, 0, usb_hid_kbd_exit, "usb_core", "kbd_core");
