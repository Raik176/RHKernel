#include "mod/device.h"

#include "console.h"
#include "file/device.h"
#include "file/vfs.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "mod/logging.h"
#include "string.h"
#include "symbol/ksym.h"

// Internal structure to link VFS to Driver
struct device_instance {
    device_ops *ops;
    void *priv;
};

static uint64_t null_read(void *, uint64_t, uint64_t, uint8_t *) { return 0; }
static uint64_t null_write(void *, uint64_t, uint64_t size, uint8_t *) { return size; }
static uint64_t zero_read(void *, uint64_t, uint64_t size, uint8_t *buffer) {
    memset(buffer, 0, size);
    return size;
}

static device_ops null_ops = {.read = null_read, .write = null_write};
static device_ops zero_ops = {.read = zero_read, .write = null_write};

static uint64_t console_dev_write(void *, uint64_t, uint64_t size, uint8_t *buffer) {
    for (uint64_t i = 0; i < size; i++) { console::putchar(buffer[i]); }

    return size;
}

static device_ops console_ops = {.read = null_read, .write = console_dev_write};

static vfs::vfs_node *dev_root = nullptr;

void init_virt_fs() {
    devfs::init();
    procfs::init();
}

static uint64_t dev_vfs_read(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    device_instance *inst = (device_instance *)node->ptr;
    if (inst && inst->ops && inst->ops->read) {
        return inst->ops->read(inst->priv, offset, size, buffer);
    }
    return 0;
}

static uint64_t dev_vfs_write(vfs::vfs_node *node, uint64_t offset, uint64_t size,
                              uint8_t *buffer) {
    device_instance *inst = (device_instance *)node->ptr;
    if (inst && inst->ops && inst->ops->write) {
        return inst->ops->write(inst->priv, offset, size, buffer);
    }
    return 0;
}

namespace devfs {
    void init() {
        dev_root = vfs::create_node("dev", vfs::VfsType::VFS_DIRECTORY, vfs::get_root());
        devfs_register("console", &console_ops, nullptr);
        devfs_register("null", &null_ops, nullptr);
        devfs_register("zero", &zero_ops, nullptr);
    }
}  // namespace devfs

static uint64_t proc_mem_total_read(vfs::vfs_node *, uint64_t offset, uint64_t size,
                                    uint8_t *buffer) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", pmm::get_total_bytes());

    if (offset >= (uint64_t)len) return 0;
    if (size > (uint64_t)len - offset) size = (uint64_t)len - offset;

    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_mem_available_read(vfs::vfs_node *, uint64_t offset, uint64_t size,
                                        uint8_t *buffer) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", pmm::get_free_bytes());

    if (offset >= (uint64_t)len) return 0;
    if (size > (uint64_t)len - offset) size = (uint64_t)len - offset;

    memcpy(buffer, buf + offset, size);
    return size;
}

namespace procfs {
    void init() {
        vfs::vfs_node *proc_root =
            vfs::create_node("proc", vfs::VfsType::VFS_DIRECTORY, vfs::get_root());
        if (!proc_root) return;

        vfs::vfs_node *mem_dir = vfs::create_node("mem", vfs::VfsType::VFS_DIRECTORY, proc_root);
        if (!mem_dir) return;

        vfs::vfs_node *total_node =
            vfs::create_node("total", vfs::VfsType::VFS_CHAR_DEVICE, mem_dir);
        if (total_node) { total_node->read = proc_mem_total_read; }

        vfs::vfs_node *avail_node =
            vfs::create_node("available", vfs::VfsType::VFS_CHAR_DEVICE, mem_dir);
        if (avail_node) { avail_node->read = proc_mem_available_read; }
    }
}  // namespace procfs

extern "C" int devfs_register(const char *path, device_ops *ops, void *priv) {
    if (!dev_root || !path) return -1;

    vfs::vfs_node *curr = dev_root;
    char path_buf[256];
    strncpy(path_buf, path, 255);

    char *saveptr;
    char *token = strtok_r(path_buf, "/", &saveptr);

    while (token != nullptr) {
        char *next_token = strtok_r(nullptr, "/", &saveptr);

        if (next_token == nullptr) {
            vfs::vfs_node *node = vfs::create_node(token, vfs::VfsType::VFS_CHAR_DEVICE, curr);
            if (!node) return -1;

            // Allocate the bridge instance
            device_instance *inst = (device_instance *)heap::kmalloc(sizeof(device_instance));
            inst->ops = ops;
            inst->priv = priv;

            node->ptr = (uintptr_t)inst;
            node->read = dev_vfs_read;
            node->write = dev_vfs_write;
            return 0;
        } else {
            vfs::vfs_node *dir = vfs::finddir(curr, token);
            if (!dir) dir = vfs::create_node(token, vfs::VfsType::VFS_DIRECTORY, curr);
            curr = dir;
            token = next_token;
        }
    }
    return -1;
}

extern "C" void devfs_unregister(const char *path) {
    if (!dev_root || !path) return;
    vfs::vfs_node *node = vfs::traverse_relative(dev_root, path);
    if (!node || node->type != vfs::VfsType::VFS_CHAR_DEVICE) return;

    // Free the bridge instance
    if (node->ptr) heap::kfree((void *)node->ptr);

    vfs::vfs_node *curr = node;
    while (curr != dev_root) {
        vfs::vfs_node *parent = curr->parent;
        if (!parent) break;

        if (parent->child == curr)
            parent->child = curr->next;
        else {
            vfs::vfs_node *prev = parent->child;
            while (prev && prev->next != curr) prev = prev->next;
            if (prev) prev->next = curr->next;
        }

        heap::kfree(curr->name);
        vfs::vfs_node *to_free = curr;
        if (parent->child != nullptr || parent == dev_root) {
            heap::kfree(to_free);
            break;
        }
        heap::kfree(to_free);
        curr = parent;
    }
}

static struct bus *bus_list = nullptr;
static struct device *device_list = nullptr;
static struct driver *driver_list = nullptr;

static void attempt_match(struct device *dev, struct driver *drv) {
    if (dev->driver || dev->bus != drv->bus) return;

    if (dev->bus->match(dev, drv)) {
        if (drv->probe(dev) == 0) {
            dev->driver = drv;
        } else {
            klog(LOG_ERR, "DEVICE: Driver %s failed to probe %s\n", drv->name, dev->name);
        }
    }
}

void bus_register(struct bus *bus) {
    bus->next = bus_list;
    bus_list = bus;
}

struct bus *find_bus(const char *name) {
    struct bus *bus = bus_list;
    while (bus) {
        if (strcmp(bus->name, name) == 0) return bus;
        bus = bus->next;
    }

    return nullptr;
}

void device_register(struct device *dev) {
    dev->next = device_list;
    device_list = dev;

    struct driver *drv = driver_list;
    while (drv) {
        attempt_match(dev, drv);
        drv = drv->next;
    }
}

void driver_register(struct driver *drv) {
    drv->next = driver_list;
    driver_list = drv;

    struct device *dev = device_list;
    while (dev) {
        attempt_match(dev, drv);
        dev = dev->next;
    }
}

KEXPORT(find_bus)
KEXPORT(bus_register)
KEXPORT(device_register)
KEXPORT(driver_register)
KEXPORT(devfs_register)
KEXPORT(devfs_unregister)