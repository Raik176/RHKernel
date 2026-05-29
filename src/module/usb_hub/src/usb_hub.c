#include "usb_core.h"

#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "mod/scheduler.h"
#include "mod/workqueue.h"
#include "string.h"

#define HUB_MAX_PORTS 16u
#define HUB_POLL_TICKS 32u
#define HUB_POWER_DELAY_TICKS 50u
#define HUB_DESC_MIN_LEN 7u
#define HUB_DESC_BUF_LEN 64u

#define HUB_REQ_GET_STATUS    0u
#define HUB_REQ_CLEAR_FEATURE 1u
#define HUB_REQ_SET_FEATURE   3u

#define HUB_FEATURE_PORT_RESET        4u
#define HUB_FEATURE_PORT_POWER        8u
#define HUB_FEATURE_C_PORT_CONNECTION 16u
#define HUB_FEATURE_C_PORT_ENABLE     17u
#define HUB_FEATURE_C_PORT_RESET      20u

#define HUB_PORT_CONNECTION 0x0001u
#define HUB_PORT_ENABLE     0x0002u
#define HUB_PORT_LOW_SPEED  0x0200u

#define HUB_CHANGE_CONNECTION 0x0001u
#define HUB_CHANGE_ENABLE     0x0002u
#define HUB_CHANGE_RESET      0x0010u

struct hub_descriptor {
    uint8_t bDescLength;
    uint8_t bDescriptorType;
    uint8_t bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood;
    uint8_t bHubContrCurrent;
    uint8_t tail[32];
} __attribute__((packed));

struct hub_port_status {
    uint16_t status;
    uint16_t change;
} __attribute__((packed));

struct usb_hub {
    struct usb_device *dev;
    struct usb_interrupt_pipe pipe;
    struct kernel_delayed_work scan_work;
    uint8_t *status_buf;
    uint8_t ports;
    uint8_t status_len;
    uint8_t active;
    uint8_t pipe_registered;
};

static uint8_t hub_status_len(uint8_t ports) {
    uint32_t bits = (uint32_t)ports + 1u;
    uint32_t len = (bits + 7u) >> 3;
    if (len == 0) len = 1;
    if (len > 8u) len = 8u;
    return (uint8_t)len;
}

static const struct usb_endpoint_descriptor *find_intr_in(const struct usb_interface *intf) {
    if (!intf) return NULL;
    for (uint32_t i = 0; i < intf->endpoint_count; i++) {
        const struct usb_endpoint_descriptor *ep = &intf->endpoints[i];
        if ((ep->bEndpointAddress & USB_DIR_IN) && ((ep->bmAttributes & 3u) == USB_ENDPOINT_XFER_INTR)) return ep;
    }
    return NULL;
}

static int hub_get_descriptor(struct usb_device *dev, struct hub_descriptor *desc) {
    if (!dev || !desc) return -1;
    uint8_t raw[HUB_DESC_BUF_LEN];
    memset(raw, 0, sizeof(raw));
    if (usb_control_transfer(dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                             USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DT_HUB << 8), 0,
                             raw, HUB_DESC_MIN_LEN, 1000) != 0) return -1;
    if (raw[0] < HUB_DESC_MIN_LEN || raw[1] != USB_DT_HUB) return -1;
    uint16_t len = raw[0];
    if (len > sizeof(raw)) len = sizeof(raw);
    if (len > HUB_DESC_MIN_LEN) {
        memset(raw, 0, sizeof(raw));
        if (usb_control_transfer(dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                                 USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DT_HUB << 8), 0,
                                 raw, len, 1000) != 0) return -1;
        if (raw[0] < HUB_DESC_MIN_LEN || raw[1] != USB_DT_HUB) return -1;
    }
    memset(desc, 0, sizeof(*desc));
    if (len > sizeof(*desc)) len = sizeof(*desc);
    memcpy(desc, raw, len);
    return desc->bNbrPorts ? 0 : -1;
}

