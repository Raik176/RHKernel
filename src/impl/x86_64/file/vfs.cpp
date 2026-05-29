#include "file/vfs.h"

#include "file/module_loader.h"
#include "memory/heap.h"
#include "string.h"

static vfs::vfs_node *root_node = nullptr;
static spinlock_t vfs_lock;

namespace vfs {
    bool valid_node(vfs_node *node) { return node && node->magic == VFS_NODE_MAGIC; }

    static bool valid_fn(uintptr_t fn) { return fn != 0; }

    static constexpr uint64_t VFS_STACK_COMPONENT = 256;
    static constexpr uint64_t VFS_MAX_COMPONENT = 255;
    static constexpr uint64_t VFS_MAX_MEMORY_FILE = 64ULL * 1024 * 1024;
    static constexpr uint32_t VFS_CHILD_HASH_MIN = 16;
    static constexpr uint32_t VFS_CHILD_HASH_BUCKETS = 64;
    static constexpr uint32_t VFS_NEG_CACHE_ENTRIES = 16;
    static constexpr uint32_t VFS_NEG_CACHE_NAME = 32;

    static uint64_t hash_name_len(const char *s, uint64_t n) {
        uint64_t h = 1469598103934665603ULL;
        for (uint64_t i = 0; i < n; i++) {
            h ^= (uint8_t)s[i];
            h *= 1099511628211ULL;
        }
        return h ? h : 1;
    }


    static bool name_matches(vfs_node *node, const char *name, uint64_t name_len, uint64_t name_hash) {
        return node && node->name && node->name_len == name_len && node->name_hash == name_hash &&
               memcmp(node->name, name, name_len) == 0;
    }

    static uint32_t child_hash_bucket(vfs_node *parent, uint64_t hash) {
        return parent && parent->child_hash_buckets ? (uint32_t)(hash & (parent->child_hash_buckets - 1)) : 0;
    }

    static vfs_node *child_hash_find_locked(vfs_node *parent, const char *name, uint64_t name_len, uint64_t name_hash) {
        if (!parent || !parent->child_hash || !parent->child_hash_buckets) return nullptr;
        uint32_t bucket = child_hash_bucket(parent, name_hash);
        for (vfs_node *node = parent->child_hash[bucket]; node; node = node->hash_next)
            if (name_matches(node, name, name_len, name_hash)) return node;
        return nullptr;
    }

    static void child_hash_insert_locked(vfs_node *parent, vfs_node *child) {
        if (!parent || !child || !parent->child_hash || !parent->child_hash_buckets || !child->name) return;
        uint32_t bucket = child_hash_bucket(parent, child->name_hash);
        child->hash_next = parent->child_hash[bucket];
        parent->child_hash[bucket] = child;
    }

    static void child_hash_remove_locked(vfs_node *parent, vfs_node *child) {
        if (!parent || !child || !parent->child_hash || !parent->child_hash_buckets || !child->name) return;
        uint32_t bucket = child_hash_bucket(parent, child->name_hash);
        vfs_node **link = &parent->child_hash[bucket];
        while (*link) {
            if (*link == child) {
                *link = child->hash_next;
                child->hash_next = nullptr;
                return;
            }
            link = &(*link)->hash_next;
        }
        child->hash_next = nullptr;
    }

    static void child_hash_build_locked(vfs_node *parent, vfs_node **table, uint32_t buckets) {
        if (!parent || parent->child_hash || !table || !buckets) return;
        parent->child_hash = table;
        parent->child_hash_buckets = buckets;
        for (vfs_node *node = parent->child; node; node = node->next) {
            node->hash_next = nullptr;
            if (node->name) child_hash_insert_locked(parent, node);
        }
    }

    static bool neg_cache_name_equal(const vfs_negative_entry *entry, const char *name, uint64_t len) {
        return entry && len <= VFS_NEG_CACHE_NAME && entry->len == len && memcmp(entry->name, name, len) == 0;
    }

