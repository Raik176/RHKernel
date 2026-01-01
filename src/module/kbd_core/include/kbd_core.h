#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define kbd_MAX_NAME 32

/* Key flags */
#define kbd_SHIFT (1 << 0)
#define kbd_CTRL (1 << 1)
#define kbd_ALT (1 << 2)
#define kbd_CAPS (1 << 3)

/* Opaque handle */
struct kbd_device;

/* Called by low-level drivers */
struct kbd_device *kbd_register(const char *name);
void kbd_unregister(struct kbd_device *kbd);

/* Inject raw key events */
void kbd_handle_scancode(struct kbd_device *kbd, uint8_t scancode, int pressed);

/* Optional helper for drivers */
uint32_t kbd_get_modifiers(struct kbd_device *kbd);

#ifdef __cplusplus
}
#endif
