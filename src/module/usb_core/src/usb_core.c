#include "usb_core.h"

#include "mod/device.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "mod/scheduler.h"
#include "mod/workqueue.h"
#include "portio.h"
#include "smp/lock.h"
#include "string.h"
#include "symbol.h"

#define USB_MAX_CONFIG 1024u
#define USB_MAX_PACKET0_DEFAULT 8u
#define USB_POLL_TICKS 8u
#define USB_ENUM_DELAY_TICKS 8u
#define USB_TEXT_MAX 4096u

#define USB_TEXT_DEVICES 1u
#define USB_TEXT_HCDS 2u

static void usb_delay_ticks(uint64_t ticks) {
    if (ticks == 0) return;
    if (kernel_sleep_ticks(ticks) == 0) return;
    for (uint64_t i = 0; i < ticks * 50000u; i++) io_wait();
}

static spinlock_t usb_lock;
static struct usb_hcd *hcds;
static struct usb_device *devices;
static struct usb_interrupt_pipe *pipes;
static struct usb_driver *drivers;
static struct kernel_delayed_work poll_work;
static uint32_t next_device_id = 1;
static uint32_t next_address = 1;
static uint32_t next_hcd_index = 0;
static int poll_started;

static void usb_hcd_scan_work(void *arg);
static void release_address_locked(struct usb_hcd *hcd, uint8_t addr);

struct usb_text_ctx {
    uint32_t kind;
    uint32_t len;
    uint32_t pos;
    char buf[USB_TEXT_MAX];
};

static uint16_t le16(uint16_t v) { return v; }
static uint16_t rd16(uint16_t v) { return v; }

int usb_control_transfer(struct usb_device *dev, uint8_t type, uint8_t req, uint16_t value,
                         uint16_t index, void *data, uint16_t len, uint32_t timeout_ms) {
    if (!dev || dev->disconnected || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->control) return -1;
    if (len && !data) return -1;
    struct usb_setup_packet setup;
    setup.bmRequestType = type;
    setup.bRequest = req;
    setup.wValue = le16(value);
    setup.wIndex = le16(index);
    setup.wLength = le16(len);
    return dev->hcd->ops->control(dev->hcd, dev->address, dev->speed, 0, dev->max_packet0,
                                  &setup, data, len, timeout_ms ? timeout_ms : 1000);
}

int usb_bulk_transfer(struct usb_device *dev, uint8_t endpoint, uint16_t max_packet,
                      void *data, uint32_t len, uint32_t *actual, uint32_t timeout_ms) {
    if (actual) *actual = 0;
    if (!dev || dev->disconnected || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->bulk) return -1;
    if (len && !data) return -1;
    if (max_packet == 0) return -1;
    return dev->hcd->ops->bulk(dev->hcd, dev, endpoint, max_packet, data, len, actual, timeout_ms ? timeout_ms : 1000);
}

int usb_isochronous_transfer(struct usb_device *dev, uint8_t endpoint, uint16_t max_packet,
                             void *data, uint32_t len, uint32_t *actual) {
    if (actual) *actual = 0;
    if (!dev || dev->disconnected || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->isochronous) return -1;
    if (len && !data) return -1;
    if (max_packet == 0) return -1;
    return dev->hcd->ops->isochronous(dev->hcd, dev, endpoint, max_packet, data, len, actual);
}

struct usb_intr_once_ctx {
    uint32_t actual;
    int status;
};

static void usb_intr_once_cb(struct usb_interrupt_pipe *pipe, const void *data,
                             uint32_t len, int status, void *priv) {
    (void)pipe;
    (void)data;
    struct usb_intr_once_ctx *ctx = (struct usb_intr_once_ctx *)priv;
    if (!ctx) return;
    ctx->actual = len;
    ctx->status = status;
}

