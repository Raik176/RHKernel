#include "file/vfs.h"

#include "memory/heap.h"
#include "string.h"
#include "symbol/ksym.h"

static vfs::vfs_node *root_node = nullptr;

namespace vfs {
    vfs_node *create_node(const char *name, VfsType type, vfs_node *parent) {
        vfs_node *node = (vfs_node *)heap::kmalloc(sizeof(vfs_node));
        if (!node) return nullptr;

        memset(node, 0, sizeof(vfs_node));
        node->name = strdup(name);
        node->type = type;
        node->parent = parent;

        if (parent) {
            node->next = parent->child;
            parent->child = node;
        }
        return node;
    }

    void init() { root_node = create_node("/", VfsType::VFS_DIRECTORY, nullptr); }

    vfs_node *get_root() { return root_node; }

    vfs_node *finddir(vfs_node *parent, const char *name) {
        if (!parent) return nullptr;
        vfs_node *curr = parent->child;
        while (curr) {
            if (strcmp(curr->name, name) == 0) return curr;
            curr = curr->next;
        }
        return nullptr;
    }

    vfs_node *traverse_relative(vfs_node *start, const char *path) {
        if (!start || !path) return nullptr;

        vfs_node *curr = start;
        char path_copy[256];
        strncpy(path_copy, path, 255);

        char *saveptr;
        char *token = strtok_r(path_copy, "/", &saveptr);

        while (token != nullptr) {
            curr = finddir(curr, token);
            if (!curr) return nullptr;
            token = strtok_r(nullptr, "/", &saveptr);
        }
        return curr;
    }

    vfs_node *open(const char *path) {
        if (!path || path[0] != '/') return nullptr;
        if (strcmp(path, "/") == 0) return root_node;
        return traverse_relative(root_node, path + 1);
    }

    uint32_t read(vfs_node *node, uint32_t offset, uint32_t size, void *buffer) {
        if (!node) return 0;

        if (node->type == VfsType::VFS_CHAR_DEVICE && node->read) {
            return node->read(node, offset, size, (uint8_t *)buffer);
        }

        if (node->type == VfsType::VFS_FILE) {
            if (offset >= node->size) return 0;
            uint32_t read_size = (offset + size > node->size) ? (node->size - offset) : size;
            memcpy(buffer, (void *)(node->ptr + offset), read_size);
            return read_size;
        }
        return 0;
    }

    uint32_t write(vfs_node *node, uint32_t offset, uint32_t size, void *buffer) {
        if (!node) return 0;
        if (node->type == VfsType::VFS_CHAR_DEVICE && node->write) {
            return node->write(node, offset, size, (uint8_t *)buffer);
        }

        return 0;
    }
}  // namespace vfs