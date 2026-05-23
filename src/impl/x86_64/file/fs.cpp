#include "mod/fs.h"

#include "console.h"
#include "file/vfs.h"
#include "memory/heap.h"
#include "mod/logging.h"
#include "string.h"
#include "symbol/ksym.h"

static fs_driver *fs_drivers = nullptr;

struct fs_mount_snapshot {
    vfs::VfsType type;
    uint64_t size;
    uint32_t inode;
    uintptr_t ptr;
    vfs::vfs_node *child;
    vfs::vfs_node *(*finddir)(vfs::vfs_node *, const char *);
    int (*readdir)(vfs::vfs_node *, uint64_t, vfs::vfs_dirent *);
    uint64_t (*read)(vfs::vfs_node *, uint64_t, uint64_t, uint8_t *);
    uint64_t (*write)(vfs::vfs_node *, uint64_t, uint64_t, uint8_t *);
    int (*create)(vfs::vfs_node *, const char *, uint32_t, vfs::vfs_node **);
    int (*unlink)(vfs::vfs_node *, const char *);
    int (*rename)(vfs::vfs_node *, const char *, vfs::vfs_node *, const char *);
    int (*truncate)(vfs::vfs_node *, uint64_t);
};

struct fs_mount_record {
    char *source;
    char *target;
    char *type;
    char *flags;
    vfs::vfs_node *source_node;
    vfs::vfs_node *mountpoint;
    fs_driver *driver;
    fs_mount_snapshot original;
    fs_mount_record *next;
};

static fs_mount_record *fs_mounts = nullptr;
static spinlock_t fs_mount_lock;

static constexpr uint64_t BLOCK_CACHE_BLOCK_SIZE = 4096;
static constexpr uint64_t BLOCK_CACHE_ENTRIES = 64;
static constexpr uint64_t BLOCK_READAHEAD_BLOCKS = 2;

struct block_cache_entry {
    vfs::vfs_node *node;
    uint64_t block;
    uint64_t last_used;
    bool valid;
    bool loading;
    uint8_t data[BLOCK_CACHE_BLOCK_SIZE];
};

static block_cache_entry block_cache[BLOCK_CACHE_ENTRIES];
static spinlock_t block_cache_lock;
static uint64_t block_cache_clock;

static block_cache_entry *block_cache_find_locked(vfs::vfs_node *node, uint64_t block) {
    for (uint64_t i = 0; i < BLOCK_CACHE_ENTRIES; i++) {
        if (block_cache[i].valid && block_cache[i].node == node && block_cache[i].block == block)
            return &block_cache[i];
    }
    return nullptr;
}

static block_cache_entry *block_cache_pick_locked() {
    block_cache_entry *best = &block_cache[0];
    for (uint64_t i = 0; i < BLOCK_CACHE_ENTRIES; i++) {
        if (!block_cache[i].valid && !block_cache[i].loading) return &block_cache[i];
        if (!block_cache[i].loading && block_cache[i].last_used < best->last_used) best = &block_cache[i];
    }
    return best->loading ? nullptr : best;
}

static bool block_cache_load(vfs::vfs_node *node, uint64_t block, block_cache_entry **out) {
    uint64_t flags;
    for (;;) {
        block_cache_lock.acquire(flags);
        block_cache_entry *entry = block_cache_find_locked(node, block);
        if (entry && !entry->loading) {
            entry->last_used = ++block_cache_clock;
            *out = entry;
            block_cache_lock.release(flags);
            return true;
        }
        if (entry && entry->loading) {
            block_cache_lock.release(flags);
            asm volatile("pause");
            continue;
        }
        entry = block_cache_pick_locked();
        if (!entry) {
            block_cache_lock.release(flags);
            asm volatile("pause");
            continue;
        }
        entry->valid = false;
        entry->loading = true;
        entry->node = node;
        entry->block = block;
        block_cache_lock.release(flags);

        uint64_t got = vfs::read(node, block * BLOCK_CACHE_BLOCK_SIZE, BLOCK_CACHE_BLOCK_SIZE, entry->data);

        block_cache_lock.acquire(flags);
        entry->loading = false;
        if (got == BLOCK_CACHE_BLOCK_SIZE) {
            entry->valid = true;
            entry->last_used = ++block_cache_clock;
            *out = entry;
            block_cache_lock.release(flags);
            return true;
        }
        entry->valid = false;
        block_cache_lock.release(flags);
        return false;
    }
}

static void block_cache_prefetch(vfs::vfs_node *node, uint64_t first_block) {
    for (uint64_t i = 0; i < BLOCK_READAHEAD_BLOCKS; i++) {
        uint64_t block = first_block + i;
        uint64_t flags;
        block_cache_lock.acquire(flags);
        bool present = block_cache_find_locked(node, block) != nullptr;
        block_cache_lock.release(flags);
        if (!present) {
            block_cache_entry *unused;
            (void)block_cache_load(node, block, &unused);
        }
    }
}

