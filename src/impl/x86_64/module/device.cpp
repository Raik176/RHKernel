#include "mod/device.h"

#include "console.h"
#include "framebuffer.h"
#include "display.h"
#include "file/device.h"
#include "file/vfs.h"
#include "file/module_loader.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "mod/fs.h"
#include "mod/logging.h"
#include "power.h"
#include "security/random.h"
#include "smp/scheduler.h"
#include "smp/smp.h"
#include "smp/workqueue.h"
#include "string.h"
#include "symbol/ksym.h"

// Internal structure to link VFS to Driver
struct device_instance {
    device_ops *ops;
    void *priv;
    bool removed;
    device_info info;
};

static uint64_t null_read(void *, uint64_t, uint64_t, uint8_t *) { return 0; }
static uint64_t null_write(void *, uint64_t, uint64_t size, uint8_t *) { return size; }
static uint64_t zero_read(void *, uint64_t, uint64_t size, uint8_t *buffer) {
    memset(buffer, 0, size);
    return size;
}

static device_ops null_ops = {.read = null_read, .write = null_write, .mmap = NULL, .open = NULL, .close = NULL, .read_file = NULL, .write_file = NULL, .mmap_file = NULL, .info = NULL};
static device_ops zero_ops = {.read = zero_read, .write = null_write, .mmap = NULL, .open = NULL, .close = NULL, .read_file = NULL, .write_file = NULL, .mmap_file = NULL, .info = NULL};

static uint64_t random_read(void *, uint64_t, uint64_t size, uint8_t *buffer) {
    random::fill(buffer, size);
    return size;
}

static device_ops random_ops = {.read = random_read, .write = null_write, .mmap = NULL, .open = NULL, .close = NULL, .read_file = NULL, .write_file = NULL, .mmap_file = NULL, .info = NULL};

static uint64_t console_dev_write(void *, uint64_t, uint64_t size, uint8_t *buffer) {
    for (uint64_t i = 0; i < size; i++) { console::putchar(buffer[i]); }

    return size;
}

static device_ops console_ops = {.read = null_read, .write = console_dev_write, .mmap = NULL, .open = NULL, .close = NULL, .read_file = NULL, .write_file = NULL, .mmap_file = NULL, .info = NULL};

enum class PowerCommand {
    Invalid,
    Poweroff,
    Restart,
};

static PowerCommand parse_power_command(const uint8_t *buffer, uint64_t size) {
    if (!buffer || size == 0) return PowerCommand::Invalid;

    char cmd[32];
    uint64_t n = size;
    if (n >= sizeof(cmd)) n = sizeof(cmd) - 1;
    memcpy(cmd, buffer, n);
    cmd[n] = 0;

    while (n > 0 && (cmd[n - 1] == '\n' || cmd[n - 1] == '\r' || cmd[n - 1] == ' ' || cmd[n - 1] == '\t')) {
        cmd[--n] = 0;
    }

    char *begin = cmd;
    while (*begin == ' ' || *begin == '\t' || *begin == '\n' || *begin == '\r') begin++;

    if (strcmp(begin, "poweroff") == 0 || strcmp(begin, "shutdown") == 0 || strcmp(begin, "off") == 0 ||
        strcmp(begin, "halt") == 0 || strcmp(begin, "0") == 0) {
        return PowerCommand::Poweroff;
    }

    if (strcmp(begin, "restart") == 0 || strcmp(begin, "reboot") == 0 || strcmp(begin, "reset") == 0 ||
        strcmp(begin, "1") == 0) {
        return PowerCommand::Restart;
    }

    return PowerCommand::Invalid;
}

static uint64_t power_device_write(void *, uint64_t, uint64_t size, uint8_t *buffer) {
    switch (parse_power_command(buffer, size)) {
        case PowerCommand::Poweroff:
            power::shutdown();
            __builtin_unreachable();
        case PowerCommand::Restart:
            power::restart();
            __builtin_unreachable();
        case PowerCommand::Invalid:
        default:
            console::printf("power: invalid command; write 'poweroff' or 'restart' to /dev/power\n");
            return 0;
    }
}

static uint64_t power_device_read(void *, uint64_t offset, uint64_t size, uint8_t *buffer) {
    static const char msg[] =
        "power control device\n"
        "write 'poweroff' to power off\n"
        "write 'restart' to reboot\n";
    uint64_t len = sizeof(msg) - 1;
    if (offset >= len) return 0;
    if (size > len - offset) size = len - offset;
    memcpy(buffer, msg + offset, size);
    return size;
}

static device_ops power_ops = {.read = power_device_read, .write = power_device_write, .mmap = NULL, .open = NULL, .close = NULL, .read_file = NULL, .write_file = NULL, .mmap_file = NULL, .info = NULL};

static spinlock_t fb_lease_lock;
static uint64_t fb_lease_owner = 0;
static uint32_t fb_lease_refs = 0;

struct fb_file_state {
    uint64_t task_id;
    bool owns_lease;
};

static uint64_t current_task_id() {
    scheduler::task *task = smp::get_cpu() ? smp::get_cpu()->current_task : nullptr;
    return task ? task->id : 0;
}

static int fb_open(void *, void **ctx) {
    if (!ctx) return -1;
    fb_file_state *state = (fb_file_state *)heap::kzalloc(sizeof(*state));
    if (!state) return -1;
    state->task_id = current_task_id();
    if (state->task_id == 0) {
        heap::kfree(state);
        return -1;
    }
    *ctx = state;
    return 0;
}

static void fb_drop_lease(fb_file_state *state) {
    if (!state || !state->owns_lease) return;

    uint64_t flags;
    fb_lease_lock.acquire(flags);
    bool release_console = false;
    if (fb_lease_owner == state->task_id && fb_lease_refs > 0) {
        fb_lease_refs--;
        if (fb_lease_refs == 0) {
            fb_lease_owner = 0;
            release_console = true;
        }
    }
    state->owns_lease = false;
    fb_lease_lock.release(flags);
    if (release_console) framebuffer::release_console_claim(state->task_id);
}

static void fb_close(void *, void *ctx) {
    fb_file_state *state = (fb_file_state *)ctx;
    fb_drop_lease(state);
    if (state) heap::kfree(state);
}

static uint64_t fb_read(void *, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!buffer) return 0;
    fb_mode mode;
    if (!framebuffer::info(&mode)) return 0;
    uint64_t len = sizeof(mode);
    if (offset >= len) return 0;
    if (size > len - offset) size = len - offset;
    memcpy(buffer, ((uint8_t *)&mode) + offset, size);
    return size;
}

static int fb_mmap_file(void *, void *ctx, uint64_t offset, uint64_t size, device_mmap_result *out) {
    fb_file_state *state = (fb_file_state *)ctx;
    if (!state || !out || !state->owns_lease) return -1;

    uint64_t lock_flags;
    fb_lease_lock.acquire(lock_flags);
    bool leased = fb_lease_owner == state->task_id && fb_lease_refs > 0;
    fb_lease_lock.release(lock_flags);
    if (!leased) return -1;

    uint64_t phys = 0;
    uint64_t bytes = 0;
    if (!framebuffer::map_region(offset, size, &phys, &bytes)) return -1;
    out->phys = phys;
    out->size = bytes;
    out->flags = DEVICE_MMAP_WRITE_COMBINING;
    return 0;
}

static bool fb_state_owns_lease(fb_file_state *state) {
    if (!state || !state->owns_lease) return false;
    uint64_t flags;
    fb_lease_lock.acquire(flags);
    bool ok = fb_lease_owner == state->task_id && fb_lease_refs > 0;
    fb_lease_lock.release(flags);
    return ok;
}

static uint64_t fb_write_file(void *, void *ctx, uint64_t, uint64_t size, uint8_t *buffer) {
    fb_file_state *state = (fb_file_state *)ctx;
    if (!state || !buffer || size != sizeof(fb_command)) return 0;

    fb_command cmd;
    memcpy(&cmd, buffer, sizeof(cmd));

    uint64_t flags;
    switch (cmd.command) {
        case FB_CMD_ACQUIRE: {
            if (cmd.flags || cmd.x || cmd.y || cmd.width || cmd.height) return 0;
            fb_lease_lock.acquire(flags);
            bool ok = fb_lease_owner == 0 || fb_lease_owner == state->task_id;
            if (ok && !state->owns_lease) {
                fb_lease_owner = state->task_id;
                fb_lease_refs++;
                state->owns_lease = true;
            }
            fb_lease_lock.release(flags);
            if (!ok) return 0;
            framebuffer::claim_console(true);
            return size;
        }
        case FB_CMD_RELEASE:
            if (cmd.flags || cmd.x || cmd.y || cmd.width || cmd.height || !fb_state_owns_lease(state)) return 0;
            fb_drop_lease(state);
            return size;
        case FB_CMD_DIRTY:
            if (cmd.flags || !fb_state_owns_lease(state)) return 0;
            return framebuffer::dirty(cmd.x, cmd.y, cmd.width, cmd.height) ? size : 0;
        case FB_CMD_PRESENT:
            if (cmd.x || cmd.y || cmd.width || cmd.height || !fb_state_owns_lease(state)) return 0;
            return framebuffer::present(cmd.flags) ? size : 0;
        default:
            return 0;
    }
}

