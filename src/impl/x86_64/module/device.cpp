#include "mod/device.h"

#include "console.h"
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

static uint64_t random_read(void *, uint64_t, uint64_t size, uint8_t *buffer) {
    random::fill(buffer, size);
    return size;
}

static device_ops random_ops = {.read = random_read, .write = null_write};

static uint64_t console_dev_write(void *, uint64_t, uint64_t size, uint8_t *buffer) {
    for (uint64_t i = 0; i < size; i++) { console::putchar(buffer[i]); }

    return size;
}

static device_ops console_ops = {.read = null_read, .write = console_dev_write};

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

static device_ops power_ops = {.read = power_device_read, .write = power_device_write};

static vfs::vfs_node *dev_root = nullptr;
static spinlock_t proc_snapshot_lock;
static char proc_cpu_debug_buf[8192];
static char proc_mem_debug_buf[8192];


static bool path_component_valid(const char *s) {
    return s && s[0] != '\0' && strcmp(s, ".") != 0 && strcmp(s, "..") != 0;
}

static void vfs_unlink_and_free(vfs::vfs_node *node) {
    if (!node || !node->parent) return;
    vfs::vfs_node *parent = node->parent;
    if (parent->child == node) {
        parent->child = node->next;
    } else {
        vfs::vfs_node *prev = parent->child;
        while (prev && prev->next != node) prev = prev->next;
        if (prev) prev->next = node->next;
    }
    if (node->name) heap::kfree(node->name);
    heap::kfree(node);
}

static void cleanup_empty_dev_dirs(vfs::vfs_node *dir) {
    while (dir && dir != dev_root && dir->type == vfs::VfsType::VFS_DIRECTORY && !dir->child) {
        vfs::vfs_node *parent = dir->parent;
        vfs_unlink_and_free(dir);
        dir = parent;
    }
}

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
        devfs_register("random", &random_ops, nullptr);
        devfs_register("power", &power_ops, nullptr);
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


static void proc_devices_append(char *buf, uint64_t cap, uint64_t *pos, const char *s) {
    while (*s && *pos + 1 < cap) buf[(*pos)++] = *s++;
    if (*pos < cap) buf[*pos] = '\0';
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
    proc_appendf(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len, "cores: %d\n", cores);

    for (uint64_t i = 0; i < cores; i++) {
        smp::cpu_local *cpu = smp::get_cpu_by_index(i);
        if (!cpu) continue;

        uint64_t queued[scheduler::MAX_QUEUES];
        for (int q = 0; q < scheduler::MAX_QUEUES; q++) {
            queued[q] = 0;
            for (scheduler::task *t = cpu->task_queues_head[q]; t; t = t->next) queued[q]++;
        }

        proc_appendf(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len,
                     "cpu%d: lapic=%d ticks=%d current=%d mailbox=%d/%d idle=%d features=pge:%d smep:%d smap:%d umip:%d pat:%d wc:%d xsave:%d avx:%d fsgsbase_supported:%d fsgsbase_enabled:%d\n",
                     i, cpu->lapic_id, cpu->ticks,
                     cpu->current_task ? cpu->current_task->id : UINT64_MAX, cpu->mail_head,
                     cpu->mail_tail, cpu->idle_task ? cpu->idle_task->id : UINT64_MAX,
                     cpu->cpu_features.pge ? 1 : 0, cpu->cpu_features.smep ? 1 : 0,
                     cpu->cpu_features.smap ? 1 : 0, cpu->cpu_features.umip ? 1 : 0,
                     cpu->cpu_features.pat ? 1 : 0, cpu->cpu_features.wc ? 1 : 0,
                     cpu->cpu_features.xsave ? 1 : 0, cpu->cpu_features.avx ? 1 : 0,
                     cpu->cpu_features.fsgsbase_supported ? 1 : 0,
                     cpu->cpu_features.fsgsbase ? 1 : 0);
        proc_appendf(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len,
                     "  queues: q0=%d oldest=%d q1=%d oldest=%d q2=%d oldest=%d q3=%d oldest=%d sleep_head=",
                     queued[0], cpu->task_queue_oldest_tick[0], queued[1],
                     cpu->task_queue_oldest_tick[1], queued[2], cpu->task_queue_oldest_tick[2],
                     queued[3], cpu->task_queue_oldest_tick[3]);
        if (cpu->sleep_list_head) {
            proc_appendf(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len,
                         "%d\n", cpu->sleep_list_head->id);
        } else {
            proc_append(proc_cpu_debug_buf, sizeof(proc_cpu_debug_buf), &len, "none\n");
        }
    }

    uint64_t ret = proc_copy_static(proc_cpu_debug_buf, len, offset, size, buffer);
    proc_snapshot_lock.release(flags);
    return ret;
}