int usb_interrupt_transfer(struct usb_device *dev, uint8_t endpoint, uint16_t max_packet,
                           void *data, uint16_t len, uint32_t *actual, uint32_t timeout_ms) {
    if (actual) *actual = 0;
    if (!dev || dev->disconnected || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->interrupt_in) return -1;
    if (!data || len == 0 || max_packet == 0) return -1;
    struct usb_intr_once_ctx ctx;
    struct usb_interrupt_pipe pipe;
    memset(&ctx, 0, sizeof(ctx));
    memset(&pipe, 0, sizeof(pipe));
    ctx.status = -1;
    pipe.hcd = dev->hcd;
    pipe.dev = dev;
    pipe.endpoint = endpoint;
    pipe.max_packet = max_packet;
    pipe.interval = 1;
    pipe.buffer = (uint8_t *)data;
    pipe.buffer_len = len;
    pipe.callback = usb_intr_once_cb;
    pipe.callback_priv = &ctx;
    pipe.active = 1;
    uint64_t deadline = kernel_monotonic_ticks() + (timeout_ms ? timeout_ms : 1000u);
    for (;;) {
        ctx.actual = 0;
        ctx.status = -1;
        int r = dev->hcd->ops->interrupt_in(&pipe);
        if (r == 0 && ctx.status == 0) {
            if (actual) *actual = ctx.actual;
            if (pipe.hcd && pipe.hcd->ops && pipe.hcd->ops->destroy_pipe) pipe.hcd->ops->destroy_pipe(&pipe);
            return 0;
        }
        if (r != -2 || (int64_t)(kernel_monotonic_ticks() - deadline) >= 0) {
            if (pipe.hcd && pipe.hcd->ops && pipe.hcd->ops->destroy_pipe) pipe.hcd->ops->destroy_pipe(&pipe);
            return -1;
        }
        if (kernel_can_sleep()) (void)kernel_sleep_ticks(1);
        else kernel_yield();
    }
}

static int get_descriptor(struct usb_device *dev, uint8_t type, uint8_t index, void *buf, uint16_t len) {
    return usb_control_transfer(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                                (uint16_t)(((uint16_t)type << 8) | index), 0, buf, len, 1000);
}

static int set_address(struct usb_device *dev, uint8_t addr) {
    int r = usb_control_transfer(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                                 USB_REQ_SET_ADDRESS, addr, 0, NULL, 0, 1000);
    if (r == 0) {
        usb_delay_ticks(2);
        dev->address = addr;
    }
    return r;
}

static int set_configuration(struct usb_device *dev, uint8_t cfg) {
    int r = usb_control_transfer(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                                 USB_REQ_SET_CONFIGURATION, cfg, 0, NULL, 0, 1000);
    if (r == 0) dev->config_value = cfg;
    return r;
}

static void free_device(struct usb_device *dev) {
    if (!dev) return;
    dev->disconnected = 1;
    for (uint32_t i = 0; i < dev->interface_count; i++) {
        struct usb_interface *intf = &dev->interfaces[i];
        if (intf->driver && intf->driver->unbind) intf->driver->unbind(intf);
        intf->driver = NULL;
        intf->driver_private = NULL;
    }
    kfree(dev);
}

