#include "file/vfs.h"

#include "memory/heap.h"
#include "string.h"

static vfs::vfs_node* root_node = nullptr;

namespace vfs {
    void init() {
        root_node = (vfs_node*)heap::kmalloc(sizeof(vfs_node));

        memset(root_node, 0, sizeof(vfs_node));

        root_node->name = (char*)heap::kmalloc(2);
        strcpy(root_node->name, "/");

        root_node->type = VfsType::VFS_DIRECTORY;
        root_node->size = 0;
        root_node->inode = 0;
        root_node->ptr = 0;
        root_node->child = nullptr;
        root_node->next = nullptr;
    }

    vfs_node* open(const char* path) {
        if (!path) return nullptr;
        if (path[0] != '/') return nullptr;  // Only absolute paths for now

        vfs_node* curr = get_root();

        // Skip the leading '/'
        const char* ptr = path + 1;

        while (*ptr != '\0') {
            // Find the end of the current segment
            const char* end = ptr;
            while (*end != '/' && *end != '\0') { end++; }

            // Calculate segment length
            uint64_t len = end - ptr;
            if (len > 0) {
                // Extract the segment into a temporary buffer for comparison
                char segment[128];
                if (len >= 128) len = 127;  // Simple overflow protection
                memcpy(segment, (void*)ptr, len);
                segment[len] = '\0';

                curr = finddir(curr, segment);
                if (!curr) return nullptr;
            }

            // Move to the next segment
            if (*end == '/') {
                ptr = end + 1;
            } else {
                break;
            }
        }

        return curr;
    }

    uint32_t read(vfs_node* node, uint32_t offset, uint32_t size, void* buffer) {
        if (!node || node->type != VfsType::VFS_FILE) return 0;
        if (offset >= node->size) return 0;

        uint32_t read_size = size;
        if (offset + size > node->size) read_size = node->size - offset;

        memcpy(buffer, (void*)(node->ptr + offset), read_size);
        return read_size;
    }

    vfs_node* finddir(vfs_node* parent, const char* name) {
        if (!parent) return nullptr;

        vfs_node* curr = parent->child;
        while (curr) {
            if (strcmp(curr->name, name) == 0) { return curr; }
            curr = curr->next;
        }

        return nullptr;
    }

    vfs_node* get_root() { return root_node; }
}  // namespace vfs