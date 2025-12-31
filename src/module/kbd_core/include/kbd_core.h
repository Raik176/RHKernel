#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KDB_MAX_NAME 32

/* Key flags */
#define KDB_SHIFT (1 << 0)
#define KDB_CTRL (1 << 1)
#define KDB_ALT (1 << 2)
#define KDB_CAPS (1 << 3)

/* Opaque handle */
struct kdb_device;

/* Called by low-level drivers */
struct kdb_device *kdb_register(const char *name);
void kdb_unregister(struct kdb_device *kdb);

/* Inject raw key events */
void kdb_handle_scancode(struct kdb_device *kdb, uint8_t scancode, int pressed);

/* Optional helper for drivers */
uint32_t kdb_get_modifiers(struct kdb_device *kdb);

#ifdef __cplusplus
}
#endif