    static bool neg_cache_hit_locked(vfs_node *parent, const char *name, uint64_t len, uint64_t hash) {
        if (!parent || !parent->neg_cache || len > VFS_NEG_CACHE_NAME) return false;
        for (uint32_t i = 0; i < parent->neg_cache_count; i++) {
            vfs_negative_entry *entry = &parent->neg_cache[i];
            if (entry->seq == parent->child_seq && entry->hash == hash && neg_cache_name_equal(entry, name, len)) return true;
        }
        return false;
    }

    static void neg_cache_store(vfs_node *parent, const char *name, uint64_t len, uint64_t hash) {
        if (!parent || !name || len == 0 || len > VFS_NEG_CACHE_NAME) return;
        vfs_negative_entry *new_cache = nullptr;
        for (;;) {
            uint64_t flags;
            vfs_lock.acquire(flags);
            if (!valid_node(parent)) {
                vfs_lock.release(flags);
                if (new_cache) heap::kfree(new_cache);
                return;
            }
            if (!parent->neg_cache && !new_cache) {
                vfs_lock.release(flags);
                new_cache = (vfs_negative_entry *)heap::kzalloc(sizeof(vfs_negative_entry) * VFS_NEG_CACHE_ENTRIES);
                if (!new_cache) return;
                continue;
            }
            if (!parent->neg_cache && new_cache) {
                parent->neg_cache = new_cache;
                new_cache = nullptr;
            }
            uint32_t slot = parent->neg_cache_next;
            if (slot >= VFS_NEG_CACHE_ENTRIES) slot = 0;
            vfs_negative_entry *entry = &parent->neg_cache[slot];
            entry->hash = hash;
            entry->seq = parent->child_seq;
            entry->len = (uint16_t)len;
            memcpy(entry->name, name, len);
            if (len < VFS_NEG_CACHE_NAME) entry->name[len] = 0;
            parent->neg_cache_next = (slot + 1) & (VFS_NEG_CACHE_ENTRIES - 1);
            if (parent->neg_cache_count < VFS_NEG_CACHE_ENTRIES) parent->neg_cache_count++;
            vfs_lock.release(flags);
            if (new_cache) heap::kfree(new_cache);
            return;
        }
    }

    static char *dup_component(const char *s, uint64_t n) {
        if (!s || n == 0 || n > VFS_MAX_COMPONENT || n == UINT64_MAX) return nullptr;
        char *out = (char *)heap::kmalloc(n + 1);
        if (!out) return nullptr;
        memcpy(out, s, n);
        out[n] = 0;
        return out;
    }

    static char *dup_name_checked(const char *s, uint64_t *len_out) {
        if (!s || !len_out) return nullptr;
        uint64_t n = 0;
        while (s[n]) {
            if (n == VFS_MAX_COMPONENT) return nullptr;
            n++;
        }
        char *out = dup_component(s, n ? n : 1);
        if (!out) return nullptr;
        if (n == 0) out[0] = 0;
        *len_out = n;
        return out;
    }

    static bool component_len_checked(const char *s, uint64_t *len_out) {
        if (!s || !len_out) return false;
        uint64_t n = 0;
        while (s[n]) {
            if (n == VFS_MAX_COMPONENT) return false;
            n++;
        }
        if (!n) return false;
        *len_out = n;
        return true;
    }

    static bool valid_path(const char *path) { return path && path[0] != 0; }

    static const char *trim_trailing_slashes(const char *path, uint64_t *len_out) {
        if (!path || !len_out) return nullptr;
        uint64_t len = strlen(path);
        while (len > 1 && path[len - 1] == '/') len--;
        *len_out = len;
        return path;
    }