void devfs_release_task_resources(uint64_t task_id) {
    if (task_id == 0) return;
    uint64_t flags;
    fb_lease_lock.acquire(flags);
    bool release_fb = fb_lease_owner == task_id;
    if (release_fb) {
        fb_lease_owner = 0;
        fb_lease_refs = 0;
    }
    fb_lease_lock.release(flags);
    if (release_fb) framebuffer::release_console_claim(task_id);
}

static device_ops fb_ops = {
    .read = fb_read,
    .write = NULL,
    .mmap = NULL,
    .open = fb_open,
    .close = fb_close,
    .read_file = NULL,
    .write_file = fb_write_file,
    .mmap_file = fb_mmap_file,
    .info = NULL,
};

static vfs::vfs_node *dev_root = nullptr;
static vfs::vfs_node *sys_root = nullptr;
static struct bus *bus_list = nullptr;
static struct device *device_list = nullptr;
static struct driver *driver_list = nullptr;
static spinlock_t proc_snapshot_lock;
static char proc_cpu_debug_buf[8192];
static char proc_cpu_feature_buf[8192];
static char proc_cpuid_raw_buf[32768];

static uint64_t proc_copy_static(char *buf, uint64_t len, uint64_t offset, uint64_t size,
                                 uint8_t *out);
static void proc_appendf(char *buf, uint64_t cap, uint64_t *pos, const char *fmt, ...);
static void cpuid_leaf(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
                       uint32_t *ecx, uint32_t *edx);

static bool path_component_valid(const char *s) {
    return s && s[0] != '\0' && strcmp(s, ".") != 0 && strcmp(s, "..") != 0;
}

static void device_node_release(vfs::vfs_node *node) {
    if (!node) return;
    device_instance *inst = (device_instance *)node->ptr;
    if (inst) {
        node->ptr = 0;
        heap::kfree(inst);
    }
}

static void vfs_detach_and_put(vfs::vfs_node *node) {
    if (!node) return;
    vfs::detach_node(node);
    vfs::put_node(node);
}

static void cleanup_empty_dev_dirs(vfs::vfs_node *dir) {
    while (dir && dir != dev_root && dir->type == vfs::VfsType::VFS_DIRECTORY && !dir->child) {
        vfs::vfs_node *parent = dir->parent;
        vfs_detach_and_put(dir);
        dir = parent;
    }
}

bool devfs_is_device_node(vfs::vfs_node *node) {
    if (!node || (node->type != vfs::VfsType::VFS_CHAR_DEVICE &&
                  node->type != vfs::VfsType::VFS_BLOCK_DEVICE))
        return false;
    for (vfs::vfs_node *cur = node; cur; cur = cur->parent) {
        if (cur == dev_root) return true;
    }
    return false;
}

void init_virt_fs() {
    devfs::init();
    procfs::init();
    sysfs::init();
}

static uint64_t dev_vfs_read(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    device_instance *inst = (device_instance *)node->ptr;
    if (inst && !inst->removed && inst->ops && inst->ops->read) {
        return inst->ops->read(inst->priv, offset, size, buffer);
    }
    return 0;
}

static uint64_t dev_vfs_write(vfs::vfs_node *node, uint64_t offset, uint64_t size,
                              uint8_t *buffer) {
    device_instance *inst = (device_instance *)node->ptr;
    if (inst && !inst->removed && inst->ops && inst->ops->write) {
        return inst->ops->write(inst->priv, offset, size, buffer);
    }
    return 0;
}

namespace devfs {
    void init() {
        spinlock_init(&fb_lease_lock);
        dev_root = vfs::create_node("dev", vfs::VfsType::VFS_DIRECTORY, vfs::get_root());
        devfs_register("console", &console_ops, nullptr);
        devfs_register("null", &null_ops, nullptr);
        devfs_register("zero", &zero_ops, nullptr);
        devfs_register("random", &random_ops, nullptr);
        devfs_register("power", &power_ops, nullptr);
        devfs_register("fb0", &fb_ops, nullptr);
    }
}  // namespace devfs

enum class VirtStat : uint64_t {
    MemTotal,
    MemAvailable,
    MemUsed,
    MemManaged,
    MemSystem,
    MemPhysicalLimit,
    VmmDirectMap,
    VmmPageTableBytes,
    VmmPhysMapBase,
    VmmPagingLevels,
    VmmVirtualAddressBits,
    VmmPage1G,
    VmmDemandZero,
    VmmGuardPage,
    VmmPat,
    VmmWriteCombining,
    VmmNx,
    PmmPageSize,
    PmmMaxOrder,
    PmmApTrampoline,
    PmmDeferredSpans,
    HeapSlabPageBytes,
    HeapLargeAllocBytes,
    HeapLargeAllocCount,
    CpuCount,
};

static void mem_snapshot(size_t *system, size_t *managed, size_t *free, size_t *physical_limit,
                         size_t *deferred) {
    pmm::DebugInfo info;
    pmm::get_debug_info(&info);
    if (system) *system = info.system_bytes;
    if (managed) *managed = info.managed_bytes;
    if (free) *free = info.free_bytes;
    if (physical_limit) *physical_limit = info.physical_limit_bytes;
    if (deferred) *deferred = info.deferred_span_count;
}

static uint64_t virt_stat_value(VirtStat stat) {
    size_t system = 0, managed = 0, free = 0, physical_limit = 0, deferred = 0;
    switch (stat) {
        case VirtStat::MemTotal: return pmm::get_total_bytes();
        case VirtStat::MemAvailable: return pmm::get_free_bytes();
        case VirtStat::MemUsed:
            mem_snapshot(&system, &managed, &free, &physical_limit, &deferred);
            return managed >= free ? managed - free : 0;
        case VirtStat::MemManaged:
            mem_snapshot(&system, &managed, &free, &physical_limit, &deferred);
            return managed;
        case VirtStat::MemSystem:
            mem_snapshot(&system, &managed, &free, &physical_limit, &deferred);
            return system;
        case VirtStat::MemPhysicalLimit:
            mem_snapshot(&system, &managed, &free, &physical_limit, &deferred);
            return physical_limit;
        case VirtStat::VmmDirectMap: return vmm::direct_map_bytes();
        case VirtStat::VmmPageTableBytes: return vmm::page_table_bytes();
        case VirtStat::VmmPhysMapBase: return paging_phys_map_base_value();
        case VirtStat::VmmPagingLevels: return vmm::paging_level_count();
        case VirtStat::VmmVirtualAddressBits: return vmm::virtual_address_bits();
        case VirtStat::VmmPage1G: return vmm::page_1g_supported() ? 1 : 0;
        case VirtStat::VmmDemandZero: return vmm::demand_zero_supported() ? 1 : 0;
        case VirtStat::VmmGuardPage: return vmm::guard_page_supported() ? 1 : 0;
        case VirtStat::VmmPat: return vmm::pat_supported() ? 1 : 0;
        case VirtStat::VmmWriteCombining: return vmm::write_combining_supported() ? 1 : 0;
        case VirtStat::VmmNx: return vmm::nx_supported() ? 1 : 0;
        case VirtStat::PmmPageSize: return pmm::PAGE_SIZE;
        case VirtStat::PmmMaxOrder: return pmm::get_max_order();
        case VirtStat::PmmApTrampoline: return pmm::get_ap_trampoline_page();
        case VirtStat::PmmDeferredSpans:
            mem_snapshot(&system, &managed, &free, &physical_limit, &deferred);
            return deferred;
        case VirtStat::HeapSlabPageBytes: {
            heap::DebugInfo info;
            heap::get_debug_info(&info);
            return info.slab_page_bytes;
        }
        case VirtStat::HeapLargeAllocBytes: {
            heap::DebugInfo info;
            heap::get_debug_info(&info);
            return info.large_alloc_bytes;
        }
        case VirtStat::HeapLargeAllocCount: {
            heap::DebugInfo info;
            heap::get_debug_info(&info);
            return info.large_alloc_count;
        }
        case VirtStat::CpuCount: return smp::get_core_count();
    }
    return 0;
}

static uint64_t virt_stat_read(vfs::vfs_node *node, uint64_t offset, uint64_t size,
                               uint8_t *buffer) {
    char buf[32];
    VirtStat stat = (VirtStat)(node ? node->ptr : 0);
    int len = snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)virt_stat_value(stat));
    return proc_copy_static(buf, len > 0 ? (uint64_t)len : 0, offset, size, buffer);
}

