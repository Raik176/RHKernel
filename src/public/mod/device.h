#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Driver operations provided by the module.
 */
struct device_mmap_result {
    uint64_t phys;
    uint64_t size;
    uint32_t flags;
};

#define DEVICE_INFO_VERSION 1u
#define DEVICE_INFO_KIND_GENERIC 0u
#define DEVICE_INFO_KIND_BLOCK 1u
#define DEVICE_INFO_KIND_PARTITION 2u

#define DEVICE_INFO_FLAG_READONLY (1u << 0)
#define DEVICE_INFO_FLAG_REMOVABLE (1u << 1)

struct device_info {
    uint32_t version;
    uint32_t kind;
    uint32_t flags;
    uint32_t logical_block_size;
    uint32_t physical_block_size;
    uint32_t max_transfer_bytes;
    uint64_t block_count;
    uint64_t size_bytes;
    uint64_t max_size_bytes;
    uint64_t start_lba;
    uint64_t parent_size_bytes;
    char driver[32];
    char media_type[32];
    char scheme[16];
    char type[64];
    char uuid[40];
    char parent[64];
};

#define DEVICE_MMAP_WRITE_COMBINING (1u << 0)
#define DEVICE_MMAP_NO_CACHE (1u << 1)

struct device_ops {
    uint64_t (*read)(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer);
    uint64_t (*write)(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer);
    int (*mmap)(void *priv, uint64_t offset, uint64_t size, struct device_mmap_result *out);
    int (*open)(void *priv, void **ctx);
    void (*close)(void *priv, void *ctx);
    uint64_t (*read_file)(void *priv, void *ctx, uint64_t offset, uint64_t size, uint8_t *buffer);
    uint64_t (*write_file)(void *priv, void *ctx, uint64_t offset, uint64_t size, uint8_t *buffer);
    int (*mmap_file)(void *priv, void *ctx, uint64_t offset, uint64_t size, struct device_mmap_result *out);
    int (*info)(void *priv, struct device_info *out);
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