static void block_cache_invalidate(vfs::vfs_node *node, uint64_t first_block, uint64_t last_block) {
    uint64_t flags;
    block_cache_lock.acquire(flags);
    for (uint64_t i = 0; i < BLOCK_CACHE_ENTRIES; i++) {
        if (block_cache[i].valid && block_cache[i].node == node && block_cache[i].block >= first_block &&
            block_cache[i].block <= last_block) {
            block_cache[i].valid = false;
        }
    }
    block_cache_lock.release(flags);
}

static void block_cache_invalidate_node(vfs::vfs_node *node) {
    uint64_t flags;
    block_cache_lock.acquire(flags);
    for (uint64_t i = 0; i < BLOCK_CACHE_ENTRIES; i++) {
        if (block_cache[i].valid && block_cache[i].node == node) block_cache[i].valid = false;
    }
    block_cache_lock.release(flags);
}

static uint64_t cached_block_read(vfs::vfs_node *node, uint64_t offset, uint64_t size, void *buffer) {
    if (!node || !buffer || node->type != vfs::VfsType::VFS_BLOCK_DEVICE) return vfs::read(node, offset, size, buffer);
    uint8_t *out = (uint8_t *)buffer;
    uint64_t done = 0;
    while (done < size) {
        uint64_t abs = offset + done;
        uint64_t block = abs / BLOCK_CACHE_BLOCK_SIZE;
        uint64_t in_block = abs % BLOCK_CACHE_BLOCK_SIZE;
        uint64_t chunk = BLOCK_CACHE_BLOCK_SIZE - in_block;
        if (chunk > size - done) chunk = size - done;

        block_cache_entry *entry;
        if (!block_cache_load(node, block, &entry)) break;

        uint64_t flags;
        block_cache_lock.acquire(flags);
        entry = block_cache_find_locked(node, block);
        if (!entry || entry->loading) {
            block_cache_lock.release(flags);
            continue;
        }
        memcpy(out + done, entry->data + in_block, chunk);
        entry->last_used = ++block_cache_clock;
        block_cache_lock.release(flags);
        done += chunk;

        if (in_block + chunk == BLOCK_CACHE_BLOCK_SIZE) block_cache_prefetch(node, block + 1);
    }
    return done;
}

static uint64_t cached_block_write(vfs::vfs_node *node, uint64_t offset, uint64_t size, const void *buffer) {
    if (!node || !buffer || node->type != vfs::VfsType::VFS_BLOCK_DEVICE) return vfs::write(node, offset, size, (void *)buffer);
    uint64_t written = vfs::write(node, offset, size, (void *)buffer);
    if (written) {
        uint64_t first = offset / BLOCK_CACHE_BLOCK_SIZE;
        uint64_t last = (offset + written - 1) / BLOCK_CACHE_BLOCK_SIZE;
        block_cache_invalidate(node, first, last);
    }
    return written;
}

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


static void fs_snapshot_node(vfs::vfs_node *node, fs_mount_snapshot *out) {
    out->type = node->type;
    out->size = node->size;
    out->inode = node->inode;
    out->ptr = node->ptr;
    out->child = node->child;
    out->finddir = node->finddir;
    out->readdir = node->readdir;
    out->read = node->read;
    out->write = node->write;
    out->create = node->create;
    out->unlink = node->unlink;
    out->rename = node->rename;
    out->truncate = node->truncate;
}

static void fs_restore_node(vfs::vfs_node *node, const fs_mount_snapshot *snap) {
    node->type = snap->type;
    node->size = snap->size;
    node->inode = snap->inode;
    node->ptr = snap->ptr;
    node->child = snap->child;
    node->finddir = snap->finddir;
    node->readdir = snap->readdir;
    node->read = snap->read;
    node->write = snap->write;
    node->create = snap->create;
    node->unlink = snap->unlink;
    node->rename = snap->rename;
    node->truncate = snap->truncate;
}

static fs_mount_record *fs_alloc_mount_record(const char *source, const char *target,
                                              const char *type, const char *flags) {
    fs_mount_record *rec = (fs_mount_record *)heap::kmalloc(sizeof(*rec));
    if (!rec) return nullptr;
    memset(rec, 0, sizeof(*rec));
    rec->source = strdup(source ? source : "");
    rec->target = strdup(target ? target : "");
    rec->type = strdup(type ? type : "");
    rec->flags = strdup(flags ? flags : "");
    if (!rec->source || !rec->target || !rec->type || !rec->flags) {
        if (rec->source) heap::kfree(rec->source);
        if (rec->target) heap::kfree(rec->target);
        if (rec->type) heap::kfree(rec->type);
        if (rec->flags) heap::kfree(rec->flags);
        heap::kfree(rec);
        return nullptr;
    }
    return rec;
}