static uint64_t proc_cpu_vendor_read(vfs::vfs_node *, uint64_t offset, uint64_t size,
                                     uint8_t *buffer) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid_leaf(0, 0, &eax, &ebx, &ecx, &edx);
    char buf[16];
    memcpy(buf + 0, &ebx, 4);
    memcpy(buf + 4, &edx, 4);
    memcpy(buf + 8, &ecx, 4);
    buf[12] = '\n';
    buf[13] = 0;
    return proc_copy_static(buf, 13, offset, size, buffer);
}



static void proc_devices_append(char *buf, uint64_t cap, uint64_t *pos, const char *s) {
    while (*s && *pos + 1 < cap) buf[(*pos)++] = *s++;
    if (*pos < cap) buf[*pos] = '\0';
}

static void device_info_seed(device_info *info, vfs::vfs_node *node) {
    if (!info || !node) return;
    memset(info, 0, sizeof(*info));
    info->version = DEVICE_INFO_VERSION;
    info->size_bytes = node->size;
    info->max_size_bytes = node->size;
    if (node->type == vfs::VfsType::VFS_BLOCK_DEVICE) {
        info->kind = DEVICE_INFO_KIND_BLOCK;
        info->logical_block_size = 512;
        info->physical_block_size = 512;
        info->block_count = node->size / 512;
    }
}

static void device_info_complete(device_info *info) {
    if (!info) return;
    if (!info->version) info->version = DEVICE_INFO_VERSION;
    if (!info->logical_block_size) info->logical_block_size = 512;
    if (!info->physical_block_size) info->physical_block_size = info->logical_block_size;
    if (!info->size_bytes && info->block_count && info->logical_block_size)
        info->size_bytes = info->block_count * (uint64_t)info->logical_block_size;
    if (!info->block_count && info->size_bytes && info->logical_block_size)
        info->block_count = info->size_bytes / info->logical_block_size;
    if (!info->max_size_bytes) info->max_size_bytes = info->size_bytes;
}

static int device_get_info(vfs::vfs_node *node, device_info *out) {
    if (!node || !out || (node->type != vfs::VfsType::VFS_CHAR_DEVICE &&
                          node->type != vfs::VfsType::VFS_BLOCK_DEVICE)) return -1;
    device_instance *inst = (device_instance *)node->ptr;
    if (!inst || inst->removed || !inst->ops) return -1;
    device_info_seed(out, node);
    if (inst->ops->info && inst->ops->info(inst->priv, out) != 0) return -1;
    device_info_complete(out);
    return 0;
}

static void proc_devices_append_node(char *buf, uint64_t cap, uint64_t *pos,
                                     vfs::vfs_node *node, const char *prefix) {
    if (!node) return;

    char path[256];
    if (prefix && prefix[0]) {
        snprintf(path, sizeof(path), "%s/%s", prefix, node->name);
    } else {
        snprintf(path, sizeof(path), "%s", node->name);
    }

    if (node->type == vfs::VfsType::VFS_CHAR_DEVICE || node->type == vfs::VfsType::VFS_BLOCK_DEVICE) {
        char line[320];
        const char *kind = (node->type == vfs::VfsType::VFS_BLOCK_DEVICE) ? "block" : "char";
        snprintf(line, sizeof(line), "%s %s %x:%x\n", kind, path,
                 (uint32_t)(node->size >> 32), (uint32_t)node->size);
        proc_devices_append(buf, cap, pos, line);
    }

    for (vfs::vfs_node *child = node->child; child; child = child->next) {
        proc_devices_append_node(buf, cap, pos, child, path);
    }
}

static uint64_t proc_devices_read(vfs::vfs_node *, uint64_t offset, uint64_t size,
                                  uint8_t *buffer) {
    char buf[4096];
    uint64_t len = 0;
    buf[0] = '\0';
    if (dev_root) {
        for (vfs::vfs_node *child = dev_root->child; child; child = child->next) {
            proc_devices_append_node(buf, sizeof(buf), &len, child, "");
        }
    }

    if (offset >= len) return 0;
    if (size > len - offset) size = len - offset;
    memcpy(buffer, buf + offset, size);
    return size;
}

static void proc_disks_append_node(char *buf, uint64_t cap, uint64_t *pos,
                                   vfs::vfs_node *node, const char *prefix) {
    if (!node) return;

    char path[256];
    if (prefix && prefix[0]) snprintf(path, sizeof(path), "%s/%s", prefix, node->name);
    else snprintf(path, sizeof(path), "%s", node->name);

    if (node->type == vfs::VfsType::VFS_BLOCK_DEVICE) {
        device_info info;
        if (device_get_info(node, &info) == 0) {
            proc_appendf(buf, cap, pos,
                         "%s kind=%s driver=%s media=%s scheme=%s type=%s size=%lu max=%lu block=%u physical=%u blocks=%lu start_lba=%lu parent_size=%lu uuid=%s parent=%s flags=%x\n",
                         path, info.kind == DEVICE_INFO_KIND_PARTITION ? "partition" : "block",
                         info.driver[0] ? info.driver : "?", info.media_type[0] ? info.media_type : "?",
                         info.scheme[0] ? info.scheme : "-", info.type[0] ? info.type : "-",
                         info.size_bytes, info.max_size_bytes, info.logical_block_size,
                         info.physical_block_size, info.block_count, info.start_lba,
                         info.parent_size_bytes, info.uuid[0] ? info.uuid : "-",
                         info.parent[0] ? info.parent : "-", info.flags);
        }
    }

    for (vfs::vfs_node *child = node->child; child; child = child->next)
        proc_disks_append_node(buf, cap, pos, child, path);
}

static uint64_t proc_disks_read(vfs::vfs_node *, uint64_t offset, uint64_t size,
                                uint8_t *buffer) {
    char buf[8192];
    uint64_t len = 0;
    buf[0] = 0;
    if (dev_root) {
        for (vfs::vfs_node *child = dev_root->child; child; child = child->next)
            proc_disks_append_node(buf, sizeof(buf), &len, child, "");
    }
    return proc_copy_static(buf, len, offset, size, buffer);
}

static uint64_t proc_copy_static(char *buf, uint64_t len, uint64_t offset, uint64_t size,
                                 uint8_t *out) {
    if (offset >= len) return 0;
    if (size > len - offset) size = len - offset;
    memcpy(out, buf + offset, size);
    return size;
}

static void proc_append(char *buf, uint64_t cap, uint64_t *pos, const char *s) {
    while (*s && *pos + 1 < cap) buf[(*pos)++] = *s++;
    if (*pos < cap) buf[*pos] = 0;
}

static void proc_appendf(char *buf, uint64_t cap, uint64_t *pos, const char *fmt, ...) {
    if (!buf || !pos || *pos >= cap) return;

    char line[384];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    if (len <= 0) return;
    proc_append(buf, cap, pos, line);
}

