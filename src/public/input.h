#pragma once
#include <stdint.h>

#define INPUT_EVENT_KEY 1u
#define INPUT_EVENT_DEVICE 2u
#define INPUT_EVENT_SYNC 3u

#define INPUT_DEVICE_KEYBOARD 1u

#define INPUT_DEVICE_ADDED 1u
#define INPUT_DEVICE_REMOVED 2u

#define INPUT_SYNC_DROPPED 1u

#define INPUT_KEY_RELEASED 0u
#define INPUT_KEY_PRESSED 1u
#define INPUT_KEY_REPEATED 2u

#define INPUT_MOD_SHIFT (1u << 0)
#define INPUT_MOD_CTRL  (1u << 1)
#define INPUT_MOD_ALT   (1u << 2)
#define INPUT_MOD_CAPS  (1u << 3)

#define INPUT_KEY_EXTENDED (1u << 16)
#define INPUT_KEY_MAX_CODE ((1u << 17) - 1u)

#define INPUT_STREAM_AGGREGATE 0xffffffffu

struct input_event {
    uint64_t sequence;
    uint32_t type;
    uint32_t device;
    uint32_t code;
    uint32_t value;
    uint32_t modifiers;
    uint32_t text;
    uint32_t reserved;
};
