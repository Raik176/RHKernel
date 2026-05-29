#pragma once

#include <stddef.h>
#include <stdint.h>
#include "mod/workqueue.h"

#define USB_DIR_OUT 0x00u
#define USB_DIR_IN  0x80u

#define USB_REQ_GET_STATUS        0x00u
#define USB_REQ_CLEAR_FEATURE     0x01u
#define USB_REQ_SET_FEATURE       0x03u
#define USB_REQ_SET_ADDRESS       0x05u
#define USB_REQ_GET_DESCRIPTOR    0x06u
#define USB_REQ_SET_DESCRIPTOR    0x07u
#define USB_REQ_GET_CONFIGURATION 0x08u
#define USB_REQ_SET_CONFIGURATION 0x09u
#define USB_REQ_SET_INTERFACE     0x0Bu

#define USB_TYPE_STANDARD 0x00u
#define USB_TYPE_CLASS    0x20u
#define USB_RECIP_DEVICE  0x00u
#define USB_RECIP_INTERFACE 0x01u
#define USB_RECIP_ENDPOINT 0x02u
#define USB_RECIP_OTHER   0x03u

#define USB_DT_DEVICE        0x01u
#define USB_DT_CONFIGURATION 0x02u
#define USB_DT_STRING        0x03u
#define USB_DT_INTERFACE     0x04u
#define USB_DT_ENDPOINT      0x05u
#define USB_DT_HID           0x21u
#define USB_DT_REPORT        0x22u
#define USB_DT_HUB           0x29u

#define USB_CLASS_HID 0x03u
#define USB_CLASS_HUB 0x09u
#define USB_HID_SUBCLASS_BOOT 0x01u
#define USB_HID_PROTOCOL_KEYBOARD 0x01u

#define USB_ENDPOINT_XFER_CONTROL 0u
#define USB_ENDPOINT_XFER_ISOC    1u
#define USB_ENDPOINT_XFER_BULK    2u
#define USB_ENDPOINT_XFER_INTR    3u

#define USB_SPEED_LOW  1u
#define USB_SPEED_FULL 2u
#define USB_SPEED_HIGH 3u

#define USB_MAX_INTERFACES 8u
#define USB_MAX_ENDPOINTS_PER_INTERFACE 8u
#define USB_MAX_INTERFACE_EXTRA 128u

struct usb_setup_packet {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

struct usb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed));

struct usb_hid_descriptor_prefix {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} __attribute__((packed));

struct usb_hcd;
struct usb_device;
struct usb_driver;
struct usb_interrupt_pipe;

struct usb_interface {
    struct usb_device *dev;
    struct usb_interface_descriptor desc;
    struct usb_endpoint_descriptor endpoints[USB_MAX_ENDPOINTS_PER_INTERFACE];
    uint8_t endpoint_count;
    uint8_t extra[USB_MAX_INTERFACE_EXTRA];
    uint16_t extra_len;
    struct usb_driver *driver;
    void *driver_private;
};

struct usb_device {
    uint32_t id;
    struct usb_hcd *hcd;
    uint32_t hcd_index;
    uint32_t root_port;
    struct usb_device *parent;
    uint32_t hub_port;
    uint32_t speed;
    uint8_t address;
    uint8_t max_packet0;
    uint8_t disconnected;
    struct usb_device_descriptor desc;
    uint8_t config_value;
    struct usb_interface interfaces[USB_MAX_INTERFACES];
    uint8_t interface_count;
    struct usb_device *next;
};

typedef void (*usb_interrupt_callback_t)(struct usb_interrupt_pipe *pipe, const void *data,
                                         uint32_t len, int status, void *priv);

struct usb_hcd_ops {
    uint32_t (*port_count)(struct usb_hcd *hcd);
    int (*port_connected)(struct usb_hcd *hcd, uint32_t port);
    int (*port_reset)(struct usb_hcd *hcd, uint32_t port, uint32_t *speed);
    int (*control)(struct usb_hcd *hcd, uint8_t addr, uint8_t speed, uint8_t ep,
                   uint16_t max_packet, const struct usb_setup_packet *setup,
                   void *data, uint16_t len, uint32_t timeout_ms);
    int (*bulk)(struct usb_hcd *hcd, struct usb_device *dev, uint8_t endpoint,
                uint16_t max_packet, void *data, uint32_t len, uint32_t *actual,
                uint32_t timeout_ms);
    int (*isochronous)(struct usb_hcd *hcd, struct usb_device *dev, uint8_t endpoint,
                       uint16_t max_packet, void *data, uint32_t len, uint32_t *actual);
    int (*interrupt_in)(struct usb_interrupt_pipe *pipe);
    void (*destroy_pipe)(struct usb_interrupt_pipe *pipe);
};

struct usb_hcd {
    const char *name;
    void *priv;
    const struct usb_hcd_ops *ops;
    uint32_t index;
    uint32_t address_bitmap[4];
    struct kernel_delayed_work scan_work;
    volatile uint32_t registered;
    volatile uint32_t scan_running;
    struct usb_hcd *next;
};

struct usb_interrupt_pipe {
    struct usb_hcd *hcd;
    struct usb_device *dev;
    uint8_t endpoint;
    uint16_t max_packet;
    uint8_t interval;
    uint8_t *buffer;
    uint16_t buffer_len;
    usb_interrupt_callback_t callback;
    void *callback_priv;
    void *hcd_priv;
    uint8_t data_toggle;
    volatile uint32_t active;
    volatile uint32_t polling;
    struct usb_interrupt_pipe *next;
};

struct usb_driver {
    const char *name;
    int (*match)(struct usb_interface *intf);
    int (*bind)(struct usb_interface *intf);
    void (*unbind)(struct usb_interface *intf);
    struct usb_driver *next;
};

int usb_hcd_register(struct usb_hcd *hcd);
void usb_hcd_unregister(struct usb_hcd *hcd);
int usb_hcd_scan(struct usb_hcd *hcd);
int usb_hcd_schedule_scan(struct usb_hcd *hcd, uint64_t delay_ticks);
int usb_enumerate_hub_port(struct usb_device *hub, uint32_t port, uint32_t speed);
void usb_remove_child_devices(struct usb_device *parent, uint32_t hub_port);
int usb_control_transfer(struct usb_device *dev, uint8_t type, uint8_t req, uint16_t value,
                         uint16_t index, void *data, uint16_t len, uint32_t timeout_ms);
int usb_bulk_transfer(struct usb_device *dev, uint8_t endpoint, uint16_t max_packet,
                      void *data, uint32_t len, uint32_t *actual, uint32_t timeout_ms);
int usb_isochronous_transfer(struct usb_device *dev, uint8_t endpoint, uint16_t max_packet,
                             void *data, uint32_t len, uint32_t *actual);
int usb_interrupt_transfer(struct usb_device *dev, uint8_t endpoint, uint16_t max_packet,
                           void *data, uint16_t len, uint32_t *actual, uint32_t timeout_ms);
int usb_register_interrupt_pipe(struct usb_interrupt_pipe *pipe);
void usb_unregister_interrupt_pipe(struct usb_interrupt_pipe *pipe);
int usb_driver_register(struct usb_driver *driver);
void usb_driver_unregister(struct usb_driver *driver);
