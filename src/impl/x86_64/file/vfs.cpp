#include "file/vfs.h"

#include "memory/heap.h"
#include "string.h"

static vfs::vfs_node *root_node = nullptr;
static spinlock_t vfs_lock;
static constexpr uint64_t VFS_MAX_PATH = 4096;
static constexpr uint64_t VFS_MAX_NAME = 255;

namespace vfs {
    static char *dup_component(const char *s, uint64_t n) {
        if (!s || n == 0 || n > VFS_MAX_NAME) return nullptr;
        char *out = (char *)heap::kmalloc(n + 1);
        if (!out) return nullptr;
        memcpy(out, s, n);
        out[n] = 0;
        return out;
    }

    static bool valid_path(const char *path) {
        if (!path || path[0] != '/') return false;
        uint64_t len = 0;
        while (path[len]) {
            len++;
            if (len >= VFS_MAX_PATH) return false;
        }
        return true;
    }

    static int split_parent(const char *path, vfs_node **parent_out, char **name_out) {
        if (!valid_path(path) || !parent_out || !name_out) return -1;
        uint64_t len = strlen(path);
        while (len > 1 && path[len - 1] == '/') len--;
        if (len <= 1) return -1;

        uint64_t slash = len - 1;
        while (slash > 0 && path[slash] != '/') slash--;
        uint64_t name_start = slash + 1;
        uint64_t name_len = len - name_start;
        if (name_len == 0 || name_len > VFS_MAX_NAME) return -1;
        if ((name_len == 1 && path[name_start] == '.') ||
            (name_len == 2 && path[name_start] == '.' && path[name_start + 1] == '.'))
            return -1;

        char *parent_path = nullptr;
        if (slash == 0) {
            parent_path = dup_component("/", 1);
        } else {
            parent_path = (char *)heap::kmalloc(slash + 1);
            if (parent_path) {
                memcpy(parent_path, path, slash);
                parent_path[slash] = 0;
            }
        }
        if (!parent_path) return -1;

        vfs_node *parent = open(parent_path);
        heap::kfree(parent_path);
        if (!parent || parent->type != VfsType::VFS_DIRECTORY) return -1;

        char *name = dup_component(path + name_start, name_len);
        if (!name) return -1;
        *parent_out = parent;
        *name_out = name;
        return 0;
    }

    static void remove_cached_child(vfs_node *parent, const char *name) {
        if (!parent || !name) return;
        vfs_node **link = &parent->child;
        while (*link) {
            vfs_node *node = *link;
            if (strcmp(node->name, name) == 0) {
                *link = node->next;
                node->next = nullptr;
                node->parent = nullptr;
                return;
            }
            link = &node->next;
        }
    }

    vfs_node *create_node(const char *name, VfsType type, vfs_node *parent) {
        vfs_node *node = (vfs_node *)heap::kmalloc(sizeof(vfs_node));
        if (!node) return nullptr;

        memset(node, 0, sizeof(vfs_node));
        node->name = strdup(name ? name : "");
        if (!node->name) {
            heap::kfree(node);
            return nullptr;
        }
        node->type = type;
        node->parent = parent;

        if (parent) {
            uint64_t flags;
            vfs_lock.acquire(flags);
            node->next = parent->child;
            parent->child = node;
            vfs_lock.release(flags);
        }
        return node;
    }

    void init() { root_node = create_node("/", VfsType::VFS_DIRECTORY, nullptr); }

    vfs_node *get_root() { return root_node; }

    vfs_node *finddir(vfs_node *parent, const char *name) {
        if (!parent || !name || strlen(name) > VFS_MAX_NAME) return nullptr;

        uint64_t flags;
        vfs_lock.acquire(flags);
        vfs_node *curr = parent->child;
        while (curr) {
            if (curr->name && strcmp(curr->name, name) == 0) {
                vfs_lock.release(flags);
                return curr;
            }
            curr = curr->next;
        }
        vfs_lock.release(flags);

        if (parent->finddir) return parent->finddir(parent, name);
        return nullptr;
    }

