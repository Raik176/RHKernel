#include "mod/device.h"
#include "mod/fs.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "smp/lock.h"
#include "string.h"

#define RAMLOOP_NAME_MAX 32
#define RAMLOOP_CMD_MAX 256
#define RAMLOOP_MAX_DEVS 16
#define RAMLOOP_SECTOR_SIZE 512u
#define RAMLOOP_SECTOR_MASK (RAMLOOP_SECTOR_SIZE - 1u)

struct ramdev {
    char name[RAMLOOP_NAME_MAX];
    uint8_t *data;
    uint64_t size;
    uint32_t index;
    spinlock_t lock;
    bool used;
};

struct loopdev {
    char name[RAMLOOP_NAME_MAX];
    char path[RAMLOOP_CMD_MAX];
    struct vfs_node *node;
    uint64_t size;
    uint32_t index;
    spinlock_t lock;
    bool used;
};

static struct ramdev ramdevs[RAMLOOP_MAX_DEVS];
static struct loopdev loopdevs[RAMLOOP_MAX_DEVS];
static spinlock_t table_lock;

static uint64_t ram_read(void *priv, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t ram_write(void *priv, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t loop_read(void *priv, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t loop_write(void *priv, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t ram_create_write(void *priv, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t ram_delete_write(void *priv, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t ram_size_read(void *priv, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t loop_attach_write(void *priv, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t loop_detach_write(void *priv, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t loop_size_read(void *priv, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t loop_backing_read(void *priv, uint64_t off, uint64_t size, uint8_t *buf);
static int ram_info(void *priv, struct device_info *out);
static int loop_info(void *priv, struct device_info *out);

static struct device_ops ram_ops = {.read = ram_read, .write = ram_write, .info = ram_info};
static struct device_ops loop_ops = {.read = loop_read, .write = loop_write, .info = loop_info};
static struct device_ops ram_create_ops = {.read = NULL, .write = ram_create_write};
static struct device_ops ram_delete_ops = {.read = NULL, .write = ram_delete_write};
static struct device_ops ram_size_ops = {.read = ram_size_read, .write = NULL};
static struct device_ops loop_attach_ops = {.read = NULL, .write = loop_attach_write};
static struct device_ops loop_detach_ops = {.read = NULL, .write = loop_detach_write};
static struct device_ops loop_size_ops = {.read = loop_size_read, .write = NULL};
static struct device_ops loop_backing_ops = {.read = loop_backing_read, .write = NULL};

static bool parse_u64(const char *s, uint64_t *out) {
    if (!s || !*s || !out) return false;
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        uint64_t d = (uint64_t)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
        s++;
    }
    if (*s == 'K' || *s == 'k') { if (v > UINT64_MAX / 1024) return false; v *= 1024; s++; }
    else if (*s == 'M' || *s == 'm') { if (v > UINT64_MAX / (1024 * 1024)) return false; v *= 1024 * 1024; s++; }
    else if (*s == 'G' || *s == 'g') { if (v > UINT64_MAX / (1024ULL * 1024 * 1024)) return false; v *= 1024ULL * 1024 * 1024; s++; }
    if (*s != 0) return false;
    *out = v;
    return true;
}

static int split_cmd(char *buf, char **argv, int max) {
    int argc = 0;
    char *p = buf;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

static bool parse_index_len(const char *s, const char *prefix, size_t prefix_len, uint32_t *out) {
    if (!s || !*s || !prefix || !out) return false;
    if (strncmp(s, prefix, prefix_len) == 0) s += prefix_len;
    if (!*s) return false;
    uint64_t v;
    if (!parse_u64(s, &v) || v >= RAMLOOP_MAX_DEVS) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_ram_index(const char *s, uint32_t *out) { return parse_index_len(s, "ram", 3, out); }
static bool parse_loop_index(const char *s, uint32_t *out) { return parse_index_len(s, "loop", 4, out); }

static uint64_t copy_text_len(const char *text, uint64_t len, uint64_t offset, uint64_t size, uint8_t *buf) {
    if (!text || !buf || offset >= len) return 0;
    if (size > len - offset) size = len - offset;
    memcpy(buf, text + offset, size);
    return size;
}

static uint64_t ram_io(struct ramdev *d, uint64_t off, uint64_t size, uint8_t *buf, bool write) {
    if (!d || !buf || !size || off >= d->size) return 0;
    if (size > d->size - off) size = d->size - off;
    uint64_t flags;
    spinlock_acquire(&d->lock, &flags);
    if (write) memcpy(d->data + off, buf, size);
    else memcpy(buf, d->data + off, size);
    spinlock_release(&d->lock, flags);
    return size;
}

static uint64_t ram_read(void *priv, uint64_t off, uint64_t size, uint8_t *buf) {
    return ram_io((struct ramdev *)priv, off, size, buf, false);
}

static uint64_t ram_write(void *priv, uint64_t off, uint64_t size, uint8_t *buf) {
    return ram_io((struct ramdev *)priv, off, size, buf, true);
}

static uint64_t loop_io(struct loopdev *d, uint64_t off, uint64_t size, uint8_t *buf, bool write) {
    if (!d || !d->node || !buf || !size || off >= d->size) return 0;
    if (size > d->size - off) size = d->size - off;
    uint64_t flags;
    spinlock_acquire(&d->lock, &flags);
    uint64_t r = write ? vfs_write_c(d->node, off, size, buf) : vfs_read_c(d->node, off, size, buf);
    spinlock_release(&d->lock, flags);
    return r;
}

static uint64_t loop_read(void *priv, uint64_t off, uint64_t size, uint8_t *buf) {
    return loop_io((struct loopdev *)priv, off, size, buf, false);
}

static uint64_t loop_write(void *priv, uint64_t off, uint64_t size, uint8_t *buf) {
    return loop_io((struct loopdev *)priv, off, size, buf, true);
}

static void ram_control_path(uint32_t idx, const char *leaf, char *out, size_t out_sz) {
    snprintf(out, out_sz, "ram/%u/%s", idx, leaf);
}

static void loop_control_path(uint32_t idx, const char *leaf, char *out, size_t out_sz) {
    snprintf(out, out_sz, "loop/%u/%s", idx, leaf);
}

static void unregister_ram_controls(uint32_t idx) {
    char path[RAMLOOP_NAME_MAX];
    ram_control_path(idx, "size", path, sizeof(path));
    devfs_unregister(path);
    ram_control_path(idx, "delete", path, sizeof(path));
    devfs_unregister(path);
}

static void unregister_loop_controls(uint32_t idx) {
    char path[RAMLOOP_NAME_MAX];
    loop_control_path(idx, "size", path, sizeof(path));
    devfs_unregister(path);
    loop_control_path(idx, "backing", path, sizeof(path));
    devfs_unregister(path);
    loop_control_path(idx, "detach", path, sizeof(path));
    devfs_unregister(path);
}

static int register_ram_controls(struct ramdev *d) {
    char path[RAMLOOP_NAME_MAX];
    ram_control_path(d->index, "size", path, sizeof(path));
    if (devfs_register(path, &ram_size_ops, d) != 0) return -1;
    ram_control_path(d->index, "delete", path, sizeof(path));
    if (devfs_register(path, &ram_delete_ops, d) != 0) {
        ram_control_path(d->index, "size", path, sizeof(path));
        devfs_unregister(path);
        return -1;
    }
    return 0;
}

static int register_loop_controls(struct loopdev *d) {
    char path[RAMLOOP_NAME_MAX];
    loop_control_path(d->index, "size", path, sizeof(path));
    if (devfs_register(path, &loop_size_ops, d) != 0) return -1;
    loop_control_path(d->index, "backing", path, sizeof(path));
    if (devfs_register(path, &loop_backing_ops, d) != 0) {
        loop_control_path(d->index, "size", path, sizeof(path));
        devfs_unregister(path);
        return -1;
    }
    loop_control_path(d->index, "detach", path, sizeof(path));
    if (devfs_register(path, &loop_detach_ops, d) != 0) {
        unregister_loop_controls(d->index);
        return -1;
    }
    return 0;
}

static int create_ram_index(uint32_t idx, uint64_t size) {
    if (idx >= RAMLOOP_MAX_DEVS || size == 0 || (size & RAMLOOP_SECTOR_MASK)) return -1;

    uint8_t *data = (uint8_t *)kmalloc(size);
    if (!data) return -1;
    memset(data, 0, size);

    char name[RAMLOOP_NAME_MAX];
    snprintf(name, sizeof(name), "ram%u", idx);

    uint64_t flags;
    spinlock_acquire(&table_lock, &flags);
    if (ramdevs[idx].used) {
        spinlock_release(&table_lock, flags);
        kfree(data);
        return -1;
    }
    struct ramdev *d = &ramdevs[idx];
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, sizeof(d->name) - 1);
    d->data = data;
    d->size = size;
    d->index = idx;
    spinlock_init(&d->lock);
    d->used = true;
    spinlock_release(&table_lock, flags);

    if (devfs_register_block(d->name, &ram_ops, d, size) != 0 || register_ram_controls(d) != 0) {
        unregister_ram_controls(idx);
        devfs_unregister(d->name);
        spinlock_acquire(&table_lock, &flags);
        d->used = false;
        d->data = NULL;
        spinlock_release(&table_lock, flags);
        kfree(data);
        return -1;
    }

    klog(LOG_INFO, "ramloop: registered /dev/%s size=%lu\n", d->name, size);
    return 0;
}

static int destroy_ram_index(uint32_t idx) {
    if (idx >= RAMLOOP_MAX_DEVS) return -1;
    uint64_t flags;
    spinlock_acquire(&table_lock, &flags);
    if (!ramdevs[idx].used) {
        spinlock_release(&table_lock, flags);
        return -1;
    }
    struct ramdev *d = &ramdevs[idx];
    uint8_t *data = d->data;
    char name[RAMLOOP_NAME_MAX];
    strncpy(name, d->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    d->used = false;
    d->data = NULL;
    spinlock_release(&table_lock, flags);

    unregister_ram_controls(idx);
    devfs_unregister(name);
    kfree(data);
    return 0;
}

static int attach_loop_index(uint32_t idx, const char *path) {
    if (idx >= RAMLOOP_MAX_DEVS || !path || !*path) return -1;
    struct vfs_node *node = vfs_open_c(path);
    if (!node) return -1;
    uint64_t size = vfs_size_c(node);
    if (size == 0 || (size & RAMLOOP_SECTOR_MASK)) return -1;

    char name[RAMLOOP_NAME_MAX];
    snprintf(name, sizeof(name), "loop%u", idx);

    uint64_t flags;
    spinlock_acquire(&table_lock, &flags);
    if (loopdevs[idx].used) {
        spinlock_release(&table_lock, flags);
        return -1;
    }
    struct loopdev *d = &loopdevs[idx];
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, sizeof(d->name) - 1);
    strncpy(d->path, path, sizeof(d->path) - 1);
    d->node = node;
    d->size = size;
    d->index = idx;
    spinlock_init(&d->lock);
    d->used = true;
    spinlock_release(&table_lock, flags);

    if (devfs_register_block(d->name, &loop_ops, d, size) != 0 || register_loop_controls(d) != 0) {
        unregister_loop_controls(idx);
        devfs_unregister(d->name);
        spinlock_acquire(&table_lock, &flags);
        d->used = false;
        spinlock_release(&table_lock, flags);
        return -1;
    }

    klog(LOG_INFO, "ramloop: attached /dev/%s to %s size=%lu\n", d->name, d->path, size);
    return 0;
}

static int detach_loop_index(uint32_t idx) {
    if (idx >= RAMLOOP_MAX_DEVS) return -1;
    uint64_t flags;
    spinlock_acquire(&table_lock, &flags);
    if (!loopdevs[idx].used) {
        spinlock_release(&table_lock, flags);
        return -1;
    }
    struct loopdev *d = &loopdevs[idx];
    char name[RAMLOOP_NAME_MAX];
    strncpy(name, d->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    d->used = false;
    spinlock_release(&table_lock, flags);

    unregister_loop_controls(idx);
    devfs_unregister(name);
    return 0;
}

static uint64_t ram_create_write(void *, uint64_t, uint64_t size, uint8_t *buf) {
    char tmp[RAMLOOP_CMD_MAX];
    if (!buf || size == 0 || size >= sizeof(tmp)) return 0;
    memcpy(tmp, buf, size);
    tmp[size] = 0;
    char *argv[2];
    int argc = split_cmd(tmp, argv, 2);
    uint32_t idx;
    uint64_t bytes;
    if (argc == 2 && parse_ram_index(argv[0], &idx) && parse_u64(argv[1], &bytes) && create_ram_index(idx, bytes) == 0) return size;
    klog(LOG_WARN, "ramloop: /dev/ram/create expects: <index|ramN> <bytes>\n");
    return 0;
}

static uint64_t ram_delete_write(void *priv, uint64_t, uint64_t size, uint8_t *) {
    struct ramdev *d = (struct ramdev *)priv;
    if (!d) return 0;
    return destroy_ram_index(d->index) == 0 ? size : 0;
}

static uint64_t ram_size_read(void *priv, uint64_t off, uint64_t size, uint8_t *buf) {
    struct ramdev *d = (struct ramdev *)priv;
    if (!d) return 0;
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lu\n", d->size);
    return n > 0 ? copy_text_len(tmp, (uint64_t)n, off, size, buf) : 0;
}

static uint64_t loop_attach_write(void *, uint64_t, uint64_t size, uint8_t *buf) {
    char tmp[RAMLOOP_CMD_MAX];
    if (!buf || size == 0 || size >= sizeof(tmp)) return 0;
    memcpy(tmp, buf, size);
    tmp[size] = 0;
    char *argv[2];
    int argc = split_cmd(tmp, argv, 2);
    uint32_t idx;
    if (argc == 2 && parse_loop_index(argv[0], &idx) && attach_loop_index(idx, argv[1]) == 0) return size;
    klog(LOG_WARN, "ramloop: /dev/loop/attach expects: <index|loopN> <path>\n");
    return 0;
}

static uint64_t loop_detach_write(void *priv, uint64_t, uint64_t size, uint8_t *) {
    struct loopdev *d = (struct loopdev *)priv;
    if (!d) return 0;
    return detach_loop_index(d->index) == 0 ? size : 0;
}

static uint64_t loop_size_read(void *priv, uint64_t off, uint64_t size, uint8_t *buf) {
    struct loopdev *d = (struct loopdev *)priv;
    if (!d) return 0;
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lu\n", d->size);
    return n > 0 ? copy_text_len(tmp, (uint64_t)n, off, size, buf) : 0;
}

static uint64_t loop_backing_read(void *priv, uint64_t off, uint64_t size, uint8_t *buf) {
    struct loopdev *d = (struct loopdev *)priv;
    if (!d) return 0;
    char tmp[RAMLOOP_CMD_MAX + 2];
    int n = snprintf(tmp, sizeof(tmp), "%s\n", d->path);
    return n > 0 ? copy_text_len(tmp, (uint64_t)n, off, size, buf) : 0;
}

static void ramloop_copy_text(char *dst, uint64_t cap, const char *src) {
    if (!dst || !cap) return;
    uint64_t i = 0;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int ram_info(void *priv, struct device_info *out) {
    struct ramdev *d = (struct ramdev *)priv;
    if (!d || !out) return -1;
    out->version = DEVICE_INFO_VERSION;
    out->kind = DEVICE_INFO_KIND_BLOCK;
    out->logical_block_size = RAMLOOP_SECTOR_SIZE;
    out->physical_block_size = RAMLOOP_SECTOR_SIZE;
    out->block_count = d->size / RAMLOOP_SECTOR_SIZE;
    out->size_bytes = d->size;
    out->max_size_bytes = d->size;
    out->max_transfer_bytes = d->size > UINT32_MAX ? UINT32_MAX : (uint32_t)d->size;
    ramloop_copy_text(out->driver, sizeof(out->driver), "ramloop");
    ramloop_copy_text(out->media_type, sizeof(out->media_type), "ram");
    ramloop_copy_text(out->type, sizeof(out->type), "disk");
    return 0;
}

static int loop_info(void *priv, struct device_info *out) {
    struct loopdev *d = (struct loopdev *)priv;
    if (!d || !out) return -1;
    out->version = DEVICE_INFO_VERSION;
    out->kind = DEVICE_INFO_KIND_BLOCK;
    out->logical_block_size = RAMLOOP_SECTOR_SIZE;
    out->physical_block_size = RAMLOOP_SECTOR_SIZE;
    out->block_count = d->size / RAMLOOP_SECTOR_SIZE;
    out->size_bytes = d->size;
    out->max_size_bytes = d->size;
    out->max_transfer_bytes = d->size > UINT32_MAX ? UINT32_MAX : (uint32_t)d->size;
    ramloop_copy_text(out->driver, sizeof(out->driver), "ramloop");
    ramloop_copy_text(out->media_type, sizeof(out->media_type), "loop");
    ramloop_copy_text(out->type, sizeof(out->type), "disk");
    ramloop_copy_text(out->parent, sizeof(out->parent), d->path);
    return 0;
}

static int ramloop_init(void) {
    spinlock_init(&table_lock);
    devfs_register("ram/create", &ram_create_ops, NULL);
    devfs_register("loop/attach", &loop_attach_ops, NULL);
    return 0;
}


MODULE_INFO("ramloop", ramloop_init, 0, NULL);