static uint64_t proc_mem_debug_read(vfs::vfs_node *, uint64_t offset, uint64_t size,
                                    uint8_t *buffer) {
    pmm::DebugInfo info;
    pmm::get_debug_info(&info);

    uint64_t flags;
    proc_snapshot_lock.acquire(flags);
    uint64_t len = 0;
    proc_mem_debug_buf[0] = 0;
    uint64_t used_bytes = info.managed_bytes - info.free_bytes;
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "system_bytes: %d\n", info.system_bytes);
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "managed_bytes: %d\n", info.managed_bytes);
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "managed_mib: %d\n", info.managed_bytes / (1024 * 1024));
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "free_bytes: %d\n", info.free_bytes);
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "free_mib: %d\n", info.free_bytes / (1024 * 1024));
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "used_bytes: %d\n", used_bytes);
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "used_kib: %d\n", used_bytes / 1024);
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "used_mib: %d\n", used_bytes / (1024 * 1024));
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "physical_limit_bytes: %d\n", info.physical_limit_bytes);
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "direct_map_bytes: %d\n", vmm::direct_map_bytes());
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "page_1g_supported: %d\n", vmm::page_1g_supported() ? 1 : 0);
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "pat_supported: %d\n", vmm::pat_supported() ? 1 : 0);
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "wc_supported: %d\n", vmm::write_combining_supported() ? 1 : 0);
    proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "deferred_spans: %d\n", info.deferred_span_count);
    proc_append(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "buddy_orders:\n");

    for (size_t i = 0; i <= pmm::MAX_ORDER; i++) {
        proc_appendf(proc_mem_debug_buf, sizeof(proc_mem_debug_buf), &len, "  order%d: blocks=%d bytes=%d\n", i,
                     info.free_blocks[i], info.free_bytes_by_order[i]);
    }

    uint64_t ret = proc_copy_static(proc_mem_debug_buf, len, offset, size, buffer);
    proc_snapshot_lock.release(flags);
    return ret;
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
    vfs::vfs_node *node = (vfs::vfs_node *)heap::kmalloc(sizeof(vfs::vfs_node));
    if (!node) return nullptr;
    memset(node, 0, sizeof(*node));
    node->name = strdup(name ? name : "");
    if (!node->name) { heap::kfree(node); return nullptr; }
    node->type = type;
    node->parent = parent;
    return node;
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

        vfs::vfs_node *devices_node =
            vfs::create_node("devices", vfs::VfsType::VFS_CHAR_DEVICE, proc_root);
        if (devices_node) { devices_node->read = proc_devices_read; }

        vfs::vfs_node *tasks_node =
            vfs::create_node("tasks", vfs::VfsType::VFS_DIRECTORY, proc_root);
        if (tasks_node) {
            tasks_node->finddir = proc_tasks_finddir;
            tasks_node->readdir = proc_tasks_readdir;
        }

        vfs::create_node("modules", vfs::VfsType::VFS_DIRECTORY, proc_root);
        fs_publish_proc();

        vfs::vfs_node *cpu_dir = vfs::create_node("cpu", vfs::VfsType::VFS_DIRECTORY, proc_root);
        if (cpu_dir) {
            vfs::vfs_node *cpu_debug_node =
                vfs::create_node("debug", vfs::VfsType::VFS_CHAR_DEVICE, cpu_dir);
            if (cpu_debug_node) { cpu_debug_node->read = proc_cpu_debug_read; }
        }

        vfs::vfs_node *mem_debug_node =
            vfs::create_node("debug", vfs::VfsType::VFS_CHAR_DEVICE, mem_dir);
        if (mem_debug_node) { mem_debug_node->read = proc_mem_debug_read; }

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
                vfs_unlink_and_free(node);
                return -1;
            }
            inst->ops = ops;
            inst->priv = priv;

            node->ptr = (uintptr_t)inst;
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

extern "C" void devfs_unregister(const char *path) {
    if (!dev_root || !path) return;
    vfs::vfs_node *node = vfs::traverse_relative(dev_root, path);
    if (!node || (node->type != vfs::VfsType::VFS_CHAR_DEVICE &&
                  node->type != vfs::VfsType::VFS_BLOCK_DEVICE))
        return;

    vfs::vfs_node *parent = node->parent;
    if (node->ptr) heap::kfree((void *)node->ptr);
    vfs_unlink_and_free(node);
    cleanup_empty_dev_dirs(parent);
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
KEXPORT(devfs_register_block)
KEXPORT(devfs_unregister)