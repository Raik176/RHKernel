#pragma once
#include "smp/lock.h"
#include "util.h"

// TODO: SMP SAFE!
namespace vfs {
    enum class VfsType { VFS_FILE = 1, VFS_DIRECTORY, VFS_CHAR_DEVICE };

    struct vfs_node {
        char *name;
        VfsType type;
        uint32_t size;
        uint32_t inode;
        uintptr_t ptr;

        struct vfs_node *next;
        struct vfs_node *child;

        struct vfs_node *(*finddir)(struct vfs_node *parent, const char *name);
        uint32_t (*read)(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
    };

    struct open_file {
        vfs_node *node;
        uint32_t offset;
        lock::spinlock lock;
    };

    void init();

    vfs_node *finddir(vfs_node *parent, const char *name);
    vfs_node *open(const char *path);
    uint32_t read(vfs_node *node, uint32_t offset, uint32_t size, void *buffer);
    vfs_node *get_root();
}  // namespace vfs