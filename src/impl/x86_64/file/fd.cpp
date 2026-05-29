#include "file/fd.h"

#include "file/device.h"
#include "memory/heap.h"
#include "string.h"

namespace fd_manager {
    static constexpr uint64_t FD_BITS_PER_WORD = 64;

    static uint64_t fd_bitmap_words(uint64_t capacity) {
        return (capacity + FD_BITS_PER_WORD - 1) / FD_BITS_PER_WORD;
    }

    static bool fd_bit_get(const scheduler::task *t, uint64_t fd) {
        return t && t->fd_bitmap && fd < t->fd_capacity &&
               (t->fd_bitmap[fd / FD_BITS_PER_WORD] & (1ULL << (fd % FD_BITS_PER_WORD)));
    }

    static void fd_bit_set(scheduler::task *t, uint64_t fd) {
        if (t && t->fd_bitmap && fd < t->fd_capacity)
            t->fd_bitmap[fd / FD_BITS_PER_WORD] |= 1ULL << (fd % FD_BITS_PER_WORD);
    }

    static void fd_bit_clear(scheduler::task *t, uint64_t fd) {
        if (t && t->fd_bitmap && fd < t->fd_capacity)
            t->fd_bitmap[fd / FD_BITS_PER_WORD] &= ~(1ULL << (fd % FD_BITS_PER_WORD));
    }

    static uint64_t fd_last_word_mask(uint64_t capacity) {
        uint64_t used = capacity % FD_BITS_PER_WORD;
        return used ? ((1ULL << used) - 1) : UINT64_MAX;
    }

    static int find_clear_bit(uint64_t word) {
        uint64_t free_bits = ~word;
        if (!free_bits) return -1;
        return __builtin_ctzll(free_bits);
    }

    vfs::open_file *get_file(int fd, scheduler::task *t) {
        if (!t || fd < 0 || (uint64_t)fd >= t->fd_capacity || !t->fd_table) return nullptr;
        return t->fd_table[fd];
    }

    bool expand_table(uint32_t needed, scheduler::task *t) {
        if (!t) return false;
        if (t->fd_capacity >= needed && t->fd_table && t->fd_bitmap) return true;

        uint64_t new_cap = t->fd_capacity ? t->fd_capacity : scheduler::INITIAL_FD_CAPACITY;
        while (new_cap < needed) {
            if (new_cap > UINT32_MAX / 2) {
                new_cap = needed;
                break;
            }
            new_cap *= 2;
        }
        if (new_cap > UINT32_MAX) return false;

        auto **new_table = (vfs::open_file **)heap::kcalloc(new_cap, sizeof(vfs::open_file *));
        if (!new_table) return false;

        uint64_t new_words = fd_bitmap_words(new_cap);
        uint64_t *new_bitmap = (uint64_t *)heap::kcalloc(new_words, sizeof(uint64_t));
        if (!new_bitmap) {
            heap::kfree(new_table);
            return false;
        }

        if (t->fd_table) {
            memcpy(new_table, t->fd_table, sizeof(vfs::open_file *) * t->fd_capacity);
            heap::kfree(t->fd_table);
        }
        if (t->fd_bitmap) {
            memcpy(new_bitmap, t->fd_bitmap, sizeof(uint64_t) * fd_bitmap_words(t->fd_capacity));
            heap::kfree(t->fd_bitmap);
        } else {
            for (uint64_t i = 0; i < t->fd_capacity; i++) {
                if (new_table[i]) new_bitmap[i / FD_BITS_PER_WORD] |= 1ULL << (i % FD_BITS_PER_WORD);
            }
        }

        t->fd_table = new_table;
        t->fd_bitmap = new_bitmap;
        t->fd_capacity = new_cap;
        if (t->next_fd_hint > t->fd_capacity) t->next_fd_hint = t->fd_capacity;
        return true;
    }

    int alloc_fd(scheduler::task *t) {
        if (!t || !expand_table(scheduler::INITIAL_FD_CAPACITY, t)) return -1;
        if (t->next_fd_hint > t->fd_capacity) t->next_fd_hint = t->fd_capacity;

        uint64_t start_word = t->next_fd_hint / FD_BITS_PER_WORD;
        uint64_t words = fd_bitmap_words(t->fd_capacity);
        for (uint64_t pass = 0; pass < 2; pass++) {
            uint64_t begin = pass == 0 ? start_word : 0;
            uint64_t end = pass == 0 ? words : start_word;
            for (uint64_t w = begin; w < end; w++) {
                uint64_t word = t->fd_bitmap[w];
                if (w == words - 1) word |= ~fd_last_word_mask(t->fd_capacity);
                if (word == UINT64_MAX) continue;
                int bit = find_clear_bit(word);
                if (bit < 0) continue;
                uint64_t fd = w * FD_BITS_PER_WORD + (uint64_t)bit;
                if (fd >= t->fd_capacity) continue;
                fd_bit_set(t, fd);
                t->next_fd_hint = fd + 1;
                return (int)fd;
            }
        }

        uint64_t old_cap = t->fd_capacity;
        uint64_t needed = old_cap ? old_cap + 1 : scheduler::INITIAL_FD_CAPACITY;
        if (needed > UINT32_MAX || !expand_table((uint32_t)needed, t)) return -1;
        fd_bit_set(t, old_cap);
        t->next_fd_hint = old_cap + 1;
        return (int)old_cap;
    }

    bool reserve_fd(int fd, scheduler::task *t) {
        if (!t || fd < 0 || fd == INT32_MAX) return false;
        if (!expand_table((uint32_t)fd + 1, t)) return false;
        fd_bit_set(t, (uint64_t)fd);
        if ((uint64_t)fd == t->next_fd_hint) {
            while (t->next_fd_hint < t->fd_capacity && fd_bit_get(t, t->next_fd_hint)) t->next_fd_hint++;
        }
        return true;
    }

    void release_reserved_fd(int fd, scheduler::task *t) {
        if (!t || fd < 0 || (uint64_t)fd >= t->fd_capacity) return;
        if (t->fd_table && t->fd_table[fd]) return;
        fd_bit_clear(t, (uint64_t)fd);
        if ((uint64_t)fd < t->next_fd_hint) t->next_fd_hint = (uint64_t)fd;
    }

    int close_fd(int fd, scheduler::task *t) {
        vfs::open_file *file = get_file(fd, t);
        if (!file) return -1;

        t->fd_table[fd] = nullptr;
        fd_bit_clear(t, (uint64_t)fd);
        if ((uint64_t)fd < t->next_fd_hint) t->next_fd_hint = (uint64_t)fd;

        bool free_file = false;
        uint64_t flags;
        spinlock_acquire(&file->lock, &flags);
        if (file->ref_count > 0) file->ref_count--;
        free_file = file->ref_count == 0;
        spinlock_release(&file->lock, flags);

        if (free_file) {
            devfs_close_file(file);
            vfs::put_node(file->node);
            heap::kfree(file);
        }
        return 0;
    }
}  // namespace fd_manager
