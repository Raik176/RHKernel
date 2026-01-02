#include "file/fd.h"

#include "memory/heap.h"
#include "string.h"

namespace fd_manager {

    vfs::open_file *get_file(int fd, scheduler::task *t) {
        if (!t || fd < 0 || (uint32_t)fd >= t->fd_capacity) return nullptr;
        return t->fd_table[fd];
    }

    bool expand_table(uint32_t needed, scheduler::task *t) {
        if (t->fd_capacity >= needed) return true;

        uint32_t new_cap = needed;
        auto **new_table = (vfs::open_file **)heap::kmalloc(sizeof(vfs::open_file *) * new_cap);
        if (!new_table) return false;

        memset(new_table, 0, sizeof(vfs::open_file *) * new_cap);

        if (t->fd_table) {
            memcpy(new_table, t->fd_table, sizeof(vfs::open_file *) * t->fd_capacity);
            heap::kfree(t->fd_table);
        }

        t->fd_table = new_table;
        t->fd_capacity = new_cap;
        return true;
    }

    int alloc_fd(scheduler::task *t) {
        for (uint32_t i = 0; i < t->fd_capacity; i++) {
            if (t->fd_table[i] == nullptr) return (int)i;
        }

        uint32_t old_cap = t->fd_capacity;
        uint32_t new_cap = (old_cap == 0) ? 8 : old_cap * 2;

        if (!expand_table(new_cap, t)) return -1;
        return (int)old_cap;
    }

    int close_fd(int fd, scheduler::task *t) {
        vfs::open_file *file = get_file(fd, t);
        if (!file) return -1;

        // Remove from table FIRST so other threads don't see it
        t->fd_table[fd] = nullptr;

        file->ref_count--;
        if (file->ref_count == 0) {
            // IMPORTANT: Only free the 'open_file' wrapper.
            // Do NOT call a destructor on file->node unless your VFS
            // specifically tracks node-wide open counts.
            heap::kfree(file);
        }

        return 0;
    }
}  // namespace fd_manager