#pragma once
#include <stdint.h>

#define kbd_MAX_NAME 32

#ifdef __cplusplus
extern "C" {
#endif

struct kbd_device;

struct kbd_device *kbd_register(const char *name);
void kbd_unregister(struct kbd_device *kbd);
void kbd_handle_scancode(struct kbd_device *kbd, uint8_t scancode, int pressed);
void kbd_handle_key(struct kbd_device *kbd, uint32_t code, int pressed, uint32_t text);
uint32_t kbd_get_modifiers(struct kbd_device *kbd);

#ifdef __cplusplus
}
#endif