static uint64_t proc_cpu_debug_read(vfs::vfs_node *, uint64_t offset, uint64_t size,
                                    uint8_t *buffer) {
    uint64_t flags;
    proc_snapshot_lock.acquire(flags);
    uint64_t len = 0;
    proc_cpu_debug_buf[0] = 0;

    uint64_t cores = smp::get_core_count();
    proc_appendf(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len, "cores: %llu\n", (unsigned long long)cores);

    for (uint64_t i = 0; i < cores; i++) {
        smp::cpu_local *cpu = smp::get_cpu_by_index(i);
        if (!cpu) continue;

        uint32_t queued[scheduler::MAX_QUEUES];
        uint64_t oldest[scheduler::MAX_QUEUES];
        uint32_t runnable = 0;
        uint64_t steal_attempts = 0;
        uint64_t steal_successes = 0;
        uint64_t steal_locked = 0;
        uint64_t steal_empty = 0;
        bool sched_locked = false;
        uint64_t sched_flags;
        if (cpu->sched_lock.try_acquire(sched_flags)) {
            for (int q = 0; q < scheduler::MAX_QUEUES; q++) {
                queued[q] = cpu->task_queue_count[q];
                oldest[q] = cpu->task_queue_oldest_tick[q];
            }
            runnable = cpu->runnable_count;
            steal_attempts = cpu->steal_attempts;
            steal_successes = cpu->steal_successes;
            steal_locked = cpu->steal_locked;
            steal_empty = cpu->steal_empty;
            cpu->sched_lock.release(sched_flags);
        } else {
            sched_locked = true;
            for (int q = 0; q < scheduler::MAX_QUEUES; q++) {
                queued[q] = 0;
                oldest[q] = 0;
            }
        }

        proc_appendf(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len,
                     "cpu%llu: lapic=%u ticks=%llu current=%llu mail_depth=%llu idle=%llu features=pge:%d smep:%d smap:%d umip:%d pat:%d wc:%d xsave:%d avx:%d avx2:%d fma:%d f16c:%d avx512:%d fsgsbase:%d pku:%d\n",
                     (unsigned long long)i, cpu->lapic_id, (unsigned long long)cpu->ticks,
                     (unsigned long long)(cpu->current_task ? cpu->current_task->id : UINT64_MAX),
                     (unsigned long long)cpu->mail_depth,
                     (unsigned long long)(cpu->idle_task ? cpu->idle_task->id : UINT64_MAX),
                     cpu->cpu_features.pge ? 1 : 0, cpu->cpu_features.smep ? 1 : 0,
                     cpu->cpu_features.smap ? 1 : 0, cpu->cpu_features.umip ? 1 : 0,
                     cpu->cpu_features.pat ? 1 : 0, cpu->cpu_features.wc ? 1 : 0,
                     cpu->cpu_features.xsave ? 1 : 0, cpu->cpu_features.avx ? 1 : 0,
                     cpu->cpu_features.avx2 ? 1 : 0, cpu->cpu_features.fma ? 1 : 0,
                     cpu->cpu_features.f16c ? 1 : 0, cpu->cpu_features.avx512 ? 1 : 0,
                     cpu->cpu_features.fsgsbase ? 1 : 0, cpu->cpu_features.pku ? 1 : 0);
        if (sched_locked) {
            proc_append(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len,
                        "  queues: locked sleep_head=");
        } else {
            proc_appendf(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len,
                         "  queues: runnable=%u q0=%u oldest=%llu q1=%u oldest=%llu q2=%u oldest=%llu q3=%u oldest=%llu steals=%llu ok=%llu locked=%llu empty=%llu sleep_head=",
                         runnable, queued[0], (unsigned long long)oldest[0],
                         queued[1], (unsigned long long)oldest[1],
                         queued[2], (unsigned long long)oldest[2],
                         queued[3], (unsigned long long)oldest[3],
                         (unsigned long long)steal_attempts,
                         (unsigned long long)steal_successes,
                         (unsigned long long)steal_locked,
                         (unsigned long long)steal_empty);
        }
        if (cpu->sleep_list_head) {
            proc_appendf(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len,
                         "%llu\n", (unsigned long long)cpu->sleep_list_head->id);
        } else {
            proc_append(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len, "none\n");
        }
        proc_appendf(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len,
                     "  mail: enqueued=%llu handled=%llu busy=%llu invalid=%llu coalesced=%llu resched_req=%llu ipi=%llu deferred=%llu switches=%llu pending=%u\n",
                     (unsigned long long)cpu->mail_enqueued,
                     (unsigned long long)cpu->mail_handled,
                     (unsigned long long)cpu->mail_busy,
                     (unsigned long long)cpu->mail_invalid,
                     (unsigned long long)cpu->mail_coalesced,
                     (unsigned long long)cpu->reschedule_requests,
                     (unsigned long long)cpu->reschedule_ipis,
                     (unsigned long long)cpu->reschedule_deferred,
                     (unsigned long long)cpu->reschedule_switches,
                     cpu->reschedule_pending ? 1 : 0);
    }

    uint64_t ret = proc_copy_static(proc_cpu_debug_buf, len, offset, size, buffer);
    proc_snapshot_lock.release(flags);
    return ret;
}

static uint64_t proc_workqueue_read(vfs::vfs_node *, uint64_t offset, uint64_t size,
                                    uint8_t *buffer) {
    char buf[512];
    uint64_t len = workqueue::format_status(buf, sizeof(buf));
    return proc_copy_static(buf, len, offset, size, buffer);
}

static uint64_t pmm_order_stat_read(vfs::vfs_node *node, uint64_t offset, uint64_t size,
                                    uint8_t *buffer) {
    pmm::DebugInfo info;
    pmm::get_debug_info(&info);
    size_t encoded = node ? (size_t)node->ptr : 0;
    size_t order = encoded >> 1;
    size_t value = 0;
    if (order <= info.max_order) {
        value = (encoded & 1) ? info.free_bytes_by_order[order] : info.free_blocks[order];
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%zu\n", value);
    return proc_copy_static(buf, len > 0 ? (uint64_t)len : 0, offset, size, buffer);
}


static bool parse_u64_strict(const char *s, uint64_t *out) {
    if (!s || !*s || !out) return false;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        uint64_t d = (uint64_t)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

static void u64_to_dec(uint64_t v, char *out, uint64_t cap) {
    if (!out || cap == 0) return;
    char tmp[32];
    uint64_t n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < sizeof(tmp));
    uint64_t p = 0;
    while (n && p + 1 < cap) out[p++] = tmp[--n];
    out[p] = 0;
}

static uint64_t proc_task_status_one_read(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[2048];
    uint64_t len = scheduler::format_task_status(buf, sizeof(buf), node ? node->ptr : UINT64_MAX);
    return proc_copy_static(buf, len, offset, size, buffer);
}

static uint64_t proc_task_maps_one_read(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[8192];
    uint64_t len = scheduler::format_task_maps(buf, sizeof(buf), node ? node->ptr : UINT64_MAX);
    return proc_copy_static(buf, len, offset, size, buffer);
}

static uint64_t proc_task_fds_one_read(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[4096];
    uint64_t len = scheduler::format_task_fds(buf, sizeof(buf), node ? node->ptr : UINT64_MAX);
    return proc_copy_static(buf, len, offset, size, buffer);
}

static vfs::vfs_node *make_transient_node(const char *name, vfs::VfsType type, vfs::vfs_node *parent) {
    vfs::vfs_node *node = (vfs::vfs_node *)heap::kzalloc(sizeof(vfs::vfs_node));
    if (!node) return nullptr;
    node->magic = vfs::VFS_NODE_MAGIC;
    node->name = strdup(name ? name : "");
    if (!node->name) { heap::kfree(node); return nullptr; }
    node->name_len = strlen(node->name);
    node->type = type;
    node->parent = parent;
    return node;
}

static uint64_t task_launch_search_read(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[4096];
    uint64_t len = scheduler::format_launch_search(buf, sizeof(buf), node ? node->ptr : UINT64_MAX);
    return proc_copy_static(buf, len, offset, size, buffer);
}

static uint64_t task_launch_search_write(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer || offset != 0 || size == 0 || size > 4096) return 0;
    return scheduler::set_launch_search_from_text(node->ptr, (const char *)buffer, size) ? size : 0;
}

static vfs::vfs_node *task_launch_finddir(vfs::vfs_node *parent, const char *name) {
    if (!parent || strcmp(name, "search") != 0) return nullptr;
    vfs::vfs_node *node = make_transient_node(name, vfs::VfsType::VFS_CHAR_DEVICE, parent);
    if (!node) return nullptr;
    node->ptr = parent->ptr;
    node->read = task_launch_search_read;
    node->write = task_launch_search_write;
    return node;
}

static int task_launch_readdir(vfs::vfs_node *, uint64_t index, vfs::vfs_dirent *out) {
    if (!out || index != 0) return -1;
    const char *name = "search";
    char *dst = out->name;
    uint64_t cap = out->name_capacity;
    uint64_t len = strlen(name);
    memset(out, 0, sizeof(*out));
    out->type = (uint32_t)vfs::VfsType::VFS_CHAR_DEVICE;
    out->name_len = len;
    out->name = dst;
    out->name_capacity = cap;
    if (dst && cap > len) memcpy(dst, name, len + 1);
    return 0;
}



static uint64_t task_events_inbox_read(vfs::vfs_node *node, uint64_t, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer || size < sizeof(task_event)) return 0;
    uint64_t count = size / sizeof(task_event);
    uint64_t got = scheduler::read_event(node->ptr, (task_event *)buffer, count, true);
    return got * sizeof(task_event);
}

static uint64_t task_events_inbox_write(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer || offset != 0 || size == 0 || (size % sizeof(task_event)) != 0) return 0;
    uint64_t count = size / sizeof(task_event);
    uint64_t sent = scheduler::send_event(node->ptr, (const task_event *)buffer, count);
    return sent * sizeof(task_event);
}

static uint64_t task_events_faultctl_read(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer || offset != 0 || size < sizeof(task_faultctl)) return 0;
    task_faultctl ctl{};
    if (!scheduler::read_faultctl(node->ptr, &ctl)) return 0;
    memcpy(buffer, &ctl, sizeof(ctl));
    return sizeof(ctl);
}

static uint64_t task_events_faultctl_write(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer || offset != 0 || size != sizeof(task_faultctl)) return 0;
    return scheduler::write_faultctl(node->ptr, (const task_faultctl *)buffer) ? size : 0;
}

