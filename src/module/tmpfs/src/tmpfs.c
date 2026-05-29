#include "mod/fs.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "string.h"

#define TMPFS_PAGE_SIZE 4096u
#define TMPFS_DEFAULT_LIMIT (16u * 1024u * 1024u)
#define TMPFS_NAME_MAX 255u
#define TMPFS_ROOT_INO 1u
#define TMPFS_PAGE_SHIFT 12u
#define TMPFS_PAGE_MASK (TMPFS_PAGE_SIZE - 1u)

struct tmpfs_mount;

struct tmpfs_node {
    struct tmpfs_mount *mnt;
    char *name;
    uint64_t name_len;
    uint32_t ino;
    uint32_t type;
    uint32_t mode;
    uint64_t size;
    struct tmpfs_node *parent;
    struct tmpfs_node *children;
    struct tmpfs_node *next;
    uint8_t **pages;
    uint64_t page_count;
    uint64_t page_capacity;
};

struct tmpfs_mount {
    uint64_t limit;
    uint64_t used;
    uint32_t next_ino;
    uint32_t enforce_limit;
    uint32_t default_mode;
    struct tmpfs_node *root;
};

static uint64_t parse_uint(const char *s, const char **end) {
    uint64_t v = 0;
    if (!s || *s < '0' || *s > '9') { if (end) *end = s; return UINT64_MAX; }
    while (*s >= '0' && *s <= '9') {
        uint64_t n = v * 10u + (uint64_t)(*s - '0');
        if (n < v) { if (end) *end = s; return UINT64_MAX; }
        v = n;
        s++;
    }
    if (end) *end = s;
    return v;
}

static uint32_t parse_octal(const char *s, const char **end) {
    uint32_t v = 0;
    if (!s || *s < '0' || *s > '7') { if (end) *end = s; return UINT32_MAX; }
    while (*s >= '0' && *s <= '7') {
        uint32_t n = (v << 3) | (uint32_t)(*s - '0');
        if (n < v) { if (end) *end = s; return UINT32_MAX; }
        v = n;
        s++;
    }
    if (end) *end = s;
    return v;
}