    static int split_parent_at(vfs_node *cwd, const char *path, vfs_node **parent_out,
                               char **name_out) {
        if (!valid_path(path) || !parent_out || !name_out) return -1;

        uint64_t len = 0;
        trim_trailing_slashes(path, &len);
        if (len == 0) return -1;
        if (len == 1 && path[0] == '/') return -1;

        uint64_t slash = len;
        while (slash > 0 && path[slash - 1] != '/') slash--;

        uint64_t name_start = slash;
        uint64_t name_len = len - name_start;
        if (name_len == 0 || name_len > VFS_MAX_COMPONENT) return -1;
        if ((name_len == 1 && path[name_start] == '.') ||
            (name_len == 2 && path[name_start] == '.' && path[name_start + 1] == '.'))
            return -1;

        vfs_node *parent = nullptr;
        if (slash == 0) {
            parent = path[0] == '/' ? root_node : cwd;
        } else if (slash == 1 && path[0] == '/') {
            parent = root_node;
        } else {
            uint64_t parent_len = slash;
            while (parent_len > 1 && path[parent_len - 1] == '/') parent_len--;
            char parent_stack[VFS_STACK_COMPONENT];
            char *parent_path = parent_stack;
            bool heap_parent_path = false;
            if (parent_len >= VFS_STACK_COMPONENT) {
                parent_path = dup_component(path, parent_len);
                heap_parent_path = true;
            } else {
                memcpy(parent_path, path, parent_len);
                parent_path[parent_len] = 0;
            }
            if (!parent_path) return -1;
            parent = open_at(cwd, parent_path);
            if (heap_parent_path) heap::kfree(parent_path);
        }

        if (!parent || parent->type != VfsType::VFS_DIRECTORY) return -1;

        char *name = dup_component(path + name_start, name_len);
        if (!name) return -1;
        *parent_out = parent;
        *name_out = name;
        return 0;
    }

    static vfs_node *detach_cached_child_len(vfs_node *parent, const char *name, uint64_t name_len) {
        if (!parent || !name) return nullptr;
        uint64_t name_hash = hash_name_len(name, name_len);
        vfs_node *target = child_hash_find_locked(parent, name, name_len, name_hash);
        vfs_node **link = &parent->child;
        while (*link) {
            vfs_node *node = *link;
            if (node == target || (!target && name_matches(node, name, name_len, name_hash))) {
                *link = node->next;
                child_hash_remove_locked(parent, node);
                node->next = nullptr;
                node->parent = nullptr;
                if (parent->child_count) parent->child_count--;
                parent->child_seq++;
                return node;
            }
            link = &node->next;
        }
        return nullptr;
    }

    static vfs_node *detach_cached_child(vfs_node *parent, const char *name) {
        return detach_cached_child_len(parent, name, name ? strlen(name) : 0);
    }

    vfs_node *detach_child(vfs_node *parent, const char *name) {
        if (!valid_node(parent) || !name) return nullptr;
        uint64_t flags;
        vfs_lock.acquire(flags);
        vfs_node *node = detach_cached_child(parent, name);
        if (node) node->unlinked = true;
        vfs_lock.release(flags);
        return node;
    }

    void detach_node(vfs_node *node) {
        if (!valid_node(node) || !node->parent) return;
        uint64_t flags;
        vfs_lock.acquire(flags);
        vfs_node *parent = node->parent;
        vfs_node **link = &parent->child;
        while (*link) {
            if (*link == node) {
                *link = node->next;
                child_hash_remove_locked(parent, node);
                node->next = nullptr;
                node->parent = nullptr;
                node->unlinked = true;
                if (parent->child_count) parent->child_count--;
                parent->child_seq++;
                break;
            }
            link = &(*link)->next;
        }
        vfs_lock.release(flags);
    }

    static constexpr uint64_t MEMORY_FILE_MAGIC = 0x4D454D46494C4531ULL;
    static constexpr uint64_t MEMORY_FILE_MIN_CAPACITY = 64;

    struct memory_file_storage {
        uint64_t magic;
        uint64_t capacity;
    };