static uint64_t task_events_fault_read(vfs::vfs_node *node, uint64_t, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer || size < sizeof(task_fault_frame)) return 0;
    uint64_t got = scheduler::read_fault(node->ptr, (task_fault_frame *)buffer, 1);
    return got * sizeof(task_fault_frame);
}

static uint64_t task_events_fault_write(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer || offset != 0 || size != sizeof(task_fault_return)) return 0;
    return scheduler::write_fault_return(node->ptr, (const task_fault_return *)buffer) ? size : 0;
}

static vfs::vfs_node *task_events_finddir(vfs::vfs_node *parent, const char *name) {
    if (!parent || !name) return nullptr;
    uint64_t pid = parent->ptr;
    if (pid != UINT64_MAX && !scheduler::get_task_by_id(pid)) return nullptr;

    uint64_t (*read_fn)(vfs::vfs_node *, uint64_t, uint64_t, uint8_t *) = nullptr;
    uint64_t (*write_fn)(vfs::vfs_node *, uint64_t, uint64_t, uint8_t *) = nullptr;
    if (strcmp(name, "inbox") == 0) {
        read_fn = task_events_inbox_read;
        write_fn = task_events_inbox_write;
    } else if (strcmp(name, "faultctl") == 0) {
        read_fn = task_events_faultctl_read;
        write_fn = task_events_faultctl_write;
    } else if (strcmp(name, "fault") == 0) {
        read_fn = task_events_fault_read;
        write_fn = task_events_fault_write;
    } else {
        return nullptr;
    }

    vfs::vfs_node *node = make_transient_node(name, vfs::VfsType::VFS_CHAR_DEVICE, parent);
    if (!node) return nullptr;
    node->ptr = pid;
    node->read = read_fn;
    node->write = write_fn;
    return node;
}

static int task_events_readdir(vfs::vfs_node *, uint64_t index, vfs::vfs_dirent *out) {
    static const char *names[] = {"inbox", "faultctl", "fault"};
    if (!out || index >= sizeof(names) / sizeof(names[0])) return -1;
    const char *name = names[index];
    char *dst = out->name;
    uint64_t cap = out->name_capacity;
    uint64_t len = strlen(name);
    memset(out, 0, sizeof(*out));
    out->type = (uint32_t)vfs::VfsType::VFS_CHAR_DEVICE;
    out->name_len = len;
    out->name = dst;
    out->name_capacity = cap;
    if (dst && cap > len) memcpy(dst, name, len + 1);
    return 0;
}

static vfs::vfs_node *task_entry_finddir(vfs::vfs_node *parent, const char *name) {
    if (!parent || !name) return nullptr;
    uint64_t pid = parent->ptr;
    if (pid != UINT64_MAX && !scheduler::get_task_by_id(pid)) return nullptr;

    vfs::vfs_node *node = nullptr;
    if (strcmp(name, "launch") == 0) {
        node = make_transient_node(name, vfs::VfsType::VFS_DIRECTORY, parent);
        if (!node) return nullptr;
        node->ptr = pid;
        node->finddir = task_launch_finddir;
        node->readdir = task_launch_readdir;
        return node;
    }
    if (strcmp(name, "events") == 0) {
        node = make_transient_node(name, vfs::VfsType::VFS_DIRECTORY, parent);
        if (!node) return nullptr;
        node->ptr = pid;
        node->finddir = task_events_finddir;
        node->readdir = task_events_readdir;
        return node;
    }
    return nullptr;
}

static int task_entry_readdir(vfs::vfs_node *, uint64_t index, vfs::vfs_dirent *out) {
    static const char *names[] = {"launch", "events"};
    if (!out || index >= sizeof(names) / sizeof(names[0])) return -1;
    const char *name = names[index];
    char *dst = out->name;
    uint64_t cap = out->name_capacity;
    uint64_t len = strlen(name);
    memset(out, 0, sizeof(*out));
    out->type = (uint32_t)vfs::VfsType::VFS_DIRECTORY;
    out->name_len = len;
    out->name = dst;
    out->name_capacity = cap;
    if (dst && cap > len) memcpy(dst, name, len + 1);
    return 0;
}

static vfs::vfs_node *task_root_finddir(vfs::vfs_node *parent, const char *name) {
    uint64_t pid = UINT64_MAX;
    if (strcmp(name, "self") != 0) {
        if (!parse_u64_strict(name, &pid) || pid == 0 || !scheduler::get_task_by_id(pid)) return nullptr;
    }
    vfs::vfs_node *node = make_transient_node(name, vfs::VfsType::VFS_DIRECTORY, parent);
    if (!node) return nullptr;
    node->ptr = pid;
    node->finddir = task_entry_finddir;
    node->readdir = task_entry_readdir;
    return node;
}

static int task_root_readdir(vfs::vfs_node *, uint64_t index, vfs::vfs_dirent *out) {
    char tmp[32];
    const char *name = nullptr;
    uint64_t pid = 0;
    if (index == 0) {
        name = "self";
    } else {
        if (!scheduler::task_id_by_index(index - 1, &pid)) return -1;
        u64_to_dec(pid, tmp, sizeof(tmp));
        name = tmp;
    }
    char *dst = out->name;
    uint64_t cap = out->name_capacity;
    uint64_t len = strlen(name);
    memset(out, 0, sizeof(*out));
    out->type = (uint32_t)vfs::VfsType::VFS_DIRECTORY;
    out->name_len = len;
    out->name = dst;
    out->name_capacity = cap;
    if (dst && cap > len) memcpy(dst, name, len + 1);
    return 0;
}

static vfs::vfs_node *proc_task_finddir(vfs::vfs_node *parent, const char *name) {
    uint64_t pid = parent ? parent->ptr : UINT64_MAX;
    if (!scheduler::get_task_by_id(pid)) return nullptr;

    uint64_t (*read_fn)(vfs::vfs_node *, uint64_t, uint64_t, uint8_t *) = nullptr;
    if (strcmp(name, "status") == 0) read_fn = proc_task_status_one_read;
    else if (strcmp(name, "maps") == 0) read_fn = proc_task_maps_one_read;
    else if (strcmp(name, "fds") == 0) read_fn = proc_task_fds_one_read;
    else return nullptr;

    vfs::vfs_node *node = make_transient_node(name, vfs::VfsType::VFS_CHAR_DEVICE, parent);
    if (!node) return nullptr;
    node->ptr = pid;
    node->read = read_fn;
    return node;
}

static int proc_task_readdir(vfs::vfs_node *, uint64_t index, vfs::vfs_dirent *out) {
    static const char *names[] = {"status", "maps", "fds"};
    if (!out || index >= sizeof(names) / sizeof(names[0])) return -1;
    const char *name = names[index];
    char *dst = out->name;
    uint64_t cap = out->name_capacity;
    uint64_t len = strlen(name);
    memset(out, 0, sizeof(*out));
    out->type = (uint32_t)vfs::VfsType::VFS_CHAR_DEVICE;
    out->name_len = len;
    out->name = dst;
    out->name_capacity = cap;
    if (dst && cap > len) memcpy(dst, name, len + 1);
    return 0;
}

static vfs::vfs_node *proc_tasks_finddir(vfs::vfs_node *parent, const char *name) {
    uint64_t pid;
    if (!parse_u64_strict(name, &pid) || pid == 0 || !scheduler::get_task_by_id(pid)) return nullptr;
    vfs::vfs_node *node = make_transient_node(name, vfs::VfsType::VFS_DIRECTORY, parent);
    if (!node) return nullptr;
    node->ptr = pid;
    node->finddir = proc_task_finddir;
    node->readdir = proc_task_readdir;
    return node;
}

static int proc_tasks_readdir(vfs::vfs_node *, uint64_t index, vfs::vfs_dirent *out) {
    uint64_t pid;
    if (!out || !scheduler::task_id_by_index(index, &pid)) return -1;
    char tmp[32];
    u64_to_dec(pid, tmp, sizeof(tmp));
    char *dst = out->name;
    uint64_t cap = out->name_capacity;
    uint64_t len = strlen(tmp);
    memset(out, 0, sizeof(*out));
    out->type = (uint32_t)vfs::VfsType::VFS_DIRECTORY;
    out->name_len = len;
    out->name = dst;
    out->name_capacity = cap;
    if (dst && cap > len) memcpy(dst, tmp, len + 1);
    return 0;
}


static void cpuid_leaf(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
                       uint32_t *ecx, uint32_t *edx) {
    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(subleaf));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

struct cpuid_bit_name {
    uint32_t leaf;
    uint32_t subleaf;
    char reg;
    uint8_t bit;
    const char *name;
};

static uint32_t cpuid_reg_value(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx, char reg) {
    switch (reg) {
        case 'a': return eax;
        case 'b': return ebx;
        case 'c': return ecx;
        case 'd': return edx;
        default: return 0;
    }
}