static int hub_get_status(struct usb_hub *hub, uint32_t port, struct hub_port_status *st) {
    if (!hub || !st || port == 0 || port > hub->ports) return -1;
    return usb_control_transfer(hub->dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
                                HUB_REQ_GET_STATUS, 0, (uint16_t)port, st, sizeof(*st), 1000);
}

static void hub_clear_feature(struct usb_hub *hub, uint32_t port, uint16_t feature) {
    if (!hub || port == 0 || port > hub->ports) return;
    (void)usb_control_transfer(hub->dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
                               HUB_REQ_CLEAR_FEATURE, feature, (uint16_t)port, NULL, 0, 1000);
}

static void hub_set_feature(struct usb_hub *hub, uint32_t port, uint16_t feature) {
    if (!hub || port == 0 || port > hub->ports) return;
    (void)usb_control_transfer(hub->dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
                               HUB_REQ_SET_FEATURE, feature, (uint16_t)port, NULL, 0, 1000);
}

static void hub_clear_changes(struct usb_hub *hub, uint32_t port, uint16_t change) {
    if (change & HUB_CHANGE_CONNECTION) hub_clear_feature(hub, port, HUB_FEATURE_C_PORT_CONNECTION);
    if (change & HUB_CHANGE_ENABLE) hub_clear_feature(hub, port, HUB_FEATURE_C_PORT_ENABLE);
    if (change & HUB_CHANGE_RESET) hub_clear_feature(hub, port, HUB_FEATURE_C_PORT_RESET);
}

static int hub_reset_port(struct usb_hub *hub, uint32_t port, uint32_t *speed) {
    if (!speed) return -1;
    hub_set_feature(hub, port, HUB_FEATURE_PORT_RESET);
    kernel_sleep_ticks(50);
    struct hub_port_status st;
    memset(&st, 0, sizeof(st));
    if (hub_get_status(hub, port, &st) != 0) return -1;
    hub_clear_changes(hub, port, st.change);
    if (!(st.status & HUB_PORT_CONNECTION) || !(st.status & HUB_PORT_ENABLE)) return -1;
    *speed = (st.status & HUB_PORT_LOW_SPEED) ? USB_SPEED_LOW : USB_SPEED_FULL;
    return 0;
}

static void hub_scan(struct usb_hub *hub) {
    if (!hub || !hub->active || !hub->dev || hub->dev->disconnected) return;
    for (uint32_t port = 1; port <= hub->ports; port++) {
        struct hub_port_status st;
        memset(&st, 0, sizeof(st));
        if (hub_get_status(hub, port, &st) != 0) continue;
        hub_clear_changes(hub, port, st.change);

        if (!(st.status & HUB_PORT_CONNECTION)) {
            usb_remove_child_devices(hub->dev, port);
            continue;
        }

        uint32_t speed = (st.status & HUB_PORT_LOW_SPEED) ? USB_SPEED_LOW : USB_SPEED_FULL;
        if (!(st.status & HUB_PORT_ENABLE) || (st.change & HUB_CHANGE_CONNECTION) || (st.change & HUB_CHANGE_RESET)) {
            usb_remove_child_devices(hub->dev, port);
            if (hub_reset_port(hub, port, &speed) != 0) continue;
        }
        (void)usb_enumerate_hub_port(hub->dev, port, speed);
    }
}

static void hub_scan_work(void *arg) {
    struct usb_hub *hub = (struct usb_hub *)arg;
    if (!hub || !hub->active) return;
    hub_scan(hub);
    if (hub->active) (void)kernel_queue_delayed_work(&hub->scan_work, HUB_POLL_TICKS);
}

static void hub_intr(struct usb_interrupt_pipe *pipe, const void *data, uint32_t len, int status, void *priv) {
    (void)pipe;
    (void)data;
    (void)len;
    struct usb_hub *hub = (struct usb_hub *)priv;
    if (!hub || !hub->active || status != 0) return;
    (void)kernel_queue_delayed_work(&hub->scan_work, 1);
}