static int name_len_valid(const char *name, uint64_t *len_out) {
    if (!name || !len_out) return 0;
    uint64_t len = 0;
    while (name[len]) {
        if (len == TMPFS_NAME_MAX || name[len] == '/') return 0;
        len++;
    }
    if (!len || (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return 0;
    *len_out = len;
    return 1;
}

static int name_valid(const char *name) {
    uint64_t len;
    return name_len_valid(name, &len);
}

static int account(struct tmpfs_mount *mnt, uint64_t add) {
    if (!mnt) return -1;
    if (mnt->enforce_limit && add > mnt->limit - mnt->used) return -1;
    mnt->used += add;
    return 0;
}

static void unaccount(struct tmpfs_mount *mnt, uint64_t sub) {
    if (!mnt) return;
    mnt->used = sub > mnt->used ? 0 : mnt->used - sub;
}

static struct tmpfs_node *find_child_len(struct tmpfs_node *dir, const char *name, uint64_t len) {
    if (!dir || !name) return 0;
    for (struct tmpfs_node *n = dir->children; n; n = n->next)
        if (n->name_len == len && memcmp(n->name, name, len) == 0) return n;
    return 0;
}

static struct tmpfs_node *find_child(struct tmpfs_node *dir, const char *name) {
    uint64_t len;
    return name_len_valid(name, &len) ? find_child_len(dir, name, len) : 0;
}

static struct tmpfs_node *alloc_node_len(struct tmpfs_mount *mnt, const char *name, uint64_t name_len, uint32_t type, uint32_t mode) {
    if (!mnt || !name || !name_len || name_len > TMPFS_NAME_MAX) return 0;
    struct tmpfs_node *n = (struct tmpfs_node *)kmalloc(sizeof(*n));
    if (!n) return 0;
    memset(n, 0, sizeof(*n));
    n->name = (char *)kmalloc(name_len + 1u);
    if (!n->name) { kfree(n); return 0; }
    memcpy(n->name, name, name_len);
    n->name[name_len] = 0;
    n->name_len = name_len;
    n->mnt = mnt;
    n->ino = mnt->next_ino++;
    n->type = type;
    n->mode = mode;
    return n;
}

static struct tmpfs_node *alloc_node(struct tmpfs_mount *mnt, const char *name, uint32_t type, uint32_t mode) {
    uint64_t len;
    return name_len_valid(name, &len) ? alloc_node_len(mnt, name, len, type, mode) : 0;
}

static void free_node(struct tmpfs_node *n) {
    if (!n) return;
    while (n->children) {
        struct tmpfs_node *c = n->children;
        n->children = c->next;
        free_node(c);
    }
    for (uint64_t i = 0; i < n->page_capacity; i++) {
        if (n->pages && n->pages[i]) {
            kfree(n->pages[i]);
            unaccount(n->mnt, TMPFS_PAGE_SIZE);
        }
    }
    if (n->pages) kfree(n->pages);
    if (n->name) kfree(n->name);
    kfree(n);
}

static uint32_t vnode_type(uint32_t type) {
    return type == VFS_NODE_DIRECTORY ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
}

static void attach_ops(struct vfs_node *vnode, uint32_t type);

static struct vfs_node *make_vnode(struct tmpfs_node *n) {
    if (!n) return 0;
    struct vfs_node *v = vfs_create_fs_node(n->name, vnode_type(n->type), n->ino, n->type == VFS_NODE_FILE ? n->size : 0, n);
    if (!v) return 0;
    attach_ops(v, n->type);
    return v;
}

static struct vfs_node *tmpfs_finddir(struct vfs_node *parent, const char *name) {
    struct tmpfs_node *dir = (struct tmpfs_node *)vfs_get_fs_data(parent);
    if (!dir || dir->type != VFS_NODE_DIRECTORY || !name) return 0;
    struct tmpfs_node *n = find_child(dir, name);
    struct vfs_node *v = make_vnode(n);
    if (v) vfs_add_child(parent, v);
    return v;
}

static int tmpfs_readdir(struct vfs_node *parent, uint64_t index, struct vfs_dirent *out) {
    struct tmpfs_node *dir = (struct tmpfs_node *)vfs_get_fs_data(parent);
    if (!dir || dir->type != VFS_NODE_DIRECTORY || !out) return -1;
    struct tmpfs_node *n = dir->children;
    while (n && index--) n = n->next;
    if (!n) return -1;
    char *dst = out->name;
    uint64_t cap = out->name_capacity;
    uint64_t len = n->name_len;
    memset(out, 0, sizeof(*out));
    out->inode = n->ino;
    out->type = vnode_type(n->type);
    out->name_len = len;
    out->name = dst;
    out->name_capacity = cap;
    if (dst && cap > len) memcpy(dst, n->name, len + 1);
    return 0;
}

static int ensure_page_array(struct tmpfs_node *n, uint64_t pages) {
    if (pages <= n->page_capacity) return 0;
    uint64_t cap = n->page_capacity ? n->page_capacity : 4;
    while (cap < pages) {
        if (cap > UINT64_MAX / 2) return -1;
        cap *= 2;
    }
    if (cap > UINT64_MAX / sizeof(uint8_t *)) return -1;
    uint8_t **new_pages = (uint8_t **)krealloc(n->pages, cap * sizeof(uint8_t *));
    if (!new_pages) return -1;
    for (uint64_t i = n->page_capacity; i < cap; i++) new_pages[i] = 0;
    n->pages = new_pages;
    n->page_capacity = cap;
    return 0;
}

static int ensure_file_capacity(struct tmpfs_node *n, uint64_t end) {
    if (end > UINT64_MAX - TMPFS_PAGE_MASK) return -1;
    uint64_t pages = (end + TMPFS_PAGE_MASK) >> TMPFS_PAGE_SHIFT;
    if (ensure_page_array(n, pages) != 0) return -1;
    if (pages > n->page_count) n->page_count = pages;
    return 0;
}

static int ensure_data_page(struct tmpfs_node *n, uint64_t page) {
    if (page > UINT64_MAX / TMPFS_PAGE_SIZE - 1u) return -1;
    if (page >= n->page_capacity && ensure_file_capacity(n, (page + 1u) << TMPFS_PAGE_SHIFT) != 0) return -1;
    if (n->pages[page]) return 0;
    if (account(n->mnt, TMPFS_PAGE_SIZE) != 0) return -1;
    n->pages[page] = (uint8_t *)kmalloc(TMPFS_PAGE_SIZE);
    if (!n->pages[page]) { unaccount(n->mnt, TMPFS_PAGE_SIZE); return -1; }
    memset(n->pages[page], 0, TMPFS_PAGE_SIZE);
    return 0;
}

static uint64_t tmpfs_read(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf) {
    struct tmpfs_node *n = (struct tmpfs_node *)vfs_get_fs_data(vnode);
    if (!n || n->type != VFS_NODE_FILE || !buf || off >= n->size) return 0;
    if (size > n->size - off) size = n->size - off;
    uint64_t done = 0;
    while (done < size) {
        uint64_t abs = off + done;
        uint64_t page = abs >> TMPFS_PAGE_SHIFT;
        uint64_t in = abs & TMPFS_PAGE_MASK;
        uint64_t take = TMPFS_PAGE_SIZE - in;
        if (take > size - done) take = size - done;
        if (page < n->page_capacity && n->pages[page]) memcpy(buf + done, n->pages[page] + in, take);
        else memset(buf + done, 0, take);
        done += take;
    }
    return done;
}

static uint64_t tmpfs_write(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf) {
    struct tmpfs_node *n = (struct tmpfs_node *)vfs_get_fs_data(vnode);
    if (!n || n->type != VFS_NODE_FILE || !buf) return 0;
    if (size && off > UINT64_MAX - size) return 0;
    if (ensure_file_capacity(n, off + size) != 0) return 0;
    uint64_t done = 0;
    while (done < size) {
        uint64_t abs = off + done;
        uint64_t page = abs >> TMPFS_PAGE_SHIFT;
        uint64_t in = abs & TMPFS_PAGE_MASK;
        uint64_t take = TMPFS_PAGE_SIZE - in;
        if (take > size - done) take = size - done;
        if (ensure_data_page(n, page) != 0) break;
        memcpy(n->pages[page] + in, buf + done, take);
        done += take;
    }
    if (off + done > n->size) {
        n->size = off + done;
        vfs_set_size(vnode, n->size);
    }
    return done;
}

static int tmpfs_truncate(struct vfs_node *vnode, uint64_t size) {
    struct tmpfs_node *n = (struct tmpfs_node *)vfs_get_fs_data(vnode);
    if (!n || n->type != VFS_NODE_FILE) return -1;
    if (size > UINT64_MAX - TMPFS_PAGE_MASK) return -1;
    uint64_t keep = size ? (size + TMPFS_PAGE_MASK) >> TMPFS_PAGE_SHIFT : 0;
    if (size > n->size && ensure_file_capacity(n, size) != 0) return -1;
    for (uint64_t i = keep; i < n->page_count; i++) {
        if (n->pages[i]) {
            kfree(n->pages[i]);
            n->pages[i] = 0;
            unaccount(n->mnt, TMPFS_PAGE_SIZE);
        }
    }
    uint64_t tail = size & TMPFS_PAGE_MASK;
    if (keep && tail && n->pages[keep - 1]) memset(n->pages[keep - 1] + tail, 0, TMPFS_PAGE_SIZE - tail);
    n->page_count = keep;
    n->size = size;
    vfs_set_size(vnode, size);
    return 0;
}

static int tmpfs_create(struct vfs_node *parent, const char *name, uint32_t type, struct vfs_node **out) {
    struct tmpfs_node *dir = (struct tmpfs_node *)vfs_get_fs_data(parent);
    uint64_t name_len;
    if (!dir || dir->type != VFS_NODE_DIRECTORY || !name_len_valid(name, &name_len) || find_child_len(dir, name, name_len)) return -1;
    if (type != VFS_NODE_FILE && type != VFS_NODE_DIRECTORY) return -1;
    struct tmpfs_node *n = alloc_node_len(dir->mnt, name, name_len, type, dir->mnt->default_mode);
    if (!n) return -1;
    n->parent = dir;
    n->next = dir->children;
    dir->children = n;
    if (out) {
        *out = make_vnode(n);
        if (!*out) {
            dir->children = n->next;
            free_node(n);
            return -1;
        }
    }
    return 0;
}

static int tmpfs_unlink(struct vfs_node *parent, const char *name) {
    struct tmpfs_node *dir = (struct tmpfs_node *)vfs_get_fs_data(parent);
    uint64_t name_len;
    if (!dir || dir->type != VFS_NODE_DIRECTORY || !name_len_valid(name, &name_len)) return -1;
    struct tmpfs_node **link = &dir->children;
    while (*link) {
        struct tmpfs_node *n = *link;
        if (n->name_len == name_len && memcmp(n->name, name, name_len) == 0) {
            if (n->type == VFS_NODE_DIRECTORY && n->children) return -1;
            *link = n->next;
            free_node(n);
            return 0;
        }
        link = &n->next;
    }
    return -1;
}

static int tmpfs_rename(struct vfs_node *old_parent, const char *old_name, struct vfs_node *new_parent, const char *new_name) {
    struct tmpfs_node *od = (struct tmpfs_node *)vfs_get_fs_data(old_parent);
    struct tmpfs_node *nd = (struct tmpfs_node *)vfs_get_fs_data(new_parent);
    if (!od || !nd || od->type != VFS_NODE_DIRECTORY || nd->type != VFS_NODE_DIRECTORY) return -1;
    uint64_t old_len, new_len;
    if (!name_len_valid(old_name, &old_len) || !name_len_valid(new_name, &new_len) || find_child_len(nd, new_name, new_len)) return -1;
    struct tmpfs_node **link = &od->children;
    while (*link && ((*link)->name_len != old_len || memcmp((*link)->name, old_name, old_len) != 0)) link = &(*link)->next;
    if (!*link) return -1;
    struct tmpfs_node *n = *link;
    char *copy = (char *)kmalloc(new_len + 1u);
    if (!copy) return -1;
    memcpy(copy, new_name, new_len);
    copy[new_len] = 0;
    *link = n->next;
    kfree(n->name);
    n->name = copy;
    n->name_len = new_len;
    n->parent = nd;
    n->next = nd->children;
    nd->children = n;
    return 0;
}

static void attach_ops(struct vfs_node *vnode, uint32_t type) {
    if (type == VFS_NODE_DIRECTORY) {
        vfs_set_finddir(vnode, tmpfs_finddir);
        vfs_set_readdir(vnode, tmpfs_readdir);
        vfs_set_create(vnode, tmpfs_create);
        vfs_set_unlink(vnode, tmpfs_unlink);
        vfs_set_rename(vnode, tmpfs_rename);
    } else {
        vfs_set_read(vnode, tmpfs_read);
        vfs_set_write(vnode, tmpfs_write);
        vfs_set_truncate(vnode, tmpfs_truncate);
    }
}

static int parse_opts(const char *flags, struct tmpfs_mount *mnt) {
    mnt->limit = TMPFS_DEFAULT_LIMIT;
    mnt->default_mode = 0644;
    if (!flags || !*flags) return 0;
    const char *p = flags;
    while (*p) {
        const char *e = p;
        while (*e && *e != ',') e++;
        size_t len = (size_t)(e - p);
        if (len > 5 && strncmp(p, "size=", 5) == 0) {
            const char *end;
            uint64_t v = parse_uint(p + 5, &end);
            if (v == UINT64_MAX) return -1;
            if (*end == 'K' || *end == 'k') { if (v > UINT64_MAX / 1024u) return -1; v *= 1024u; end++; }
            else if (*end == 'M' || *end == 'm') { if (v > UINT64_MAX / (1024u * 1024u)) return -1; v *= 1024u * 1024u; end++; }
            else if (*end == 'G' || *end == 'g') { if (v > UINT64_MAX / (1024ull * 1024ull * 1024ull)) return -1; v *= 1024ull * 1024ull * 1024ull; end++; }
            if ((size_t)(end - p) != len || !v) return -1;
            mnt->limit = v;
        } else if (len > 5 && strncmp(p, "mode=", 5) == 0) {
            const char *end;
            uint32_t v = parse_octal(p + 5, &end);
            if (v == UINT32_MAX || (size_t)(end - p) != len || v > 07777u) return -1;
            mnt->default_mode = v;
        } else if (len && strncmp(p, "rw", len) != 0 && strncmp(p, "defaults", len) != 0) return -1;
        p += len;
        if (*p == ',') p++;
    }
    return 0;
}

static int mount_common(struct vfs_node *mountpoint, const char *flags, uint32_t enforce) {
    struct tmpfs_mount *mnt = (struct tmpfs_mount *)kmalloc(sizeof(*mnt));
    if (!mnt) return -1;
    memset(mnt, 0, sizeof(*mnt));
    mnt->enforce_limit = enforce;
    mnt->next_ino = TMPFS_ROOT_INO + 1;
    if (parse_opts(flags, mnt) != 0) { kfree(mnt); return -2; }
    struct tmpfs_node *root = alloc_node(mnt, "tmpfs", VFS_NODE_DIRECTORY, mnt->default_mode);
    if (!root) { kfree(mnt); return -1; }
    root->ino = TMPFS_ROOT_INO;
    mnt->root = root;
    vfs_set_fs_data(mountpoint, root);
    vfs_set_size(mountpoint, 0);
    attach_ops(mountpoint, VFS_NODE_DIRECTORY);
    return 0;
}

static int tmpfs_mount(struct vfs_node *blockdev, struct vfs_node *mountpoint, const char *flags) {
    (void)blockdev;
    int r = mount_common(mountpoint, flags, 1);
    if (r == 0) klog(LOG_INFO, "tmpfs: mounted\n");
    return r;
}

static int ramfs_mount(struct vfs_node *blockdev, struct vfs_node *mountpoint, const char *flags) {
    (void)blockdev;
    int r = mount_common(mountpoint, flags, 0);
    if (r == 0) klog(LOG_INFO, "ramfs: mounted\n");
    return r;
}

static int tmpfs_unmount(struct vfs_node *mountpoint) {
    struct tmpfs_node *root = (struct tmpfs_node *)vfs_get_fs_data(mountpoint);
    if (!root || !root->mnt) return -1;
    struct tmpfs_mount *mnt = root->mnt;
    free_node(root);
    kfree(mnt);
    return 0;
}

static int nodev_probe(struct vfs_node *node) { (void)node; return FS_PROBE_UNSUPPORTED; }

static struct fs_driver tmpfs_driver = { .name = "tmpfs", .mount = tmpfs_mount, .next = 0, .probe = nodev_probe, .unmount = tmpfs_unmount, .flags = FS_DRIVER_NODEV };
static struct fs_driver ramfs_driver = { .name = "ramfs", .mount = ramfs_mount, .next = 0, .probe = nodev_probe, .unmount = tmpfs_unmount, .flags = FS_DRIVER_NODEV };

static int tmpfs_init(void) {
    int r = fs_register(&tmpfs_driver);
    if (r != 0) return r;
    return fs_register(&ramfs_driver);
}

MODULE_INFO("tmpfs", tmpfs_init, 0, 0, "fs");