static const cpuid_bit_name cpuid_features[] = {
    {1,0,'d',0,"fpu"},{1,0,'d',1,"vme"},{1,0,'d',2,"de"},{1,0,'d',3,"pse"},
    {1,0,'d',4,"tsc"},{1,0,'d',5,"msr"},{1,0,'d',6,"pae"},{1,0,'d',7,"mce"},
    {1,0,'d',8,"cx8"},{1,0,'d',9,"apic"},{1,0,'d',11,"sep"},{1,0,'d',12,"mtrr"},
    {1,0,'d',13,"pge"},{1,0,'d',14,"mca"},{1,0,'d',15,"cmov"},{1,0,'d',16,"pat"},
    {1,0,'d',17,"pse36"},{1,0,'d',18,"psn"},{1,0,'d',19,"clfsh"},{1,0,'d',21,"ds"},
    {1,0,'d',22,"acpi"},{1,0,'d',23,"mmx"},{1,0,'d',24,"fxsr"},{1,0,'d',25,"sse"},
    {1,0,'d',26,"sse2"},{1,0,'d',27,"ss"},{1,0,'d',28,"htt"},{1,0,'d',29,"tm"},
    {1,0,'d',30,"ia64"},{1,0,'d',31,"pbe"},
    {1,0,'c',0,"sse3"},{1,0,'c',1,"pclmulqdq"},{1,0,'c',2,"dtes64"},{1,0,'c',3,"monitor"},
    {1,0,'c',4,"ds_cpl"},{1,0,'c',5,"vmx"},{1,0,'c',6,"smx"},{1,0,'c',7,"est"},
    {1,0,'c',8,"tm2"},{1,0,'c',9,"ssse3"},{1,0,'c',10,"cnxt_id"},{1,0,'c',11,"sdbg"},
    {1,0,'c',12,"fma"},{1,0,'c',13,"cx16"},{1,0,'c',14,"xtpr"},{1,0,'c',15,"pdcm"},
    {1,0,'c',17,"pcid"},{1,0,'c',18,"dca"},{1,0,'c',19,"sse4_1"},{1,0,'c',20,"sse4_2"},
    {1,0,'c',21,"x2apic"},{1,0,'c',22,"movbe"},{1,0,'c',23,"popcnt"},{1,0,'c',24,"tsc_deadline"},
    {1,0,'c',25,"aes"},{1,0,'c',26,"xsave"},{1,0,'c',27,"osxsave"},{1,0,'c',28,"avx"},
    {1,0,'c',29,"f16c"},{1,0,'c',30,"rdrand"},{1,0,'c',31,"hypervisor"},
    {7,0,'b',0,"fsgsbase"},{7,0,'b',1,"tsc_adjust"},{7,0,'b',2,"sgx"},{7,0,'b',3,"bmi1"},
    {7,0,'b',4,"hle"},{7,0,'b',5,"avx2"},{7,0,'b',6,"fdp_excptn_only"},{7,0,'b',7,"smep"},
    {7,0,'b',8,"bmi2"},{7,0,'b',9,"erms"},{7,0,'b',10,"invpcid"},{7,0,'b',11,"rtm"},
    {7,0,'b',12,"rdt_m"},{7,0,'b',13,"depr_fpu_cs_ds"},{7,0,'b',14,"mpx"},{7,0,'b',15,"rdt_a"},
    {7,0,'b',16,"avx512f"},{7,0,'b',17,"avx512dq"},{7,0,'b',18,"rdseed"},{7,0,'b',19,"adx"},
    {7,0,'b',20,"smap"},{7,0,'b',21,"avx512_ifma"},{7,0,'b',22,"pcommit"},{7,0,'b',23,"clflushopt"},
    {7,0,'b',24,"clwb"},{7,0,'b',25,"intel_pt"},{7,0,'b',26,"avx512pf"},{7,0,'b',27,"avx512er"},
    {7,0,'b',28,"avx512cd"},{7,0,'b',29,"sha"},{7,0,'b',30,"avx512bw"},{7,0,'b',31,"avx512vl"},
    {7,0,'c',0,"prefetchwt1"},{7,0,'c',1,"avx512_vbmi"},{7,0,'c',2,"umip"},{7,0,'c',3,"pku"},
    {7,0,'c',4,"ospke"},{7,0,'c',5,"waitpkg"},{7,0,'c',6,"avx512_vbmi2"},{7,0,'c',7,"cet_ss"},
    {7,0,'c',8,"gfni"},{7,0,'c',9,"vaes"},{7,0,'c',10,"vpclmulqdq"},{7,0,'c',11,"avx512_vnni"},
    {7,0,'c',12,"avx512_bitalg"},{7,0,'c',14,"avx512_vpopcntdq"},{7,0,'c',16,"la57"},{7,0,'c',22,"rdpid"},
    {7,0,'c',25,"cldemote"},{7,0,'c',27,"movdiri"},{7,0,'c',28,"movdir64b"},{7,0,'c',29,"enqcmd"},
    {7,0,'c',30,"sgx_lc"},{7,0,'d',2,"avx512_4vnniw"},{7,0,'d',3,"avx512_4fmaps"},{7,0,'d',4,"fsrm"},
    {7,0,'d',8,"avx512_vp2intersect"},{7,0,'d',10,"md_clear"},{7,0,'d',14,"serialize"},{7,0,'d',16,"tsxldtrk"},
    {7,0,'d',18,"pconfig"},{7,0,'d',19,"arch_lbr"},{7,0,'d',20,"cet_ibt"},{7,0,'d',22,"amx_bf16"},
    {7,0,'d',23,"avx512_fp16"},{7,0,'d',24,"amx_tile"},{7,0,'d',25,"amx_int8"},{7,0,'d',26,"ibrs_ibpb"},
    {7,0,'d',27,"stibp"},{7,0,'d',28,"l1d_flush"},{7,0,'d',29,"arch_caps"},{7,0,'d',30,"core_caps"},
    {7,0,'d',31,"ssbd"},
    {0x80000001u,0,'d',11,"syscall"},{0x80000001u,0,'d',20,"nx"},{0x80000001u,0,'d',22,"mmxext"},
    {0x80000001u,0,'d',25,"fxsr_opt"},{0x80000001u,0,'d',26,"pdpe1gb"},{0x80000001u,0,'d',27,"rdtscp"},
    {0x80000001u,0,'d',29,"lm"},{0x80000001u,0,'d',30,"3dnowext"},{0x80000001u,0,'d',31,"3dnow"},
    {0x80000001u,0,'c',0,"lahf_lm"},{0x80000001u,0,'c',1,"cmp_legacy"},{0x80000001u,0,'c',2,"svm"},
    {0x80000001u,0,'c',3,"extapic"},{0x80000001u,0,'c',4,"cr8_legacy"},{0x80000001u,0,'c',5,"abm"},
    {0x80000001u,0,'c',6,"sse4a"},{0x80000001u,0,'c',7,"misalignsse"},{0x80000001u,0,'c',8,"3dnowprefetch"},
    {0x80000001u,0,'c',9,"osvw"},{0x80000001u,0,'c',10,"ibs"},{0x80000001u,0,'c',11,"xop"},
    {0x80000001u,0,'c',12,"skinit"},{0x80000001u,0,'c',13,"wdt"},{0x80000001u,0,'c',15,"lwp"},
    {0x80000001u,0,'c',16,"fma4"},{0x80000001u,0,'c',21,"tbm"},{0x80000001u,0,'c',22,"topoext"},
    {0x80000001u,0,'c',23,"perfctr_core"},{0x80000001u,0,'c',24,"perfctr_nb"},{0x80000001u,0,'c',26,"dbx"},
    {0x80000001u,0,'c',27,"perftsc"},{0x80000001u,0,'c',28,"pcx_l2i"},
};