    int readdir(vfs_node *dir, uint64_t index, vfs_dirent *out) {
        if (!dir || !out || dir->type != VfsType::VFS_DIRECTORY) return -1;
        if (dir->readdir) return dir->readdir(dir, index, out);

        vfs_node *curr = dir->child;
        uint64_t i = 0;
        while (curr) {
            if (i == index) {
                memset(out, 0, sizeof(*out));
                out->inode = curr->inode;
                out->type = (uint32_t)curr->type;
                strncpy(out->name, curr->name ? curr->name : "", sizeof(out->name) - 1);
                out->name[sizeof(out->name) - 1] = 0;
                return 0;
            }
            i++;
            curr = curr->next;
        }
        return -1;
    }

    vfs_node *traverse_relative(vfs_node *start, const char *path) {
        if (!start || !path) return nullptr;

        vfs_node *curr = start;
        const char *p = path;

        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;

            const char *component = p;
            uint64_t len = 0;
            while (p[len] && p[len] != '/') {
                len++;
                if (len > VFS_MAX_NAME) return nullptr;
            }

            if (len == 1 && component[0] == '.') {
                p += len;
                continue;
            }
            if (len == 2 && component[0] == '.' && component[1] == '.') {
                if (curr->parent) curr = curr->parent;
                p += len;
                continue;
            }

            char *name = dup_component(component, len);
            if (!name) return nullptr;
            curr = finddir(curr, name);
            heap::kfree(name);
            if (!curr) return nullptr;
            p += len;
        }
        return curr;
    }

    vfs_node *open(const char *path) {
        if (!valid_path(path)) return nullptr;
        if (strcmp(path, "/") == 0) return root_node;
        return traverse_relative(root_node, path + 1);
    }

    vfs_node *create(const char *path, VfsType type) {
        vfs_node *parent = nullptr;
        char *name = nullptr;
        if (split_parent(path, &parent, &name) != 0) return nullptr;
        vfs_node *existing = finddir(parent, name);
        if (existing) {
            heap::kfree(name);
            return existing;
        }
        vfs_node *out = nullptr;
        int rc = -1;
        if (parent->create) rc = parent->create(parent, name, (uint32_t)type, &out);
        else {
            out = create_node(name, type, parent);
            rc = out ? 0 : -1;
        }
        heap::kfree(name);
        return rc == 0 ? out : nullptr;
    }

    int unlink(const char *path) {
        vfs_node *parent = nullptr;
        char *name = nullptr;
        if (split_parent(path, &parent, &name) != 0) return -1;
        int rc = parent->unlink ? parent->unlink(parent, name) : -1;
        if (rc == 0) remove_cached_child(parent, name);
        heap::kfree(name);
        return rc;
    }

    int rename(const char *old_path, const char *new_path) {
        vfs_node *old_parent = nullptr;
        vfs_node *new_parent = nullptr;
        char *old_name = nullptr;
        char *new_name = nullptr;
        if (split_parent(old_path, &old_parent, &old_name) != 0) return -1;
        if (split_parent(new_path, &new_parent, &new_name) != 0) {
            heap::kfree(old_name);
            return -1;
        }
        int rc = old_parent->rename ? old_parent->rename(old_parent, old_name, new_parent, new_name) : -1;
        if (rc == 0) {
            remove_cached_child(old_parent, old_name);
            remove_cached_child(new_parent, new_name);
        }
        heap::kfree(old_name);
        heap::kfree(new_name);
        return rc;
    }

    int truncate(vfs_node *node, uint64_t size) {
        if (!node || !node->truncate) return -1;
        return node->truncate(node, size);
    }

    uint64_t read(vfs_node *node, uint64_t offset, uint64_t size, void *buffer) {
        if (!node) return 0;
        if (node->read) return node->read(node, offset, size, (uint8_t *)buffer);
        if (node->type == VfsType::VFS_FILE) {
            if (offset >= node->size) return 0;
            uint64_t read_size = (offset + size > node->size) ? (node->size - offset) : size;
            memcpy(buffer, (void *)(node->ptr + offset), read_size);
            return read_size;
        }
        return 0;
    }

    uint64_t write(vfs_node *node, uint64_t offset, uint64_t size, void *buffer) {
        if (!node) return 0;
        if (node->write) return node->write(node, offset, size, (uint8_t *)buffer);
        return 0;
    }
}  // namespace vfs