static void fs_free_mount_record(fs_mount_record *rec) {
    if (!rec) return;
    heap::kfree(rec->source);
    heap::kfree(rec->target);
    heap::kfree(rec->type);
    heap::kfree(rec->flags);
    heap::kfree(rec);
}

static bool fs_target_mounted_locked(const char *target) {
    for (fs_mount_record *m = fs_mounts; m; m = m->next) {
        if (strcmp(m->target, target) == 0) return true;
    }
    return false;
}

static uint64_t fs_proc_copy(const char *buf, uint64_t len, uint64_t offset, uint64_t size, uint8_t *out) {
    if (offset >= len) return 0;
    uint64_t n = len - offset;
    if (n > size) n = size;
    memcpy(out, buf + offset, n);
    return n;
}

static void fs_append(char *buf, uint64_t cap, uint64_t *pos, const char *s) {
    if (!buf || !cap || !pos || !s) return;
    while (*s && *pos + 1 < cap) buf[(*pos)++] = *s++;
    buf[*pos < cap ? *pos : cap - 1] = 0;
}

static uint64_t proc_filesystems_read(vfs::vfs_node *, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[1024];
    uint64_t len = 0;
    uint64_t flags;
    fs_mount_lock.acquire(flags);
    for (fs_driver *d = fs_drivers; d; d = d->next) {
        fs_append(buf, sizeof(buf), &len, d->name ? d->name : "?");
        fs_append(buf, sizeof(buf), &len, d->probe ? " probe" : " noprobe");
        fs_append(buf, sizeof(buf), &len, d->unmount ? " unmount" : " nounmount");
        fs_append(buf, sizeof(buf), &len, "\n");
    }
    fs_mount_lock.release(flags);
    return fs_proc_copy(buf, len, offset, size, buffer);
}

static uint64_t proc_mounts_read(vfs::vfs_node *, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[4096];
    uint64_t len = 0;
    uint64_t flags;
    fs_mount_lock.acquire(flags);
    for (fs_mount_record *m = fs_mounts; m; m = m->next) {
        fs_append(buf, sizeof(buf), &len, m->source);
        fs_append(buf, sizeof(buf), &len, " ");
        fs_append(buf, sizeof(buf), &len, m->target);
        fs_append(buf, sizeof(buf), &len, " ");
        fs_append(buf, sizeof(buf), &len, m->type);
        fs_append(buf, sizeof(buf), &len, " ");
        fs_append(buf, sizeof(buf), &len, m->flags && m->flags[0] ? m->flags : "-");
        fs_append(buf, sizeof(buf), &len, "\n");
    }
    fs_mount_lock.release(flags);
    return fs_proc_copy(buf, len, offset, size, buffer);
}

extern "C" void fs_publish_proc(void) {
    vfs::vfs_node *proc = vfs::open("/proc");
    if (!proc || proc->type != vfs::VfsType::VFS_DIRECTORY) return;

    vfs::vfs_node *filesystems = vfs::finddir(proc, "filesystems");
    if (!filesystems) filesystems = vfs::create_node("filesystems", vfs::VfsType::VFS_CHAR_DEVICE, proc);
    if (filesystems) filesystems->read = proc_filesystems_read;

    vfs::vfs_node *mounts = vfs::finddir(proc, "mounts");
    if (!mounts) mounts = vfs::create_node("mounts", vfs::VfsType::VFS_CHAR_DEVICE, proc);
    if (mounts) mounts->read = proc_mounts_read;
}

static fs_driver *fs_find_driver(const char *fstype) {
    if (!fstype) return nullptr;
    for (fs_driver *d = fs_drivers; d; d = d->next) {
        if (strcmp(d->name, fstype) == 0) return d;
    }
    return nullptr;
}

extern "C" int fs_probe_node(struct vfs_node *source, const char *fstype) {
    if (!source) return FS_PROBE_ERR;
    if (fstype) {
        fs_driver *driver = fs_find_driver(fstype);
        if (!driver || !driver->probe) return FS_PROBE_UNSUPPORTED;
        return driver->probe(source);
    }

    int saw_supported = 0;
    for (fs_driver *d = fs_drivers; d; d = d->next) {
        if (!d->probe) continue;
        saw_supported = 1;
        int r = d->probe(source);
        if (r == FS_PROBE_YES) return FS_PROBE_YES;
    }
    return saw_supported ? FS_PROBE_NO : FS_PROBE_UNSUPPORTED;
}

extern "C" int fs_probe(const char *source, const char *fstype) {
    if (!source) return FS_PROBE_ERR;
    vfs::vfs_node *src = vfs::open(source);
    if (!src) return FS_PROBE_ERR;
    return fs_probe_node((struct vfs_node *)src, fstype);
}

