#pragma once
#include "smp/lock.h"
#include "util.h"

namespace vfs {
    enum class VfsType { VFS_FILE = 1, VFS_DIRECTORY, VFS_CHAR_DEVICE };

    struct vfs_node {
        char *name;
        VfsType type;
        uint64_t size;
        uint32_t inode;
        uintptr_t ptr;

        struct vfs_node *next;
        struct vfs_node *child;
        struct vfs_node *parent;

        struct vfs_node *(*finddir)(struct vfs_node *parent, const char *name);
        uint64_t (*read)(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
        uint64_t (*write)(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
    };

    struct open_file {
        vfs_node *node;
        uint64_t offset;
        spinlock_t lock;
        uint32_t ref_count;
    };

    void init();

    vfs_node *create_node(const char *name, VfsType type, vfs_node *parent);
    vfs_node *traverse_relative(vfs_node *start, const char *path);

    vfs_node *finddir(vfs_node *parent, const char *name);
    vfs_node *open(const char *path);
    uint64_t read(vfs_node *node, uint64_t offset, uint64_t size, void *buffer);
    uint64_t write(vfs_node *node, uint64_t offset, uint64_t size, void *buffer);
    vfs_node *get_root();
}  // namespace vfs