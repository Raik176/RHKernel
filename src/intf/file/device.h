#pragma once
#include <stdint.h>
#include "file/vfs.h"
#include "mod/device.h"

namespace devfs {
    /**
     * @brief Bootstraps the /dev directory in the VFS.
     */
    void init();
}  // namespace devfs

namespace procfs {
    /**
     * @brief Bootstraps the /proc directory in the VFS.
     */
    void init();
}  // namespace procfs

namespace sysfs {
    void init();
}

void init_virt_fs();
bool devfs_is_device_node(vfs::vfs_node *node);
int devfs_open_file(vfs::vfs_node *node, vfs::open_file *file);
void devfs_close_file(vfs::open_file *file);
uint64_t devfs_read_file(vfs::open_file *file, uint64_t offset, uint64_t size, uint8_t *buffer);
uint64_t devfs_write_file(vfs::open_file *file, uint64_t offset, uint64_t size, uint8_t *buffer);
int devfs_mmap(vfs::vfs_node *node, uint64_t offset, uint64_t size, device_mmap_result *out);
int devfs_mmap_file(vfs::open_file *file, uint64_t offset, uint64_t size, device_mmap_result *out);

void devfs_release_task_resources(uint64_t task_id);
