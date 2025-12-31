#pragma once
#include <stdint.h>

/**
 * @brief Driver operations provided by the module.
 */
struct device_ops {
    uint32_t (*read)(void *priv, uint32_t offset, uint32_t size, uint8_t *buffer);
    uint32_t (*write)(void *priv, uint32_t offset, uint32_t size, uint8_t *buffer);
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register a character device into /dev.
 * Supports nested paths like "input/mouse0".
 */
int device_register(const char *path, struct device_ops *ops, void *priv);

/**
 * @brief Unregister a device and clean up empty parent directories.
 */
void device_unregister(const char *path);

#ifdef __cplusplus
}
#endif