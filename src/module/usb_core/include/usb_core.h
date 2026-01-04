#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mod/device.h"

// USB Descriptor Types
#define USB_DESC_DEVICE 0x01
#define USB_DESC_CONFIG 0x02
#define USB_DESC_STRING 0x03
#define USB_DESC_INTERFACE 0x04
#define USB_DESC_ENDPOINT 0x05

// Standard Requests
#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_CONFIG 0x09

struct usb_setup_packet {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed));

struct usb_device_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t bcd_usb;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size0;
    uint16_t id_vendor;
    uint16_t id_product;
    uint16_t bcd_device;
    uint8_t i_manufacturer;
    uint8_t i_product;
    uint8_t i_serial_number;
    uint8_t num_configurations;
} __attribute__((packed));

struct usb_device;
struct usb_hcd;

struct usb_hcd {
    struct device *pci_dev;
    void *priv;
    int (*control_msg)(struct usb_hcd *hcd, uint8_t addr, uint8_t endpoint,
                       struct usb_setup_packet *setup, void *data, uint16_t len);
};

struct usb_device {
    uint8_t address;
    uint8_t port;
    struct usb_hcd *hcd;
    struct usb_device_descriptor descriptor;
    struct device dev;
};

void usb_register_hcd(struct usb_hcd *hcd);
int usb_enumerate_device(struct usb_hcd *hcd, uint8_t port);