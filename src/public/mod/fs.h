#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vfs_node;

struct vfs_dirent {
    uint32_t inode;
    uint32_t type;
    char name[256];
};

enum vfs_node_type {
    VFS_NODE_FILE = 1,
    VFS_NODE_DIRECTORY = 2,
    VFS_NODE_CHAR_DEVICE = 3,
    VFS_NODE_BLOCK_DEVICE = 4,
};

struct fs_driver {
    const char *name;
    int (*mount)(struct vfs_node *blockdev, struct vfs_node *mountpoint, const char *flags);
    struct fs_driver *next;
};

int fs_register(struct fs_driver *driver);
int fs_mount(const char *source, const char *target, const char *fstype, const char *flags);
int fs_mount_auto(const char *source, const char *target, const char *flags);

struct vfs_node *vfs_open_c(const char *path);
struct vfs_node *vfs_create_fs_node(const char *name, uint32_t type, uint32_t inode,
                                    uint64_t size, void *fs_data);
int vfs_add_child(struct vfs_node *parent, struct vfs_node *child);
void *vfs_get_fs_data(struct vfs_node *node);
void vfs_set_fs_data(struct vfs_node *node, void *data);
void vfs_set_size(struct vfs_node *node, uint64_t size);
void vfs_set_finddir(struct vfs_node *node,
                     struct vfs_node *(*finddir)(struct vfs_node *, const char *));
void vfs_set_readdir(struct vfs_node *node,
                     int (*readdir)(struct vfs_node *, uint64_t, struct vfs_dirent *));
void vfs_set_read(struct vfs_node *node,
                  uint64_t (*read)(struct vfs_node *, uint64_t, uint64_t, uint8_t *));
void vfs_set_write(struct vfs_node *node,
                   uint64_t (*write)(struct vfs_node *, uint64_t, uint64_t, uint8_t *));
void vfs_set_create(struct vfs_node *node,
                    int (*create)(struct vfs_node *, const char *, uint32_t, struct vfs_node **));
void vfs_set_unlink(struct vfs_node *node, int (*unlink)(struct vfs_node *, const char *));
void vfs_set_rename(struct vfs_node *node,
                    int (*rename)(struct vfs_node *, const char *, struct vfs_node *, const char *));
void vfs_set_truncate(struct vfs_node *node, int (*truncate)(struct vfs_node *, uint64_t));
uint64_t vfs_read_c(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer);
uint64_t vfs_write_c(struct vfs_node *node, uint64_t offset, uint64_t size, const void *buffer);
uint64_t block_read(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer);
uint64_t block_write(struct vfs_node *node, uint64_t offset, uint64_t size, const void *buffer);
uint64_t block_size(struct vfs_node *node);

#ifdef __cplusplus
}
#endif
