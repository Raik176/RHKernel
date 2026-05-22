#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Driver operations provided by the module.
 */
struct device_ops {
    uint64_t (*read)(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer);
    uint64_t (*write)(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer);
};

struct device;
struct driver;
struct bus;

struct bus {
    const char *name;
    bool (*match)(struct device *dev, struct driver *drv);
    struct bus *next;
};

struct device {
    const char *name;
    struct bus *bus;
    struct driver *driver;
    void *bus_data;     // Bus-specific info (e.g., PCI BDF)
    void *driver_data;  // Driver-specific private state
    struct device *next;
};

struct driver {
    const char *name;
    struct bus *bus;
    struct module_metadata *module;

    // ID table is bus-specific (e.g., pci_device_id list)
    const void *id_table;

    int (*probe)(struct device *dev);
    void (*remove)(struct device *dev);

    struct driver *next;
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register a character device into /dev.
 * Supports nested paths like "input/mouse0".
 */
int devfs_register(const char *path, struct device_ops *ops, void *priv);

/**
 * @brief Register a block device into /dev.
 * Supports nested paths like "disk/sda".
 */
int devfs_register_block(const char *path, struct device_ops *ops, void *priv, uint64_t size);

/**
 * @brief Unregister a device and clean up empty parent directories.
 */
void devfs_unregister(const char *path);

void bus_register(struct bus *bus);
struct bus *find_bus(const char *name);
void device_register(struct device *dev);
void driver_register(struct driver *drv);

#ifdef __cplusplus
}
#endif