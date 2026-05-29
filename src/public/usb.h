#pragma once
#include <stdint.h>

#define USB_USER_SPEED_LOW  1u
#define USB_USER_SPEED_FULL 2u

struct usb_user_device {
    uint32_t id;
    uint32_t hcd_index;
    uint32_t port;
    uint32_t speed;
    uint32_t address;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t configuration;
};