    static uint64_t memory_file_read(vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
    static uint64_t memory_file_write(vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
    static int memory_file_truncate(vfs_node *node, uint64_t size);
    static void destroy_node(vfs_node *node);

    static memory_file_storage *memory_file_header(uintptr_t data) {
        if (data < sizeof(memory_file_storage)) return nullptr;
        auto *header = (memory_file_storage *)(data - sizeof(memory_file_storage));
        return header->magic == MEMORY_FILE_MAGIC ? header : nullptr;
    }

    static uint64_t memory_file_next_capacity(uint64_t size) {
        if (size <= MEMORY_FILE_MIN_CAPACITY) return MEMORY_FILE_MIN_CAPACITY;
        uint64_t cap = MEMORY_FILE_MIN_CAPACITY;
        while (cap < size) {
            if (cap > UINT64_MAX / 2) return size;
            cap *= 2;
        }
        return cap;
    }

    static bool memory_file_ensure_capacity(vfs_node *node, uint64_t size) {
        if (!node || size > VFS_MAX_MEMORY_FILE) return false;
        memory_file_storage *old = memory_file_header(node->ptr);
        if (old && old->capacity >= size) return true;

        uint64_t cap = memory_file_next_capacity(size);
        if (cap > UINT64_MAX - sizeof(memory_file_storage)) return false;
        auto *next = (memory_file_storage *)heap::kmalloc(sizeof(memory_file_storage) + cap);
        if (!next) return false;
        next->magic = MEMORY_FILE_MAGIC;
        next->capacity = cap;
        uint8_t *next_data = (uint8_t *)(next + 1);

        uint64_t keep = node->size < size ? node->size : size;
        if (keep && node->ptr) memcpy(next_data, (void *)node->ptr, keep);
        if (old) {
            old->magic = 0;
            heap::kfree(old);
        } else if (node->ptr) {
            heap::kfree((void *)node->ptr);
        }
        node->ptr = (uintptr_t)next_data;
        return true;
    }

    static void release_memory_node(vfs_node *node) {
        if (!node) return;
        if (node->type == VfsType::VFS_FILE && node->read == memory_file_read && node->ptr) {
            memory_file_storage *storage = memory_file_header(node->ptr);
            if (storage) {
                storage->magic = 0;
                heap::kfree(storage);
            } else {
                heap::kfree((void *)node->ptr);
            }
            node->ptr = 0;
        }
    }

    static void destroy_node(vfs_node *node) {
        if (!node) return;
        while (node->child) {
            vfs_node *child = node->child;
            node->child = child->next;
            if (node->child_count) node->child_count--;
            child->next = nullptr;
            child->hash_next = nullptr;
            child->parent = nullptr;
            child->unlinked = true;
            put_node(child);
        }
        if (node->child_hash) {
            heap::kfree(node->child_hash);
            node->child_hash = nullptr;
            node->child_hash_buckets = 0;
        }
        if (node->neg_cache) {
            heap::kfree(node->neg_cache);
            node->neg_cache = nullptr;
            node->neg_cache_count = 0;
            node->neg_cache_next = 0;
        }
        if (node->release) node->release(node);
        else release_memory_node(node);
        node->magic = 0;
        if (node->name) heap::kfree(node->name);
        heap::kfree(node);
    }

    static bool is_memory_node(vfs_node *node) {
        return node && !node->finddir && !node->readdir &&
               (!node->read || node->read == memory_file_read) &&
               (!node->write || node->write == memory_file_write) &&
               !node->create && !node->unlink && !node->rename &&
               (!node->truncate || node->truncate == memory_file_truncate);
    }

    static void make_memory_file(vfs_node *node) {
        if (!node || node->type != VfsType::VFS_FILE) return;
        node->read = memory_file_read;
        node->write = memory_file_write;
        node->truncate = memory_file_truncate;
    }

    static int attach_child_if_needed(vfs_node *parent, vfs_node *child) {
        if (!valid_node(parent) || !valid_node(child)) return -1;
        if (child->parent == parent) return 0;
        if (child->parent) return -1;

        vfs_node **new_hash = nullptr;
        bool hash_alloc_failed = false;
        for (;;) {
            uint64_t flags;
            vfs_lock.acquire(flags);
            if (child->parent == parent) {
                vfs_lock.release(flags);
                if (new_hash) heap::kfree(new_hash);
                return 0;
            }
            if (child->parent) {
                vfs_lock.release(flags);
                if (new_hash) heap::kfree(new_hash);
                return -1;
            }
            if (child->name && !child->name_hash) child->name_hash = hash_name_len(child->name, child->name_len);
            if (!parent->child_hash && parent->child_count + 1 >= VFS_CHILD_HASH_MIN && !new_hash && !hash_alloc_failed) {
                vfs_lock.release(flags);
                new_hash = (vfs_node **)heap::kzalloc(sizeof(vfs_node *) * VFS_CHILD_HASH_BUCKETS);
                if (!new_hash) hash_alloc_failed = true;
                continue;
            }
            if (!parent->child_hash && new_hash) {
                child_hash_build_locked(parent, new_hash, VFS_CHILD_HASH_BUCKETS);
                new_hash = nullptr;
            }

            vfs_node *same = child->name ? child_hash_find_locked(parent, child->name, child->name_len, child->name_hash) : nullptr;
            if (!same) {
                for (vfs_node *curr = parent->child; curr; curr = curr->next) {
                    if (curr == child || (child->name && name_matches(curr, child->name, child->name_len, child->name_hash))) {
                        same = curr;
                        break;
                    }
                }
            }
            if (same) {
                vfs_lock.release(flags);
                if (new_hash) heap::kfree(new_hash);
                return same == child ? 0 : -1;
            }
            child->parent = parent;
            child->next = parent->child;
            parent->child = child;
            parent->child_count++;
            parent->child_seq++;
            child_hash_insert_locked(parent, child);
            vfs_lock.release(flags);
            if (new_hash) heap::kfree(new_hash);
            return 0;
        }
    }

    int add_child(vfs_node *parent, vfs_node *child) { return attach_child_if_needed(parent, child); }

    vfs_node *get_node(vfs_node *node) {
        if (!valid_node(node)) return nullptr;
        uint64_t flags;
        vfs_lock.acquire(flags);
        if (!valid_node(node) || node->ref_count == UINT32_MAX) {
            vfs_lock.release(flags);
            return nullptr;
        }
        node->ref_count++;
        vfs_lock.release(flags);
        return node;
    }

    void put_node(vfs_node *node) {
        if (!valid_node(node)) return;
        bool destroy = false;
        uint64_t flags;
        vfs_lock.acquire(flags);
        if (valid_node(node) && node->ref_count > 0) {
            node->ref_count--;
            destroy = node->ref_count == 0;
        }
        vfs_lock.release(flags);
        if (destroy) destroy_node(node);
    }

    vfs_node *create_node(const char *name, VfsType type, vfs_node *parent) {
        vfs_node *node = (vfs_node *)heap::kzalloc(sizeof(vfs_node));
        if (!node) return nullptr;

        node->magic = VFS_NODE_MAGIC;
        node->name = dup_name_checked(name ? name : "", &node->name_len);
        if (!node->name) {
            heap::kfree(node);
            return nullptr;
        }
        node->name_hash = hash_name_len(node->name, node->name_len);
        node->type = type;
        node->ref_count = 1;

        if (parent && attach_child_if_needed(parent, node) != 0) {
            heap::kfree(node->name);
            heap::kfree(node);
            return nullptr;
        }
        return node;
    }

    void init() { root_node = create_node("/", VfsType::VFS_DIRECTORY, nullptr); }

    vfs_node *get_root() { return root_node; }

    static vfs_node *find_cached_child_len(vfs_node *parent, const char *name, uint64_t name_len, bool *negative) {
        if (negative) *negative = false;
        if (!parent || !name) return nullptr;
        uint64_t name_hash = hash_name_len(name, name_len);
        uint64_t flags;
        vfs_lock.acquire(flags);
        vfs_node *hit = child_hash_find_locked(parent, name, name_len, name_hash);
        if (!hit) {
            for (vfs_node *curr = parent->child; curr; curr = curr->next) {
                if (name_matches(curr, name, name_len, name_hash)) {
                    hit = curr;
                    break;
                }
            }
        }
        if (!hit && negative && neg_cache_hit_locked(parent, name, name_len, name_hash)) *negative = true;
        vfs_lock.release(flags);
        return hit;
    }

    static bool has_driver_lookup(vfs_node *node) {
        return node && node->finddir && valid_fn((uintptr_t)node->finddir);
    }

    static vfs_node *finddir_len(vfs_node *parent, const char *name, uint64_t name_len) {
        if (!valid_node(parent) || !name || name_len == 0) return nullptr;

        bool negative = false;
        vfs_node *cached = find_cached_child_len(parent, name, name_len, &negative);
        if (cached || negative || !has_driver_lookup(parent)) return cached;
        uint64_t name_hash = hash_name_len(name, name_len);

        char name_stack[VFS_STACK_COMPONENT];
        char *nul_name = name_stack;
        bool heap_name = false;
        if (name_len >= VFS_STACK_COMPONENT) {
            nul_name = dup_component(name, name_len);
            heap_name = true;
        } else {
            memcpy(nul_name, name, name_len);
            nul_name[name_len] = 0;
        }
        if (!nul_name) return nullptr;

        vfs_node *found = parent->finddir(parent, nul_name);
        if (heap_name) heap::kfree(nul_name);
        if (valid_node(found)) {
            (void)attach_child_if_needed(parent, found);
            return found;
        }
        neg_cache_store(parent, name, name_len, name_hash);
        return nullptr;
    }

    vfs_node *finddir(vfs_node *parent, const char *name) {
        if (!name) return nullptr;
        return finddir_len(parent, name, strlen(name));
    }

    int readdir(vfs_node *dir, uint64_t index, vfs_dirent *out) {
        if (!valid_node(dir) || !out || dir->type != VfsType::VFS_DIRECTORY) return -1;
        if (dir->readdir && valid_fn((uintptr_t)dir->readdir)) return dir->readdir(dir, index, out);
        if (index >= dir->child_count) return -1;

        uint64_t flags;
        vfs_lock.acquire(flags);
        vfs_node *curr = dir->child;
        uint64_t i = 0;
        while (curr) {
            if (i == index) {
                const char *name = curr->name ? curr->name : "";
                uint64_t len = curr->name ? curr->name_len : 0;
                char *dst = out->name;
                uint64_t cap = out->name_capacity;
                memset(out, 0, sizeof(*out));
                out->inode = curr->inode;
                out->type = (uint32_t)curr->type;
                out->name_len = len;
                out->name = dst;
                out->name_capacity = cap;
                if (dst && cap > len) memcpy(dst, name, len + 1);
                vfs_lock.release(flags);
                return 0;
            }
            i++;
            curr = curr->next;
        }
        vfs_lock.release(flags);
        return -1;
    }

    vfs_node *traverse_relative(vfs_node *start, const char *path) {
        if (!valid_node(start) || !path) return nullptr;

        vfs_node *curr = start;
        const char *p = path;

        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;

            const char *component = p;
            uint64_t len = 0;
            while (p[len] && p[len] != '/') {
                if (len == VFS_MAX_COMPONENT) return nullptr;
                len++;
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

            curr = finddir_len(curr, component, len);
            if (!curr) return nullptr;
            p += len;
        }
        return curr;
    }

    vfs_node *open_at(vfs_node *cwd, const char *path) {
        if (!valid_path(path) || !valid_node(root_node)) return nullptr;
        if (path[0] == '/') {
            if (path[1] == 0) return root_node;
            return traverse_relative(root_node, path + 1);
        }
        return traverse_relative(cwd ? cwd : root_node, path);
    }

    vfs_node *open(const char *path) { return open_at(root_node, path); }

    vfs_node *create_at(vfs_node *cwd, const char *path, VfsType type) {
        vfs_node *parent = nullptr;
        char *name = nullptr;
        if (split_parent_at(cwd ? cwd : root_node, path, &parent, &name) != 0) return nullptr;
        if (!valid_node(parent) || parent->type != VfsType::VFS_DIRECTORY || !name || name[0] == 0) {
            heap::kfree(name);
            return nullptr;
        }

        vfs_node *existing = finddir(parent, name);
        if (existing) {
            heap::kfree(name);
            return existing;
        }

        vfs_node *out = nullptr;
        int rc = -1;
        if (parent->create && valid_fn((uintptr_t)parent->create)) {
            rc = parent->create(parent, name, (uint32_t)type, &out);
        } else {
            out = create_node(name, type, parent);
            if (out && type == VfsType::VFS_FILE) make_memory_file(out);
            rc = out ? 0 : -1;
        }

        if (rc == 0 && valid_node(out) && attach_child_if_needed(parent, out) != 0) rc = -1;
        heap::kfree(name);
        return (rc == 0 && valid_node(out)) ? out : nullptr;
    }

    vfs_node *create(const char *path, VfsType type) { return create_at(root_node, path, type); }

    int unlink_at(vfs_node *cwd, const char *path) {
        vfs_node *parent = nullptr;
        char *name = nullptr;
        if (split_parent_at(cwd ? cwd : root_node, path, &parent, &name) != 0) return -1;
        if (!valid_node(parent)) { heap::kfree(name); return -1; }
        int rc = -1;
        vfs_node *detached = nullptr;
        if (parent->unlink && valid_fn((uintptr_t)parent->unlink)) {
            rc = parent->unlink(parent, name);
            if (rc == 0) detached = detach_cached_child(parent, name);
        } else {
            vfs_node *node = finddir(parent, name);
            if (node && node->parent == parent && is_memory_node(node)) {
                detached = detach_cached_child(parent, name);
                rc = detached ? 0 : -1;
            }
        }
        heap::kfree(name);
        if (detached) detached->unlinked = true;
        if (detached && is_memory_node(detached)) put_node(detached);
        return rc;
    }

    int unlink(const char *path) { return unlink_at(root_node, path); }

    int rename_at(vfs_node *cwd, const char *old_path, const char *new_path) {
        vfs_node *base = cwd ? cwd : root_node;
        vfs_node *old_parent = nullptr;
        vfs_node *new_parent = nullptr;
        char *old_name = nullptr;
        char *new_name = nullptr;
        if (split_parent_at(base, old_path, &old_parent, &old_name) != 0) return -1;
        if (split_parent_at(base, new_path, &new_parent, &new_name) != 0) {
            heap::kfree(old_name);
            return -1;
        }
        uint64_t old_len = 0;
        uint64_t new_len = 0;
        if (!valid_node(old_parent) || !valid_node(new_parent) ||
            !component_len_checked(old_name, &old_len) || !component_len_checked(new_name, &new_len)) {
            heap::kfree(old_name);
            heap::kfree(new_name);
            return -1;
        }
        int rc = -1;
        if (old_parent->rename && valid_fn((uintptr_t)old_parent->rename)) {
            rc = old_parent->rename(old_parent, old_name, new_parent, new_name);
            if (rc == 0) {
                vfs_node *old_detached = detach_cached_child_len(old_parent, old_name, old_len);
                vfs_node *new_detached = detach_cached_child_len(new_parent, new_name, new_len);
                if (old_detached) old_detached->unlinked = true;
                if (new_detached) new_detached->unlinked = true;
                if (old_detached && is_memory_node(old_detached)) put_node(old_detached);
                if (new_detached && is_memory_node(new_detached)) put_node(new_detached);
            }
        } else {
            vfs_node *node = finddir_len(old_parent, old_name, old_len);
            if (node && node->parent == old_parent && is_memory_node(node) && !finddir_len(new_parent, new_name, new_len)) {
                vfs_node *detached = detach_cached_child_len(old_parent, old_name, old_len);
                char *copy = dup_component(new_name, new_len);
                if (detached && copy) {
                    heap::kfree(detached->name);
                    detached->name = copy;
                    detached->name_len = new_len;
                    detached->name_hash = hash_name_len(copy, detached->name_len);
                    rc = attach_child_if_needed(new_parent, detached);
                }
                if (rc != 0) {
                    if (copy && (!detached || detached->name != copy)) heap::kfree(copy);
                    if (detached) {
                        (void)attach_child_if_needed(old_parent, detached);
                    }
                }
            }
        }
        heap::kfree(old_name);
        heap::kfree(new_name);
        return rc;
    }

    int rename(const char *old_path, const char *new_path) {
        return rename_at(root_node, old_path, new_path);
    }

    static uint64_t memory_file_read(vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
        if (!node || !buffer || offset >= node->size) return 0;
        uint64_t n = node->size - offset;
        if (n > size) n = size;
        if (!n) return 0;
        if (!node->ptr || node->ptr > UINTPTR_MAX - offset) return 0;
        memcpy(buffer, (void *)(node->ptr + offset), n);
        return n;
    }

    static int memory_file_truncate(vfs_node *node, uint64_t size) {
        if (!node || node->type != VfsType::VFS_FILE) return -1;
        if (size == 0) {
            if (node->ptr) {
                memory_file_storage *storage = memory_file_header(node->ptr);
                if (storage) {
                    storage->magic = 0;
                    heap::kfree(storage);
                } else {
                    heap::kfree((void *)node->ptr);
                }
            }
            node->ptr = 0;
            node->size = 0;
            return 0;
        }

        uint64_t old_size = node->size;
        if (!memory_file_ensure_capacity(node, size)) return -1;
        if (size > old_size) memset((void *)(node->ptr + old_size), 0, size - old_size);
        node->size = size;
        return 0;
    }

    static uint64_t memory_file_write(vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
        if (!node || (size && !buffer)) return 0;
        if (size == 0) return 0;
        if (size > UINT64_MAX - offset) return 0;
        uint64_t end = offset + size;
        if (end > VFS_MAX_MEMORY_FILE) return 0;
        if (end > node->size && memory_file_truncate(node, end) != 0) return 0;
        if (size && (!node->ptr || node->ptr > UINTPTR_MAX - offset)) return 0;
        memcpy((void *)(node->ptr + offset), buffer, size);
        return size;
    }

    int truncate(vfs_node *node, uint64_t size) {
        if (!valid_node(node) || !node->truncate || !valid_fn((uintptr_t)node->truncate)) return -1;
        return node->truncate(node, size);
    }

    uint64_t read(vfs_node *node, uint64_t offset, uint64_t size, void *buffer) {
        if (!valid_node(node) || (size && !buffer)) return 0;
        if (node->read && valid_fn((uintptr_t)node->read)) return node->read(node, offset, size, (uint8_t *)buffer);
        if (node->type == VfsType::VFS_FILE) {
            if (offset >= node->size) return 0;
            uint64_t remaining = node->size - offset;
            uint64_t read_size = size < remaining ? size : remaining;
            if (read_size == 0) return 0;
            if (node->ptr > UINTPTR_MAX - offset) return 0;
            memcpy(buffer, (void *)(node->ptr + offset), read_size);
            return read_size;
        }
        return 0;
    }

    uint64_t write(vfs_node *node, uint64_t offset, uint64_t size, void *buffer) {
        if (!valid_node(node) || (size && !buffer)) return 0;
        if (node->write && valid_fn((uintptr_t)node->write)) return node->write(node, offset, size, (uint8_t *)buffer);
        return 0;
    }
}  // namespace vfs