static int fs_mount_driver(fs_driver *driver, vfs::vfs_node *src, vfs::vfs_node *dst,
                           const char *source, const char *target, const char *flags) {
    if (!driver || !src || !dst || !source || !target) return -1;

    uint64_t lock_flags;
    fs_mount_lock.acquire(lock_flags);
    if (fs_target_mounted_locked(target)) {
        fs_mount_lock.release(lock_flags);
        return -1;
    }
    fs_mount_lock.release(lock_flags);

    fs_mount_record *rec = fs_alloc_mount_record(source, target, driver->name, flags);
    if (!rec) return -1;
    rec->source_node = src;
    rec->mountpoint = dst;
    rec->driver = driver;
    fs_snapshot_node(dst, &rec->original);
    dst->child = nullptr;

    int r = driver->mount((struct vfs_node *)src, (struct vfs_node *)dst, flags);
    if (r != 0) {
        fs_restore_node(dst, &rec->original);
        fs_free_mount_record(rec);
        klog(LOG_ERR, "FS: %s mount of %s on %s failed: %d\n", driver->name, source, target, r);
        return r;
    }

    fs_mount_lock.acquire(lock_flags);
    if (fs_target_mounted_locked(target)) {
        fs_mount_lock.release(lock_flags);
        if (driver->unmount) (void)driver->unmount((struct vfs_node *)dst);
        fs_restore_node(dst, &rec->original);
        fs_free_mount_record(rec);
        return -1;
    }
    rec->next = fs_mounts;
    fs_mounts = rec;
    fs_mount_lock.release(lock_flags);

    klog(LOG_INFO, "FS: mounted %s on %s as %s\n", source, target, driver->name);
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
    fs_driver *driver = fs_find_driver(fstype);
    if (!driver) {
        klog(LOG_ERR, "FS: unknown filesystem %s\n", fstype);
        return -1;
    }
    return fs_mount_driver(driver, src, dst, source, target, flags);
}

extern "C" int fs_mount_auto(const char *source, const char *target, const char *flags) {
    if (!source || !target) return -1;
    vfs::vfs_node *src = vfs::open(source);
    vfs::vfs_node *dst = vfs::open(target);
    if (!src || !dst || dst->type != vfs::VfsType::VFS_DIRECTORY) {
        klog(LOG_ERR, "FS: automount %s on %s failed: bad source or target\n", source, target);
        return -1;
    }

    for (fs_driver *d = fs_drivers; d; d = d->next) {
        if (!d->probe) continue;
        int pr = d->probe((struct vfs_node *)src);
        if (pr == FS_PROBE_YES) return fs_mount_driver(d, src, dst, source, target, flags);
        if (pr < 0) klog(LOG_ERR, "FS: probe %s on %s failed: %d\n", d->name, source, pr);
    }

    for (fs_driver *d = fs_drivers; d; d = d->next) {
        if (d->probe) continue;
        if (fs_mount_driver(d, src, dst, source, target, flags) == 0) return 0;
    }
    return -1;
}

extern "C" int fs_unmount(const char *target) {
    if (!target) return -1;

    uint64_t flags;
    fs_mount_lock.acquire(flags);
    fs_mount_record **link = &fs_mounts;
    fs_mount_record *rec = nullptr;
    while (*link) {
        if (strcmp((*link)->target, target) == 0) {
            rec = *link;
            *link = rec->next;
            break;
        }
        link = &(*link)->next;
    }
    fs_mount_lock.release(flags);

    if (!rec) return -1;
    if (rec->driver && rec->driver->unmount) {
        int r = rec->driver->unmount((struct vfs_node *)rec->mountpoint);
        if (r != 0) {
            fs_mount_lock.acquire(flags);
            rec->next = fs_mounts;
            fs_mounts = rec;
            fs_mount_lock.release(flags);
            return r;
        }
    }
    fs_restore_node(rec->mountpoint, &rec->original);
    block_cache_invalidate_node(rec->source_node);
    klog(LOG_INFO, "FS: unmounted %s\n", rec->target);
    fs_free_mount_record(rec);
    return 0;
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
extern "C" uint64_t block_read(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer) { return cached_block_read((vfs::vfs_node *)node, offset, size, buffer); }
extern "C" uint64_t block_write(struct vfs_node *node, uint64_t offset, uint64_t size, const void *buffer) { return cached_block_write((vfs::vfs_node *)node, offset, size, buffer); }
extern "C" uint64_t block_size(struct vfs_node *node_raw) { auto *node = (vfs::vfs_node *)node_raw; return node ? node->size : 0; }

KEXPORT(fs_register)
KEXPORT(fs_mount)
KEXPORT(fs_probe_node)
KEXPORT(fs_probe)
KEXPORT(fs_mount_auto)
KEXPORT(fs_unmount)
KEXPORT(fs_publish_proc)
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
