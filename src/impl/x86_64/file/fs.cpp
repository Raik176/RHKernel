#include "mod/fs.h"

#include "console.h"
#include "file/vfs.h"
#include "memory/heap.h"
#include "mod/logging.h"
#include "string.h"
#include "symbol/ksym.h"

static fs_driver *fs_drivers = nullptr;

extern "C" int fs_register(fs_driver *driver) {
    if (!driver || !driver->name || !driver->mount) return -1;
    for (fs_driver *d = fs_drivers; d; d = d->next) {
        if (strcmp(d->name, driver->name) == 0) return -1;
    }
    driver->next = fs_drivers;
    fs_drivers = driver;
    klog(LOG_INFO, "FS: registered %s\n", driver->name);
    return 0;
}

extern "C" int fs_mount(const char *source, const char *target, const char *fstype, const char *flags) {
    if (!source || !target || !fstype) return -1;
    vfs::vfs_node *src = vfs::open(source);
    vfs::vfs_node *dst = vfs::open(target);
    if (!src || !dst || dst->type != vfs::VfsType::VFS_DIRECTORY) {
        klog(LOG_ERR, "FS: mount %s on %s failed: bad source or target\n", source, target);
        return -1;
    }
    for (fs_driver *d = fs_drivers; d; d = d->next) {
        if (strcmp(d->name, fstype) == 0) {
            int r = d->mount((struct vfs_node *)src, (struct vfs_node *)dst, flags);
            if (r == 0) klog(LOG_INFO, "FS: mounted %s on %s as %s\n", source, target, fstype);
            else klog(LOG_ERR, "FS: %s mount of %s on %s failed: %d\n", fstype, source, target, r);
            return r;
        }
    }
    klog(LOG_ERR, "FS: unknown filesystem %s\n", fstype);
    return -1;
}

extern "C" int fs_mount_auto(const char *source, const char *target, const char *flags) {
    for (fs_driver *d = fs_drivers; d; d = d->next) {
        if (fs_mount(source, target, d->name, flags) == 0) return 0;
    }
    return -1;
}

extern "C" struct vfs_node *vfs_open_c(const char *path) { return (struct vfs_node *)vfs::open(path); }

extern "C" struct vfs_node *vfs_create_fs_node(const char *name, uint32_t type, uint32_t inode,
                                                uint64_t size, void *fs_data) {
    vfs::VfsType vt = vfs::VfsType::VFS_FILE;
    if (type == VFS_NODE_DIRECTORY) vt = vfs::VfsType::VFS_DIRECTORY;
    else if (type == VFS_NODE_CHAR_DEVICE) vt = vfs::VfsType::VFS_CHAR_DEVICE;
    else if (type == VFS_NODE_BLOCK_DEVICE) vt = vfs::VfsType::VFS_BLOCK_DEVICE;

    vfs::vfs_node *node = (vfs::vfs_node *)heap::kmalloc(sizeof(vfs::vfs_node));
    if (!node) return nullptr;
    memset(node, 0, sizeof(*node));
    node->name = strdup(name ? name : "");
    node->type = vt;
    node->inode = inode;
    node->size = size;
    node->ptr = (uintptr_t)fs_data;
    return (struct vfs_node *)node;
}

extern "C" int vfs_add_child(struct vfs_node *parent_raw, struct vfs_node *child_raw) {
    vfs::vfs_node *parent = (vfs::vfs_node *)parent_raw;
    vfs::vfs_node *child = (vfs::vfs_node *)child_raw;
    if (!parent || !child) return -1;
    child->parent = parent;
    child->next = parent->child;
    parent->child = child;
    return 0;
}