static void unlink_device_locked(struct usb_device *dev) {
    struct usb_device **pp = &devices;
    while (*pp) {
        if (*pp == dev) {
            *pp = dev->next;
            release_address_locked(dev->hcd, dev->address);
            dev->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void remove_device_tree(struct usb_device *dev) {
    if (!dev) return;
    for (;;) {
        struct usb_device *child = NULL;
        uint64_t flags;
        spinlock_acquire(&usb_lock, &flags);
        for (struct usb_device *d = devices; d; d = d->next) {
            if (d->parent == dev) {
                child = d;
                unlink_device_locked(d);
                d->disconnected = 1;
                break;
            }
        }
        spinlock_release(&usb_lock, flags);
        if (!child) break;
        remove_device_tree(child);
        free_device(child);
    }
}

void usb_remove_child_devices(struct usb_device *parent, uint32_t hub_port) {
    if (!parent) return;
    for (;;) {
        struct usb_device *victim = NULL;
        uint64_t flags;
        spinlock_acquire(&usb_lock, &flags);
        for (struct usb_device *d = devices; d; d = d->next) {
            if (d->parent == parent && (hub_port == 0 || d->hub_port == hub_port)) {
                victim = d;
                unlink_device_locked(d);
                d->disconnected = 1;
                break;
            }
        }
        spinlock_release(&usb_lock, flags);
        if (!victim) return;
        uint32_t old_id = victim->id;
        remove_device_tree(victim);
        free_device(victim);
        klog(LOG_INFO, "USB: removed child dev=%u\n", old_id);
    }
}

static int bind_interface(struct usb_interface *intf) {
    if (!intf) return -1;
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    if (intf->driver) {
        spinlock_release(&usb_lock, flags);
        return 0;
    }
    struct usb_driver *match = NULL;
    for (struct usb_driver *drv = drivers; drv; drv = drv->next) {
        if (drv->match && drv->bind && drv->match(intf)) {
            match = drv;
            intf->driver = drv;
            break;
        }
    }
    spinlock_release(&usb_lock, flags);
    if (!match) return -1;

    if (match->bind(intf) == 0) {
        klog(LOG_INFO, "USB: bound %s to dev=%u iface=%u\n",
             match->name ? match->name : "driver", intf->dev->id, intf->desc.bInterfaceNumber);
        return 0;
    }

    spinlock_acquire(&usb_lock, &flags);
    if (intf->driver == match) {
        intf->driver = NULL;
        intf->driver_private = NULL;
    }
    spinlock_release(&usb_lock, flags);
    return -1;
}

static void bind_device_interfaces(struct usb_device *dev) {
    for (uint32_t i = 0; dev && i < dev->interface_count; i++) bind_interface(&dev->interfaces[i]);
}

static int append_extra(struct usb_interface *intf, const uint8_t *desc, uint8_t len) {
    if (!intf || !desc || len == 0) return 0;
    if (len > USB_MAX_INTERFACE_EXTRA - intf->extra_len) return -1;
    memcpy(intf->extra + intf->extra_len, desc, len);
    intf->extra_len = (uint16_t)(intf->extra_len + len);
    return 0;
}

static int endpoint_descriptor_valid(const struct usb_endpoint_descriptor *ep) {
    uint8_t type = ep->bmAttributes & 3u;
    uint16_t max_packet = rd16(ep->wMaxPacketSize) & 0x07ffu;
    uint8_t num = ep->bEndpointAddress & 0x0fu;
    if (num == 0 || max_packet == 0) return 0;
    if (type == USB_ENDPOINT_XFER_CONTROL) return 0;
    return 1;
}

static int parse_config(struct usb_device *dev, const uint8_t *cfg, uint16_t len) {
    if (!dev || !cfg || len < sizeof(struct usb_config_descriptor)) return -1;
    const struct usb_config_descriptor *head = (const struct usb_config_descriptor *)cfg;
    if (head->bLength < sizeof(*head) || head->bDescriptorType != USB_DT_CONFIGURATION ||
        rd16(head->wTotalLength) != len) return -1;

    struct usb_interface *cur = NULL;
    uint32_t seen_interfaces = 0;
    dev->interface_count = 0;
    for (uint16_t off = 0; off + 2u <= len;) {
        uint8_t blen = cfg[off];
        uint8_t dtype = cfg[off + 1];
        if (blen < 2u || (uint32_t)off + blen > len) return -1;
        if (dtype == USB_DT_CONFIGURATION) {
            if (off != 0 || blen < sizeof(struct usb_config_descriptor)) return -1;
        } else if (dtype == USB_DT_INTERFACE) {
            const struct usb_interface_descriptor *desc = (const struct usb_interface_descriptor *)(cfg + off);
            cur = NULL;
            if (desc->bNumEndpoints > USB_MAX_ENDPOINTS_PER_INTERFACE) return -1;
            if (desc->bAlternateSetting != 0) {
                off = (uint16_t)(off + blen);
                continue;
            }
            uint32_t bit = 0;
            if (desc->bInterfaceNumber < 32) {
                bit = 1u << desc->bInterfaceNumber;
                if (seen_interfaces & bit) return -1;
            } else {
                for (uint32_t i = 0; i < dev->interface_count; i++)
                    if (dev->interfaces[i].desc.bInterfaceNumber == desc->bInterfaceNumber) return -1;
            }
            if (dev->interface_count >= head->bNumInterfaces || dev->interface_count >= USB_MAX_INTERFACES) return -1;
            cur = &dev->interfaces[dev->interface_count++];
            memset(cur, 0, sizeof(*cur));
            cur->dev = dev;
            cur->desc = *desc;
            seen_interfaces |= bit;
        } else if (dtype == USB_DT_ENDPOINT) {
            if (!cur || blen < sizeof(struct usb_endpoint_descriptor)) return -1;
            if (cur->endpoint_count >= cur->desc.bNumEndpoints ||
                cur->endpoint_count >= USB_MAX_ENDPOINTS_PER_INTERFACE) return -1;
            const struct usb_endpoint_descriptor *ep = (const struct usb_endpoint_descriptor *)(cfg + off);
            if (!endpoint_descriptor_valid(ep)) return -1;
            cur->endpoints[cur->endpoint_count++] = *ep;
        } else if (cur) {
            if (append_extra(cur, cfg + off, blen) != 0) return -1;
        }
        off = (uint16_t)(off + blen);
    }
    if (dev->interface_count != head->bNumInterfaces) return -1;
    for (uint32_t i = 0; i < dev->interface_count; i++) {
        if (dev->interfaces[i].endpoint_count != dev->interfaces[i].desc.bNumEndpoints) return -1;
    }
    return 0;
}

static int root_device_exists_locked(struct usb_hcd *hcd, uint32_t port) {
    for (struct usb_device *d = devices; d; d = d->next)
        if (!d->parent && d->hcd == hcd && d->root_port == port) return 1;
    return 0;
}

static int child_device_exists_locked(struct usb_device *hub, uint32_t port) {
    for (struct usb_device *d = devices; d; d = d->next)
        if (d->parent == hub && d->hub_port == port) return 1;
    return 0;
}

static uint32_t usb_addr_word(uint8_t addr) { return (uint32_t)(addr >> 5); }
static uint32_t usb_addr_bit(uint8_t addr) { return 1u << (addr & 31u); }

static int address_reserved_locked(struct usb_hcd *hcd, uint8_t addr) {
    if (!hcd || addr == 0 || addr > 127u) return 1;
    return (hcd->address_bitmap[usb_addr_word(addr)] & usb_addr_bit(addr)) != 0;
}

static uint8_t reserve_address_locked(struct usb_hcd *hcd) {
    for (uint32_t tries = 0; tries < 127u; tries++) {
        uint8_t addr = (uint8_t)next_address++;
        if (next_address > 127u) next_address = 1u;
        if (addr == 0) addr = 1u;
        if (address_reserved_locked(hcd, addr)) continue;
        hcd->address_bitmap[usb_addr_word(addr)] |= usb_addr_bit(addr);
        return addr;
    }
    return 0;
}

static void release_address_locked(struct usb_hcd *hcd, uint8_t addr) {
    if (!hcd || addr == 0 || addr > 127u) return;
    hcd->address_bitmap[usb_addr_word(addr)] &= ~usb_addr_bit(addr);
}

static int enumerate_device(struct usb_hcd *hcd, uint32_t hcd_index, uint32_t root_port,
                            struct usb_device *parent, uint32_t hub_port, uint32_t speed) {
    if (!hcd || !hcd->ops || !hcd->ops->control) return -1;
    uint32_t dev_id;
    uint8_t addr;
    uint64_t alloc_flags;
    spinlock_acquire(&usb_lock, &alloc_flags);
    addr = reserve_address_locked(hcd);
    dev_id = next_device_id++;
    spinlock_release(&usb_lock, alloc_flags);
    if (addr == 0) return -1;

    struct usb_device *dev = (struct usb_device *)kzalloc(sizeof(*dev));
    if (!dev) {
        uint64_t rf;
        spinlock_acquire(&usb_lock, &rf);
        release_address_locked(hcd, addr);
        spinlock_release(&usb_lock, rf);
        return -1;
    }
    dev->id = dev_id;
    dev->hcd = hcd;
    dev->hcd_index = hcd_index;
    dev->root_port = root_port;
    dev->parent = parent;
    dev->hub_port = hub_port;
    dev->speed = speed;
    dev->address = 0;
    dev->max_packet0 = USB_MAX_PACKET0_DEFAULT;

    struct usb_device_descriptor d8;
    memset(&d8, 0, sizeof(d8));
    if (get_descriptor(dev, USB_DT_DEVICE, 0, &d8, 8) != 0 || d8.bLength < 8 || d8.bDescriptorType != USB_DT_DEVICE) {
        free_device(dev);
        uint64_t rf;
        spinlock_acquire(&usb_lock, &rf);
        release_address_locked(hcd, addr);
        spinlock_release(&usb_lock, rf);
        return -1;
    }
    dev->max_packet0 = d8.bMaxPacketSize0 ? d8.bMaxPacketSize0 : USB_MAX_PACKET0_DEFAULT;
    if (dev->max_packet0 != 8 && dev->max_packet0 != 16 && dev->max_packet0 != 32 && dev->max_packet0 != 64)
        dev->max_packet0 = USB_MAX_PACKET0_DEFAULT;

    if (set_address(dev, addr) != 0) {
        free_device(dev);
        uint64_t rf;
        spinlock_acquire(&usb_lock, &rf);
        release_address_locked(hcd, addr);
        spinlock_release(&usb_lock, rf);
        return -1;
    }
    if (get_descriptor(dev, USB_DT_DEVICE, 0, &dev->desc, sizeof(dev->desc)) != 0 ||
        dev->desc.bLength < sizeof(dev->desc) || dev->desc.bDescriptorType != USB_DT_DEVICE) {
        free_device(dev);
        uint64_t rf;
        spinlock_acquire(&usb_lock, &rf);
        release_address_locked(hcd, addr);
        spinlock_release(&usb_lock, rf);
        return -1;
    }

    struct usb_config_descriptor cd;
    memset(&cd, 0, sizeof(cd));
    if (get_descriptor(dev, USB_DT_CONFIGURATION, 0, &cd, sizeof(cd)) != 0 ||
        cd.bLength < sizeof(cd) || cd.bDescriptorType != USB_DT_CONFIGURATION ||
        rd16(cd.wTotalLength) < sizeof(cd) || rd16(cd.wTotalLength) > USB_MAX_CONFIG) {
        free_device(dev);
        uint64_t rf;
        spinlock_acquire(&usb_lock, &rf);
        release_address_locked(hcd, addr);
        spinlock_release(&usb_lock, rf);
        return -1;
    }

    uint16_t cfg_len = rd16(cd.wTotalLength);
    uint8_t *cfg = (uint8_t *)kmalloc(cfg_len);
    if (!cfg) {
        free_device(dev);
        uint64_t rf;
        spinlock_acquire(&usb_lock, &rf);
        release_address_locked(hcd, addr);
        spinlock_release(&usb_lock, rf);
        return -1;
    }
    if (get_descriptor(dev, USB_DT_CONFIGURATION, 0, cfg, cfg_len) != 0) {
        kfree(cfg);
        free_device(dev);
        uint64_t rf;
        spinlock_acquire(&usb_lock, &rf);
        release_address_locked(hcd, addr);
        spinlock_release(&usb_lock, rf);
        return -1;
    }
    if (parse_config(dev, cfg, cfg_len) != 0) {
        kfree(cfg);
        free_device(dev);
        uint64_t rf;
        spinlock_acquire(&usb_lock, &rf);
        release_address_locked(hcd, addr);
        spinlock_release(&usb_lock, rf);
        return -1;
    }
    kfree(cfg);

    if (set_configuration(dev, cd.bConfigurationValue) != 0) {
        free_device(dev);
        uint64_t rf;
        spinlock_acquire(&usb_lock, &rf);
        release_address_locked(hcd, addr);
        spinlock_release(&usb_lock, rf);
        return -1;
    }

    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    dev->next = devices;
    devices = dev;
    spinlock_release(&usb_lock, flags);

    bind_device_interfaces(dev);
    klog(LOG_INFO, "USB: device id=%u addr=%u %04x:%04x root=%u hub=%u.%u ifaces=%u\n",
         dev->id, dev->address, dev->desc.idVendor, dev->desc.idProduct, root_port + 1,
         parent ? parent->id : 0, hub_port, dev->interface_count);
    return 0;
}

static int enumerate_root_port(struct usb_hcd *hcd, uint32_t hcd_index, uint32_t port) {
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    int exists = root_device_exists_locked(hcd, port);
    spinlock_release(&usb_lock, flags);

    int connected = hcd->ops->port_connected(hcd, port);
    if (exists && !connected) {
        struct usb_device *victim = NULL;
        spinlock_acquire(&usb_lock, &flags);
        for (struct usb_device *d = devices; d; d = d->next) {
            if (!d->parent && d->hcd == hcd && d->root_port == port) {
                victim = d;
                unlink_device_locked(d);
                d->disconnected = 1;
                break;
            }
        }
        spinlock_release(&usb_lock, flags);
        if (victim) {
            remove_device_tree(victim);
            free_device(victim);
            klog(LOG_INFO, "USB: root port %u disconnected\n", port + 1);
        }
        return 0;
    }
    if (exists || !connected) return 0;

    uint32_t speed = 0;
    if (hcd->ops->port_reset(hcd, port, &speed) != 0) return -1;
    return enumerate_device(hcd, hcd_index, port, NULL, 0, speed);
}

int usb_enumerate_hub_port(struct usb_device *hub, uint32_t port, uint32_t speed) {
    if (!hub || hub->disconnected || port == 0) return -1;
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    int exists = child_device_exists_locked(hub, port);
    spinlock_release(&usb_lock, flags);
    if (exists) return 0;
    return enumerate_device(hub->hcd, hub->hcd_index, hub->root_port, hub, port, speed);
}

int usb_hcd_scan(struct usb_hcd *hcd) {
    if (!hcd || !hcd->ops || !hcd->ops->port_count || !hcd->ops->port_connected) return -1;
    uint32_t hcd_index = hcd->index;
    uint32_t ports = hcd->ops->port_count(hcd);
    for (uint32_t i = 0; i < ports; i++) {
        if (!hcd->registered) return -1;
        enumerate_root_port(hcd, hcd_index, i);
    }
    return 0;
}

static void usb_hcd_scan_work(void *arg) {
    struct usb_hcd *hcd = (struct usb_hcd *)arg;
    if (!hcd || !hcd->registered) return;
    hcd->scan_running = 1;
    if (hcd->registered) usb_hcd_scan(hcd);
    hcd->scan_running = 0;
}

int usb_hcd_schedule_scan(struct usb_hcd *hcd, uint64_t delay_ticks) {
    if (!hcd || !hcd->registered) return -1;
    int r = kernel_queue_delayed_work(&hcd->scan_work, delay_ticks);
    if (r == -2) return 0;
    return r;
}

int usb_hcd_register(struct usb_hcd *hcd) {
    if (!hcd || !hcd->ops || !hcd->ops->control || !hcd->ops->interrupt_in || !hcd->ops->port_reset) return -1;
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    hcd->index = next_hcd_index++;
    memset(hcd->address_bitmap, 0, sizeof(hcd->address_bitmap));
    hcd->registered = 1;
    hcd->scan_running = 0;
    kernel_delayed_work_init(&hcd->scan_work, usb_hcd_scan_work, hcd);
    hcd->next = hcds;
    hcds = hcd;
    spinlock_release(&usb_lock, flags);
    if (usb_hcd_schedule_scan(hcd, USB_ENUM_DELAY_TICKS) != 0) {
        usb_hcd_unregister(hcd);
        return -1;
    }
    return 0;
}

static void usb_queue_poll_if_needed(void);

void usb_hcd_unregister(struct usb_hcd *hcd) {
    if (!hcd) return;
    hcd->registered = 0;
    (void)kernel_cancel_delayed_work(&hcd->scan_work);
    while (hcd->scan_running ||
           (hcd->scan_work.work.flags & (KERNEL_WORK_PENDING | KERNEL_WORK_RUNNING))) kernel_yield();
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    struct usb_hcd **hp = &hcds;
    while (*hp) {
        if (*hp == hcd) {
            *hp = hcd->next;
            break;
        }
        hp = &(*hp)->next;
    }
    spinlock_release(&usb_lock, flags);

    for (;;) {
        struct usb_device *victim = NULL;
        spinlock_acquire(&usb_lock, &flags);
        for (struct usb_device *d = devices; d; d = d->next) {
            if (d->hcd == hcd) {
                victim = d;
                unlink_device_locked(d);
                d->disconnected = 1;
                break;
            }
        }
        spinlock_release(&usb_lock, flags);
        if (!victim) break;
        remove_device_tree(victim);
        free_device(victim);
    }
}

int usb_register_interrupt_pipe(struct usb_interrupt_pipe *pipe) {
    if (!pipe || !pipe->hcd || !pipe->buffer || !pipe->callback || !pipe->dev) return -1;
    if (pipe->buffer_len == 0 || pipe->max_packet == 0) return -1;
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    pipe->active = 1;
    pipe->polling = 0;
    pipe->next = pipes;
    pipes = pipe;
    spinlock_release(&usb_lock, flags);
    usb_queue_poll_if_needed();
    return 0;
}

void usb_unregister_interrupt_pipe(struct usb_interrupt_pipe *pipe) {
    if (!pipe) return;
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    pipe->active = 0;
    struct usb_interrupt_pipe **pp = &pipes;
    while (*pp) {
        if (*pp == pipe) {
            *pp = pipe->next;
            break;
        }
        pp = &(*pp)->next;
    }
    spinlock_release(&usb_lock, flags);

    while (pipe->polling) kernel_yield();
    if (pipe->hcd && pipe->hcd->ops && pipe->hcd->ops->destroy_pipe) pipe->hcd->ops->destroy_pipe(pipe);
}

int usb_driver_register(struct usb_driver *driver) {
    if (!driver || !driver->match || !driver->bind) return -1;
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    driver->next = drivers;
    drivers = driver;
    struct usb_device *head = devices;
    spinlock_release(&usb_lock, flags);
    for (struct usb_device *dev = head; dev; dev = dev->next) bind_device_interfaces(dev);
    return 0;
}

void usb_driver_unregister(struct usb_driver *driver) {
    if (!driver) return;
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    struct usb_driver **pp = &drivers;
    while (*pp) {
        if (*pp == driver) {
            *pp = driver->next;
            break;
        }
        pp = &(*pp)->next;
    }
    struct usb_device *head = devices;
    spinlock_release(&usb_lock, flags);

    for (struct usb_device *dev = head; dev; dev = dev->next) {
        for (uint32_t i = 0; i < dev->interface_count; i++) {
            struct usb_interface *intf = &dev->interfaces[i];
            if (intf->driver == driver) {
                if (driver->unbind) driver->unbind(intf);
                intf->driver = NULL;
                intf->driver_private = NULL;
            }
        }
    }
}

static void poll_fn(void *arg) {
    (void)arg;
    struct usb_interrupt_pipe *snapshot[32];
    uint32_t count = 0;
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    for (struct usb_interrupt_pipe *p = pipes; p && count < 32; p = p->next) {
        if (p->active && !p->polling) {
            p->polling = 1;
            snapshot[count++] = p;
        }
    }
    if (count == 0) poll_started = 0;
    spinlock_release(&usb_lock, flags);
    if (count == 0) return;

    for (uint32_t i = 0; i < count; i++) {
        struct usb_interrupt_pipe *p = snapshot[i];
        if (p && p->active && p->hcd && p->hcd->ops && p->hcd->ops->interrupt_in) p->hcd->ops->interrupt_in(p);
        p->polling = 0;
    }

    spinlock_acquire(&usb_lock, &flags);
    int again = pipes != NULL;
    if (!again) poll_started = 0;
    spinlock_release(&usb_lock, flags);
    if (again && kernel_queue_delayed_work(&poll_work, USB_POLL_TICKS) != 0) {
        spinlock_acquire(&usb_lock, &flags);
        poll_started = 0;
        spinlock_release(&usb_lock, flags);
    }
}

static void usb_queue_poll_if_needed(void) {
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    int start = pipes && !poll_started;
    if (start) poll_started = 1;
    spinlock_release(&usb_lock, flags);
    if (start && kernel_queue_delayed_work(&poll_work, USB_POLL_TICKS) != 0) {
        spinlock_acquire(&usb_lock, &flags);
        poll_started = 0;
        spinlock_release(&usb_lock, flags);
    }
}

static void usb_text_build_devices(struct usb_text_ctx *ctx) {
    ctx->len = 0;
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    for (struct usb_device *d = devices; d && ctx->len + 160 < sizeof(ctx->buf); d = d->next) {
        int n = snprintf(ctx->buf + ctx->len, sizeof(ctx->buf) - ctx->len,
                         "id=%u hcd=%u root=%u hub=%u.%u speed=%u addr=%u vid=%04x pid=%04x class=%02x.%02x.%02x cfg=%u ifaces=%u\n",
                         d->id, d->hcd_index, d->root_port + 1, d->parent ? d->parent->id : 0,
                         d->hub_port, d->speed, d->address, d->desc.idVendor, d->desc.idProduct,
                         d->desc.bDeviceClass, d->desc.bDeviceSubClass, d->desc.bDeviceProtocol,
                         d->config_value, d->interface_count);
        if (n <= 0) break;
        if ((uint32_t)n >= sizeof(ctx->buf) - ctx->len) { ctx->len = sizeof(ctx->buf); break; }
        ctx->len += (uint32_t)n;
    }
    spinlock_release(&usb_lock, flags);
}

static void usb_text_build_hcds(struct usb_text_ctx *ctx) {
    ctx->len = 0;
    uint64_t flags;
    spinlock_acquire(&usb_lock, &flags);
    for (struct usb_hcd *h = hcds; h && ctx->len + 80 < sizeof(ctx->buf); h = h->next) {
        uint32_t ports = (h->ops && h->ops->port_count) ? h->ops->port_count(h) : 0;
        int n = snprintf(ctx->buf + ctx->len, sizeof(ctx->buf) - ctx->len, "id=%u name=%s ports=%u\n",
                         h->index, h->name ? h->name : "?", ports);
        if (n <= 0) break;
        if ((uint32_t)n >= sizeof(ctx->buf) - ctx->len) { ctx->len = sizeof(ctx->buf); break; }
        ctx->len += (uint32_t)n;
    }
    spinlock_release(&usb_lock, flags);
}

static int usb_text_open(void *priv, void **out) {
    if (!out) return -1;
    struct usb_text_ctx *ctx = (struct usb_text_ctx *)kzalloc(sizeof(*ctx));
    if (!ctx) return -1;
    ctx->kind = (uint32_t)(uintptr_t)priv;
    if (ctx->kind == USB_TEXT_DEVICES) usb_text_build_devices(ctx);
    else if (ctx->kind == USB_TEXT_HCDS) usb_text_build_hcds(ctx);
    else {
        kfree(ctx);
        return -1;
    }
    *out = ctx;
    return 0;
}

static void usb_text_close(void *priv, void *raw) {
    (void)priv;
    if (raw) kfree(raw);
}

static uint64_t usb_text_read_file(void *priv, void *raw, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)priv;
    struct usb_text_ctx *ctx = (struct usb_text_ctx *)raw;
    if (!ctx || !buffer || offset >= ctx->len) return 0;
    uint64_t n = ctx->len - offset;
    if (n > size) n = size;
    memcpy(buffer, ctx->buf + offset, n);
    ctx->pos = (uint32_t)(offset + n);
    return n;
}

static struct device_ops usb_devices_ops = {.open = usb_text_open, .close = usb_text_close, .read_file = usb_text_read_file};
static struct device_ops usb_hcds_ops = {.open = usb_text_open, .close = usb_text_close, .read_file = usb_text_read_file};

static int usb_core_init(void) {
    spinlock_init(&usb_lock);
    kernel_delayed_work_init(&poll_work, poll_fn, NULL);
    devfs_register("usb/devices", &usb_devices_ops, (void *)(uintptr_t)USB_TEXT_DEVICES);
    devfs_register("usb/hcds", &usb_hcds_ops, (void *)(uintptr_t)USB_TEXT_HCDS);
    return 0;
}

KEXPORT(usb_hcd_register)
KEXPORT(usb_hcd_unregister)
KEXPORT(usb_hcd_scan)
KEXPORT(usb_hcd_schedule_scan)
KEXPORT(usb_enumerate_hub_port)
KEXPORT(usb_remove_child_devices)
KEXPORT(usb_control_transfer)
KEXPORT(usb_bulk_transfer)
KEXPORT(usb_isochronous_transfer)
KEXPORT(usb_interrupt_transfer)
KEXPORT(usb_register_interrupt_pipe)
KEXPORT(usb_unregister_interrupt_pipe)
KEXPORT(usb_driver_register)
KEXPORT(usb_driver_unregister)

MODULE_INFO("usb_core", usb_core_init, 0, NULL);
