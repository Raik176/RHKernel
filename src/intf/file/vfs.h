#pragma once
#include "smp/lock.h"
#include "util.h"

namespace vfs {
    enum class VfsType { VFS_FILE = 1, VFS_DIRECTORY, VFS_CHAR_DEVICE, VFS_BLOCK_DEVICE };

    struct vfs_dirent {
        uint32_t inode;
        uint32_t type;
        char name[256];
    };

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
        int (*readdir)(struct vfs_node *dir, uint64_t index, struct vfs_dirent *out);
        uint64_t (*read)(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
        uint64_t (*write)(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
        int (*create)(struct vfs_node *parent, const char *name, uint32_t type, struct vfs_node **out);
        int (*unlink)(struct vfs_node *parent, const char *name);
        int (*rename)(struct vfs_node *old_parent, const char *old_name,
                      struct vfs_node *new_parent, const char *new_name);
        int (*truncate)(struct vfs_node *node, uint64_t size);
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
    int readdir(vfs_node *dir, uint64_t index, vfs_dirent *out);
    vfs_node *open(const char *path);
    vfs_node *create(const char *path, VfsType type);
    int unlink(const char *path);
    int rename(const char *old_path, const char *new_path);
    int truncate(vfs_node *node, uint64_t size);
    uint64_t read(vfs_node *node, uint64_t offset, uint64_t size, void *buffer);
    uint64_t write(vfs_node *node, uint64_t offset, uint64_t size, void *buffer);
    vfs_node *get_root();
}  // namespace vfs