extern "C" void *vfs_get_fs_data(struct vfs_node *node_raw) { auto *node = (vfs::vfs_node *)node_raw; return node ? (void *)node->ptr : nullptr; }
extern "C" void vfs_set_fs_data(struct vfs_node *node_raw, void *data) { auto *node = (vfs::vfs_node *)node_raw; if (node) node->ptr = (uintptr_t)data; }
extern "C" void vfs_set_size(struct vfs_node *node_raw, uint64_t size) { auto *node = (vfs::vfs_node *)node_raw; if (node) node->size = size; }
extern "C" void vfs_set_finddir(struct vfs_node *node_raw, struct vfs_node *(*finddir)(struct vfs_node *, const char *)) { auto *node = (vfs::vfs_node *)node_raw; if (node) node->finddir = (vfs::vfs_node *(*)(vfs::vfs_node *, const char *))finddir; }
extern "C" void vfs_set_readdir(struct vfs_node *node_raw, int (*readdir)(struct vfs_node *, uint64_t, struct vfs_dirent *)) { auto *node = (vfs::vfs_node *)node_raw; if (node) node->readdir = (int (*)(vfs::vfs_node *, uint64_t, vfs::vfs_dirent *))readdir; }
extern "C" void vfs_set_read(struct vfs_node *node_raw, uint64_t (*read)(struct vfs_node *, uint64_t, uint64_t, uint8_t *)) { auto *node = (vfs::vfs_node *)node_raw; if (node) node->read = (uint64_t (*)(vfs::vfs_node *, uint64_t, uint64_t, uint8_t *))read; }
extern "C" void vfs_set_write(struct vfs_node *node_raw, uint64_t (*write)(struct vfs_node *, uint64_t, uint64_t, uint8_t *)) { auto *node = (vfs::vfs_node *)node_raw; if (node) node->write = (uint64_t (*)(vfs::vfs_node *, uint64_t, uint64_t, uint8_t *))write; }
extern "C" void vfs_set_create(struct vfs_node *node_raw, int (*create)(struct vfs_node *, const char *, uint32_t, struct vfs_node **)) { auto *node = (vfs::vfs_node *)node_raw; if (node) node->create = (int (*)(vfs::vfs_node *, const char *, uint32_t, vfs::vfs_node **))create; }
extern "C" void vfs_set_unlink(struct vfs_node *node_raw, int (*unlink)(struct vfs_node *, const char *)) { auto *node = (vfs::vfs_node *)node_raw; if (node) node->unlink = (int (*)(vfs::vfs_node *, const char *))unlink; }
extern "C" void vfs_set_rename(struct vfs_node *node_raw, int (*rename)(struct vfs_node *, const char *, struct vfs_node *, const char *)) { auto *node = (vfs::vfs_node *)node_raw; if (node) node->rename = (int (*)(vfs::vfs_node *, const char *, vfs::vfs_node *, const char *))rename; }
extern "C" void vfs_set_truncate(struct vfs_node *node_raw, int (*truncate)(struct vfs_node *, uint64_t)) { auto *node = (vfs::vfs_node *)node_raw; if (node) node->truncate = (int (*)(vfs::vfs_node *, uint64_t))truncate; }
extern "C" uint64_t vfs_read_c(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer) { return vfs::read((vfs::vfs_node *)node, offset, size, buffer); }
extern "C" uint64_t vfs_write_c(struct vfs_node *node, uint64_t offset, uint64_t size, const void *buffer) { return vfs::write((vfs::vfs_node *)node, offset, size, (void *)buffer); }
extern "C" uint64_t block_read(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer) { return vfs::read((vfs::vfs_node *)node, offset, size, buffer); }
extern "C" uint64_t block_write(struct vfs_node *node, uint64_t offset, uint64_t size, const void *buffer) { return vfs::write((vfs::vfs_node *)node, offset, size, (void *)buffer); }
extern "C" uint64_t block_size(struct vfs_node *node_raw) { auto *node = (vfs::vfs_node *)node_raw; return node ? node->size : 0; }

KEXPORT(fs_register)
KEXPORT(fs_mount)
KEXPORT(fs_mount_auto)
KEXPORT(vfs_open_c)
KEXPORT(vfs_create_fs_node)
KEXPORT(vfs_add_child)
KEXPORT(vfs_get_fs_data)
KEXPORT(vfs_set_fs_data)
KEXPORT(vfs_set_size)
KEXPORT(vfs_set_finddir)
KEXPORT(vfs_set_readdir)
KEXPORT(vfs_set_read)
KEXPORT(vfs_set_write)
KEXPORT(vfs_set_create)
KEXPORT(vfs_set_unlink)
KEXPORT(vfs_set_rename)
KEXPORT(vfs_set_truncate)
KEXPORT(vfs_read_c)
KEXPORT(vfs_write_c)
KEXPORT(block_read)
KEXPORT(block_write)
KEXPORT(block_size)