static int hub_match(struct usb_interface *intf) {
    if (!intf || !intf->dev) return 0;
    return intf->desc.bInterfaceClass == USB_CLASS_HUB || intf->dev->desc.bDeviceClass == USB_CLASS_HUB;
}

static void hub_setup_interrupt_pipe(struct usb_hub *hub, const struct usb_endpoint_descriptor *ep) {
    if (!hub || !ep || !hub->status_buf) return;
    uint16_t max_packet = ep->wMaxPacketSize & 0x7ffu;
    if (max_packet == 0 || max_packet > 8) max_packet = 8;
    if (max_packet < hub->status_len) return;
    hub->pipe.hcd = hub->dev->hcd;
    hub->pipe.dev = hub->dev;
    hub->pipe.endpoint = ep->bEndpointAddress;
    hub->pipe.max_packet = max_packet;
    hub->pipe.interval = ep->bInterval ? ep->bInterval : 16;
    hub->pipe.buffer = hub->status_buf;
    hub->pipe.buffer_len = hub->status_len;
    hub->pipe.callback = hub_intr;
    hub->pipe.callback_priv = hub;
    if (usb_register_interrupt_pipe(&hub->pipe) == 0) hub->pipe_registered = 1;
}

static int hub_bind(struct usb_interface *intf) {
    if (!intf || !intf->dev) return -1;

    struct hub_descriptor desc;
    if (hub_get_descriptor(intf->dev, &desc) != 0) return -1;

    struct usb_hub *hub = (struct usb_hub *)kzalloc(sizeof(*hub));
    if (!hub) return -1;
    hub->dev = intf->dev;
    hub->ports = desc.bNbrPorts > HUB_MAX_PORTS ? HUB_MAX_PORTS : desc.bNbrPorts;
    hub->status_len = hub_status_len(hub->ports);
    hub->status_buf = (uint8_t *)kmalloc(hub->status_len);
    if (!hub->status_buf) goto fail;
    memset(hub->status_buf, 0, hub->status_len);
    hub->active = 1;
    kernel_delayed_work_init(&hub->scan_work, hub_scan_work, hub);

    for (uint32_t port = 1; port <= hub->ports; port++) hub_set_feature(hub, port, HUB_FEATURE_PORT_POWER);
    kernel_sleep_ticks(desc.bPwrOn2PwrGood ? desc.bPwrOn2PwrGood * 2u : HUB_POWER_DELAY_TICKS);

    hub_setup_interrupt_pipe(hub, find_intr_in(intf));
    intf->driver_private = hub;
    hub_scan(hub);
    (void)kernel_queue_delayed_work(&hub->scan_work, HUB_POLL_TICKS);
    klog(LOG_INFO, "usb_hub: dev=%u ports=%u%s\n", intf->dev->id, hub->ports,
         hub->pipe_registered ? "" : " polling");
    return 0;

fail:
    if (hub) {
        if (hub->status_buf) kfree(hub->status_buf);
        kfree(hub);
    }
    return -1;
}

static void hub_unbind(struct usb_interface *intf) {
    if (!intf || !intf->driver_private) return;
    struct usb_hub *hub = (struct usb_hub *)intf->driver_private;
    hub->active = 0;
    kernel_cancel_delayed_work(&hub->scan_work);
    if (hub->pipe_registered) usb_unregister_interrupt_pipe(&hub->pipe);
    usb_remove_child_devices(hub->dev, 0);
    if (hub->status_buf) kfree(hub->status_buf);
    kfree(hub);
    intf->driver_private = NULL;
}

static struct usb_driver hub_driver = {
    .name = "usb_hub",
    .match = hub_match,
    .bind = hub_bind,
    .unbind = hub_unbind,
    .next = NULL,
};

static int usb_hub_init(void) { return usb_driver_register(&hub_driver); }
static void usb_hub_exit(void) { usb_driver_unregister(&hub_driver); }

MODULE_INFO("usb_hub", usb_hub_init, 0, usb_hub_exit, "usb_core");
