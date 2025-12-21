#include "file/vfs.h"

#include "heap.h"
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

    vfs_node* finddir(vfs_node* parent, const char* name) {
        vfs_node* curr = parent->child;
        while (curr) {
            if (strcmp(curr->name, name) == 0) { return curr; }
            curr = curr->next;
        }
        return nullptr;
    }

    vfs_node* get_root() { return root_node; }
}  // namespace vfs