static uint64_t cpu_features_read(vfs::vfs_node *, uint64_t offset, uint64_t size, uint8_t *buffer) {
    uint64_t flags;
    proc_snapshot_lock.acquire(flags);
    uint64_t len = 0;
    proc_cpu_feature_buf[0] = 0;

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid_leaf(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_basic = eax;
    char vendor[13];
    memcpy(vendor + 0, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = 0;
    cpuid_leaf(0x80000000u, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_ext = eax;
    proc_appendf(proc_cpu_feature_buf, sizeof(proc_cpu_feature_buf), &len, "vendor: %s\n", vendor);
    proc_appendf(proc_cpu_feature_buf, sizeof(proc_cpu_feature_buf), &len, "max_basic: 0x%x\n", max_basic);
    proc_appendf(proc_cpu_feature_buf, sizeof(proc_cpu_feature_buf), &len, "max_extended: 0x%x\n", max_ext);
    proc_append(proc_cpu_feature_buf, sizeof(proc_cpu_feature_buf), &len, "features:");

    for (uint64_t i = 0; i < sizeof(cpuid_features) / sizeof(cpuid_features[0]); i++) {
        const cpuid_bit_name *f = &cpuid_features[i];
        if ((f->leaf < 0x80000000u && f->leaf > max_basic) ||
            (f->leaf >= 0x80000000u && f->leaf > max_ext)) continue;
        cpuid_leaf(f->leaf, f->subleaf, &eax, &ebx, &ecx, &edx);
        uint32_t v = cpuid_reg_value(eax, ebx, ecx, edx, f->reg);
        if ((v & (1u << f->bit)) == 0) continue;
        proc_append(proc_cpu_feature_buf, sizeof(proc_cpu_feature_buf), &len, " ");
        proc_append(proc_cpu_feature_buf, sizeof(proc_cpu_feature_buf), &len, f->name);
    }
    proc_append(proc_cpu_feature_buf, sizeof(proc_cpu_feature_buf), &len, "\n");

    uint64_t ret = proc_copy_static(proc_cpu_feature_buf, len, offset, size, buffer);
    proc_snapshot_lock.release(flags);
    return ret;
}


static uint64_t cpu_cpuid_raw_read(vfs::vfs_node *, uint64_t offset, uint64_t size,
                                   uint8_t *buffer) {
    uint64_t flags;
    proc_snapshot_lock.acquire(flags);
    uint64_t len = 0;
    proc_cpuid_raw_buf[0] = 0;

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid_leaf(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_basic = eax;
    cpuid_leaf(0x80000000u, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_ext = eax;

    for (uint32_t leaf = 0; leaf <= max_basic && leaf < 0x40u; leaf++) {
        uint32_t sub_max = 0;
        if (leaf == 4 || leaf == 7 || leaf == 0xBu || leaf == 0xDu || leaf == 0xFu || leaf == 0x10u) sub_max = 8;
        for (uint32_t sub = 0; sub <= sub_max; sub++) {
            cpuid_leaf(leaf, sub, &eax, &ebx, &ecx, &edx);
            proc_appendf(proc_cpuid_raw_buf, sizeof(proc_cpuid_raw_buf), &len,
                         "leaf=0x%x sub=0x%x eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
                         leaf, sub, eax, ebx, ecx, edx);
            if (leaf == 4 && (eax & 0x1f) == 0) break;
            if (leaf == 0xBu && eax == 0 && ebx == 0) break;
            if (leaf == 0xDu && sub > 1 && eax == 0 && ebx == 0 && ecx == 0 && edx == 0) break;
        }
    }
    for (uint32_t leaf = 0x80000000u; leaf <= max_ext && leaf < 0x80000040u; leaf++) {
        cpuid_leaf(leaf, 0, &eax, &ebx, &ecx, &edx);
        proc_appendf(proc_cpuid_raw_buf, sizeof(proc_cpuid_raw_buf), &len,
                     "leaf=0x%x sub=0x0 eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
                     leaf, eax, ebx, ecx, edx);
    }

    uint64_t ret = proc_copy_static(proc_cpuid_raw_buf, len, offset, size, buffer);
    proc_snapshot_lock.release(flags);
    return ret;
}

static uint64_t sys_devices_read(vfs::vfs_node *, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[4096];
    uint64_t len = 0;
    buf[0] = 0;
    for (device *dev = device_list; dev; dev = dev->next) {
        proc_appendf(buf, sizeof(buf), &len, "%s bus=%s driver=%s\n", dev->name ? dev->name : "?",
                     dev->bus && dev->bus->name ? dev->bus->name : "none",
                     dev->driver && dev->driver->name ? dev->driver->name : "none");
    }
    return proc_copy_static(buf, len, offset, size, buffer);
}

static uint64_t sys_buses_read(vfs::vfs_node *, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[2048];
    uint64_t len = 0;
    buf[0] = 0;
    for (bus *b = bus_list; b; b = b->next) proc_appendf(buf, sizeof(buf), &len, "%s\n", b->name ? b->name : "?");
    return proc_copy_static(buf, len, offset, size, buffer);
}

static uint64_t sys_drivers_read(vfs::vfs_node *, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[4096];
    uint64_t len = 0;
    buf[0] = 0;
    for (driver *d = driver_list; d; d = d->next) {
        proc_appendf(buf, sizeof(buf), &len, "%s bus=%s\n", d->name ? d->name : "?",
                     d->bus && d->bus->name ? d->bus->name : "none");
    }
    return proc_copy_static(buf, len, offset, size, buffer);
}

static vfs::vfs_node *virt_mkdir(vfs::vfs_node *parent, const char *name) {
    if (!parent || !name) return nullptr;
    vfs::vfs_node *node = vfs::finddir(parent, name);
    if (node) return node->type == vfs::VfsType::VFS_DIRECTORY ? node : nullptr;
    return vfs::create_node(name, vfs::VfsType::VFS_DIRECTORY, parent);
}

static vfs::vfs_node *virt_file(vfs::vfs_node *parent, const char *name,
                                uint64_t (*read)(vfs::vfs_node *, uint64_t, uint64_t, uint8_t *),
                                uintptr_t ptr = 0) {
    if (!parent || !name || !read) return nullptr;
    vfs::vfs_node *node = vfs::finddir(parent, name);
    if (!node) node = vfs::create_node(name, vfs::VfsType::VFS_CHAR_DEVICE, parent);
    if (!node || node->type != vfs::VfsType::VFS_CHAR_DEVICE) return nullptr;
    node->read = read;
    node->ptr = ptr;
    return node;
}

static void virt_stat_file(vfs::vfs_node *parent, const char *name, VirtStat stat) {
    virt_file(parent, name, virt_stat_read, (uintptr_t)stat);
}

static void publish_pmm_buddy_debug(vfs::vfs_node *parent) {
    vfs::vfs_node *buddy = virt_mkdir(parent, "buddy");
    if (!buddy) return;

    for (size_t order = 0; order <= pmm::get_max_order(); order++) {
        char name[16];
        snprintf(name, sizeof(name), "order%zu", order);
        vfs::vfs_node *dir = virt_mkdir(buddy, name);
        if (!dir) continue;
        virt_file(dir, "blocks", pmm_order_stat_read, (uintptr_t)(order << 1));
        virt_file(dir, "bytes", pmm_order_stat_read, (uintptr_t)((order << 1) | 1));
    }
}

namespace sysfs {
    void init() {
        sys_root = vfs::create_node("sys", vfs::VfsType::VFS_DIRECTORY, vfs::get_root());
        if (!sys_root) return;

        vfs::vfs_node *devices = virt_mkdir(sys_root, "devices");
        if (devices) virt_file(devices, "list", sys_devices_read);

        vfs::vfs_node *bus = virt_mkdir(sys_root, "bus");
        if (bus) virt_file(bus, "list", sys_buses_read);

        vfs::vfs_node *drivers = virt_mkdir(sys_root, "drivers");
        if (drivers) virt_file(drivers, "list", sys_drivers_read);

        vfs::vfs_node *kernel = virt_mkdir(sys_root, "kernel");
        if (!kernel) return;

        vfs::vfs_node *mm = virt_mkdir(kernel, "mm");
        if (mm) {
            vfs::vfs_node *pmm_dir = virt_mkdir(mm, "pmm");
            if (pmm_dir) {
                virt_stat_file(pmm_dir, "page_size", VirtStat::PmmPageSize);
                virt_stat_file(pmm_dir, "max_order", VirtStat::PmmMaxOrder);
                virt_stat_file(pmm_dir, "ap_trampoline_phys", VirtStat::PmmApTrampoline);
            }

            vfs::vfs_node *heap_dir = virt_mkdir(mm, "heap");
            if (heap_dir) {
                virt_stat_file(heap_dir, "slab_page_bytes", VirtStat::HeapSlabPageBytes);
                virt_stat_file(heap_dir, "large_alloc_bytes", VirtStat::HeapLargeAllocBytes);
                virt_stat_file(heap_dir, "large_alloc_count", VirtStat::HeapLargeAllocCount);
            }

            vfs::vfs_node *vmm_dir = virt_mkdir(mm, "vmm");
            if (vmm_dir) {
                virt_stat_file(vmm_dir, "direct_map_bytes", VirtStat::VmmDirectMap);
                virt_stat_file(vmm_dir, "page_table_bytes", VirtStat::VmmPageTableBytes);
                virt_stat_file(vmm_dir, "phys_map_base", VirtStat::VmmPhysMapBase);
                virt_stat_file(vmm_dir, "paging_levels", VirtStat::VmmPagingLevels);
                virt_stat_file(vmm_dir, "virtual_address_bits", VirtStat::VmmVirtualAddressBits);
                virt_stat_file(vmm_dir, "page_1g_supported", VirtStat::VmmPage1G);
                virt_stat_file(vmm_dir, "demand_zero_supported", VirtStat::VmmDemandZero);
                virt_stat_file(vmm_dir, "guard_page_supported", VirtStat::VmmGuardPage);
                virt_stat_file(vmm_dir, "pat_supported", VirtStat::VmmPat);
                virt_stat_file(vmm_dir, "write_combining_supported", VirtStat::VmmWriteCombining);
                virt_stat_file(vmm_dir, "nx_supported", VirtStat::VmmNx);
            }
        }

        vfs::vfs_node *debug = virt_mkdir(kernel, "debug");
        if (!debug) return;

        vfs::vfs_node *debug_cpu = virt_mkdir(debug, "cpu");
        if (debug_cpu) virt_file(debug_cpu, "cpuid_raw", cpu_cpuid_raw_read);

        vfs::vfs_node *debug_sched = virt_mkdir(debug, "scheduler");
        if (debug_sched) {
            virt_file(debug_sched, "cpus", proc_cpu_debug_read);
            virt_file(debug_sched, "workqueue", proc_workqueue_read);
            vfs::vfs_node *debug_tasks = virt_mkdir(debug_sched, "tasks");
            if (debug_tasks) {
                debug_tasks->finddir = proc_tasks_finddir;
                debug_tasks->readdir = proc_tasks_readdir;
            }
        }

        vfs::vfs_node *debug_mm = virt_mkdir(debug, "mm");
        if (debug_mm) {
            vfs::vfs_node *debug_pmm = virt_mkdir(debug_mm, "pmm");
            if (debug_pmm) {
                virt_stat_file(debug_pmm, "deferred_spans", VirtStat::PmmDeferredSpans);
                publish_pmm_buddy_debug(debug_pmm);
            }
        }
    }
}

namespace procfs {
    void init() {
        vfs::vfs_node *task_root =
            vfs::create_node("task", vfs::VfsType::VFS_DIRECTORY, vfs::get_root());
        if (task_root) {
            task_root->finddir = task_root_finddir;
            task_root->readdir = task_root_readdir;
        }

        vfs::vfs_node *proc_root =
            vfs::create_node("proc", vfs::VfsType::VFS_DIRECTORY, vfs::get_root());
        if (!proc_root) return;

        vfs::vfs_node *mem_dir = virt_mkdir(proc_root, "mem");
        if (mem_dir) {
            virt_stat_file(mem_dir, "total", VirtStat::MemTotal);
            virt_stat_file(mem_dir, "available", VirtStat::MemAvailable);
            virt_stat_file(mem_dir, "used", VirtStat::MemUsed);
            virt_stat_file(mem_dir, "managed", VirtStat::MemManaged);
            virt_stat_file(mem_dir, "system", VirtStat::MemSystem);
            virt_stat_file(mem_dir, "physical_limit", VirtStat::MemPhysicalLimit);
            virt_stat_file(mem_dir, "direct_map", VirtStat::VmmDirectMap);
        }

        virt_file(proc_root, "devices", proc_devices_read);
        virt_file(proc_root, "disks", proc_disks_read);

        fs_publish_proc();

        vfs::vfs_node *cpu_dir = virt_mkdir(proc_root, "cpu");
        if (cpu_dir) {
            virt_stat_file(cpu_dir, "count", VirtStat::CpuCount);
            virt_file(cpu_dir, "vendor", proc_cpu_vendor_read);
            virt_file(cpu_dir, "features", cpu_features_read);
        }
    }
}  // namespace procfs


static int devfs_register_typed(const char *path, device_ops *ops, void *priv,
                                vfs::VfsType type, uint64_t size) {
    if (!dev_root || !path || !ops) return -1;

    vfs::vfs_node *curr = dev_root;
    char path_buf[256];
    strncpy(path_buf, path, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    char *saveptr = nullptr;
    char *token = strtok_r(path_buf, "/", &saveptr);

    while (token != nullptr) {
        if (!path_component_valid(token)) return -1;
        char *next_token = strtok_r(nullptr, "/", &saveptr);

        if (next_token == nullptr) {
            if (vfs::finddir(curr, token)) return -1;

            vfs::vfs_node *node = vfs::create_node(token, type, curr);
            if (!node) return -1;

            device_instance *inst = (device_instance *)heap::kmalloc(sizeof(device_instance));
            if (!inst) {
                vfs_detach_and_put(node);
                return -1;
            }
            inst->ops = ops;
            inst->priv = priv;
            inst->removed = false;
            memset(&inst->info, 0, sizeof(inst->info));

            node->ptr = (uintptr_t)inst;
            node->release = device_node_release;
            node->size = size;
            node->read = dev_vfs_read;
            node->write = dev_vfs_write;
            return 0;
        } else {
            vfs::vfs_node *dir = vfs::finddir(curr, token);
            if (dir && dir->type != vfs::VfsType::VFS_DIRECTORY) return -1;
            if (!dir) dir = vfs::create_node(token, vfs::VfsType::VFS_DIRECTORY, curr);
            if (!dir) return -1;
            curr = dir;
            token = next_token;
        }
    }
    return -1;
}

extern "C" int devfs_register(const char *path, device_ops *ops, void *priv) {
    return devfs_register_typed(path, ops, priv, vfs::VfsType::VFS_CHAR_DEVICE, 0);
}

extern "C" int devfs_register_block(const char *path, device_ops *ops, void *priv, uint64_t size) {
    return devfs_register_typed(path, ops, priv, vfs::VfsType::VFS_BLOCK_DEVICE, size);
}


int devfs_open_file(vfs::vfs_node *node, vfs::open_file *file) {
    if (!node || !file) return 0;
    if (!devfs_is_device_node(node)) return 0;
    device_instance *inst = (device_instance *)node->ptr;
    if (!inst || inst->removed || !inst->ops) return -1;
    file->device_ctx = nullptr;
    if (!inst->ops->open) return 0;
    return inst->ops->open(inst->priv, &file->device_ctx);
}

void devfs_close_file(vfs::open_file *file) {
    if (!file || !file->node || !devfs_is_device_node(file->node)) return;
    device_instance *inst = (device_instance *)file->node->ptr;
    if (inst && inst->ops && inst->ops->close) inst->ops->close(inst->priv, file->device_ctx);
    file->device_ctx = nullptr;
}

uint64_t devfs_read_file(vfs::open_file *file, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!file || !file->node || !devfs_is_device_node(file->node)) return 0;
    device_instance *inst = (device_instance *)file->node->ptr;
    if (!inst || !inst->ops) return 0;
    if (inst->ops->read_file) return inst->ops->read_file(inst->priv, file->device_ctx, offset, size, buffer);
    if (!inst->removed && inst->ops->read) return inst->ops->read(inst->priv, offset, size, buffer);
    return 0;
}

uint64_t devfs_write_file(vfs::open_file *file, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!file || !file->node || !devfs_is_device_node(file->node)) return 0;
    device_instance *inst = (device_instance *)file->node->ptr;
    if (!inst || !inst->ops) return 0;
    if (inst->ops->write_file) return inst->ops->write_file(inst->priv, file->device_ctx, offset, size, buffer);
    if (!inst->removed && inst->ops->write) return inst->ops->write(inst->priv, offset, size, buffer);
    return 0;
}

int devfs_mmap(vfs::vfs_node *node, uint64_t offset, uint64_t size, device_mmap_result *out) {
    if (!node || !out || !devfs_is_device_node(node)) return -1;
    device_instance *inst = (device_instance *)node->ptr;
    if (!inst || inst->removed || !inst->ops || !inst->ops->mmap) return -1;
    return inst->ops->mmap(inst->priv, offset, size, out);
}

int devfs_mmap_file(vfs::open_file *file, uint64_t offset, uint64_t size, device_mmap_result *out) {
    if (!file || !file->node || !out || !devfs_is_device_node(file->node)) return -1;
    device_instance *inst = (device_instance *)file->node->ptr;
    if (!inst || inst->removed || !inst->ops) return -1;
    if (inst->ops->mmap_file) return inst->ops->mmap_file(inst->priv, file->device_ctx, offset, size, out);
    if (inst->ops->mmap) return inst->ops->mmap(inst->priv, offset, size, out);
    return -1;
}

extern "C" void devfs_unregister(const char *path) {
    if (!dev_root || !path) return;
    vfs::vfs_node *node = vfs::traverse_relative(dev_root, path);
    if (!node || (node->type != vfs::VfsType::VFS_CHAR_DEVICE &&
                  node->type != vfs::VfsType::VFS_BLOCK_DEVICE))
        return;

    device_instance *inst = (device_instance *)node->ptr;
    if (inst) inst->removed = true;
    vfs::vfs_node *parent = node->parent;
    vfs_detach_and_put(node);
    cleanup_empty_dev_dirs(parent);
}

/* device registry globals are declared near dev_root */

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
KEXPORT(devfs_register_block)
KEXPORT(devfs_unregister)