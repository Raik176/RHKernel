#include "mod/fs.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "string.h"

#define ISO_SECTOR 2048u
#define ISO_VD_START 16u
#define ISO_MAX_NAME 255u
#define ISO_ROOT_INO 1u
#define ISO_FLAG_DIR 0x02u

struct iso_extent {
    uint32_t lba;
    uint32_t size;
    uint8_t flags;
};

struct iso_fs {
    struct vfs_node *dev;
    uint16_t block_size;
    uint8_t joliet;
    uint8_t rock_ridge;
    struct iso_extent root;
};

struct iso_node {
    struct iso_fs *fs;
    struct iso_extent ex;
    uint32_t ino;
};

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int read_bytes(struct iso_fs *fs, uint64_t off, uint64_t size, void *buf) {
    return block_read(fs->dev, off, size, buf) == size ? 0 : -1;
}

static int read_sector(struct vfs_node *dev, uint32_t lba, void *buf) {
    return block_read(dev, (uint64_t)lba * ISO_SECTOR, ISO_SECTOR, buf) == ISO_SECTOR ? 0 : -1;
}

static int vd_valid(const uint8_t *s) {
    return s[1] == 'C' && s[2] == 'D' && s[3] == '0' && s[4] == '0' && s[5] == '1' && s[6] == 1;
}

static int joliet_desc(const uint8_t *s) {
    return s[0] == 2 && vd_valid(s) && s[88] == 0x25 && s[89] == 0x2f && (s[90] == 0x40 || s[90] == 0x43 || s[90] == 0x45);
}

static void parse_extent(const uint8_t *rec, struct iso_extent *ex) {
    ex->lba = rd32le(rec + 2);
    ex->size = rd32le(rec + 10);
    ex->flags = rec[25];
}

static uint32_t extent_ino(const struct iso_extent *ex) {
    return ex->lba ? ex->lba : ISO_ROOT_INO;
}

static int utf8_put(uint32_t cp, char *out, uint32_t *pos, uint32_t cap) {
    if (*pos + 4 >= cap) return -1;
    if (cp < 0x80) out[(*pos)++] = (char)cp;
    else if (cp < 0x800) {
        out[(*pos)++] = (char)(0xC0 | (cp >> 6));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[(*pos)++] = (char)(0xE0 | (cp >> 12));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    } else return -1;
    return 0;
}

static uint32_t strip_version_len(char *name, uint32_t len) {
    uint32_t out = 0;
    while (out < len && name[out] != ';') out++;
    if (out > 1 && name[out - 1] == '.') out--;
    name[out] = 0;
    return out;
}

static int dot_name(const char *name, uint32_t len) {
    return len == 1 && name[0] == '.';
}

static int dotdot_name(const char *name, uint32_t len) {
    return len == 2 && name[0] == '.' && name[1] == '.';
}

static int name_from_iso(const uint8_t *in, uint8_t len, uint8_t joliet, char *out, uint32_t cap, uint32_t *out_len) {
    if (!len || !out || cap < 2 || !out_len) return -1;
    if (len == 1 && in[0] == 0) { out[0] = '.'; out[1] = 0; *out_len = 1; return 0; }
    if (len == 1 && in[0] == 1) { out[0] = '.'; out[1] = '.'; out[2] = 0; *out_len = 2; return 0; }
    uint32_t pos = 0;
    if (joliet) {
        if (len & 1) return -1;
        for (uint32_t i = 0; i < len; i += 2) {
            uint16_t ch = ((uint16_t)in[i] << 8) | in[i + 1];
            if (!ch) break;
            if (utf8_put(ch, out, &pos, cap) != 0) return -1;
        }
    } else {
        if (len >= cap) len = (uint8_t)(cap - 1);
        memcpy(out, in, len);
        pos = len;
    }
    pos = strip_version_len(out, pos);
    *out_len = pos;
    return pos ? 0 : -1;
}

static uint32_t su_offset(const uint8_t *rec) {
    uint32_t len = rec[32];
    return 33u + len + ((len & 1u) ? 0u : 1u);
}

static int rr_nm_append(const uint8_t *data, uint32_t len, char *out, uint32_t *pos, uint32_t cap) {
    if (len < 5) return -1;
    uint8_t flags = data[4];
    if (flags & 0xF8u) return -1;
    if (flags & 0x02u) {
        if (*pos + 1 >= cap) return -1;
        out[(*pos)++] = '.';
        out[*pos] = 0;
        return 1;
    }
    if (flags & 0x04u) {
        if (*pos + 2 >= cap) return -1;
        out[(*pos)++] = '.';
        out[(*pos)++] = '.';
        out[*pos] = 0;
        return 1;
    }
    uint32_t n = len - 5u;
    if (*pos + n >= cap) return -1;
    memcpy(out + *pos, data + 5, n);
    *pos += n;
    out[*pos] = 0;
    return 1;
}

static int rr_parse_area(struct iso_fs *fs, const uint8_t *area, uint32_t len, char *out, uint32_t *pos, uint32_t cap, uint32_t depth) {
    uint32_t off = 0;
    int found = 0;
    while (off + 4 <= len) {
        uint8_t slen = area[off + 2];
        if (slen < 4 || off + slen > len) break;
        const uint8_t *e = area + off;
        if (e[0] == 'N' && e[1] == 'M') {
            int r = rr_nm_append(e, slen, out, pos, cap);
            if (r < 0) return -1;
            found |= r;
        } else if (e[0] == 'C' && e[1] == 'E' && slen >= 28 && depth < 4) {
            uint32_t lba = rd32le(e + 4);
            uint32_t ce_off = rd32le(e + 12);
            uint32_t ce_len = rd32le(e + 20);
            if (!ce_len || ce_len > 65536u) return -1;
            uint8_t *buf = (uint8_t *)kmalloc(ce_len);
            if (!buf) return -1;
            int rr = read_bytes(fs, (uint64_t)lba * fs->block_size + ce_off, ce_len, buf);
            if (rr == 0) rr = rr_parse_area(fs, buf, ce_len, out, pos, cap, depth + 1);
            kfree(buf);
            if (rr < 0) return -1;
            found |= rr;
        }
        off += slen;
    }
    return found;
}

static int rr_name(struct iso_fs *fs, const uint8_t *rec, uint32_t rec_len, char *out, uint32_t cap, uint32_t *out_len) {
    uint32_t off = su_offset(rec);
    if (off >= rec_len) return 0;
    uint32_t pos = 0;
    out[0] = 0;
    int r = rr_parse_area(fs, rec + off, rec_len - off, out, &pos, cap, 0);
    if (r <= 0 || !pos) return r;
    *out_len = pos;
    return 1;
}

static int rr_has_sp(struct iso_fs *fs, const struct iso_extent *root) {
    uint8_t buf[ISO_SECTOR];
    if (read_bytes(fs, (uint64_t)root->lba * fs->block_size, ISO_SECTOR, buf) != 0) return 0;
    uint8_t len = buf[0];
    if (len < 34) return 0;
    uint32_t off = su_offset(buf);
    while (off + 7 <= len) {
        uint8_t slen = buf[off + 2];
        if (slen < 4 || off + slen > len) break;
        if (buf[off] == 'S' && buf[off + 1] == 'P' && slen >= 7 && buf[off + 4] == 0xBE && buf[off + 5] == 0xEF) return 1;
        off += slen;
    }
    return 0;
}

static int iso_record_name(struct iso_fs *fs, const uint8_t *rec, uint32_t rec_len, char *out, uint32_t cap, uint32_t *out_len) {
    if (fs->rock_ridge) {
        int r = rr_name(fs, rec, rec_len, out, cap, out_len);
        if (r < 0) return -1;
        if (r > 0) return 0;
    }
    return name_from_iso(rec + 33, rec[32], fs->joliet && !fs->rock_ridge, out, cap, out_len);
}

static int parse_vds(struct vfs_node *dev, struct iso_fs *fs, int probe_only) {
    uint8_t *sec = (uint8_t *)kmalloc(ISO_SECTOR);
    if (!sec) return -1;
    uint8_t *best = probe_only ? NULL : (uint8_t *)kmalloc(ISO_SECTOR);
    if (!probe_only && !best) { kfree(sec); return -1; }
    int have_pvd = 0;
    int have_joliet = 0;
    if (best) memset(best, 0, ISO_SECTOR);
    for (uint32_t i = 0; i < 64; i++) {
        if (read_sector(dev, ISO_VD_START + i, sec) != 0) { if (best) kfree(best); kfree(sec); return -1; }
        if (!vd_valid(sec)) { if (best) kfree(best); kfree(sec); return FS_PROBE_NO; }
        if (sec[0] == 1 && !have_pvd) {
            if (probe_only) { kfree(sec); return FS_PROBE_YES; }
            memcpy(best, sec, ISO_SECTOR);
            have_pvd = 1;
        } else if (joliet_desc(sec) && !have_joliet) have_joliet = 1;
        else if (sec[0] == 255) break;
    }
    if (!have_pvd) { kfree(best); kfree(sec); return FS_PROBE_NO; }
    fs->block_size = rd16le(best + 128);
    if (fs->block_size < ISO_SECTOR || (fs->block_size & (fs->block_size - 1u))) { kfree(best); kfree(sec); return -2; }
    parse_extent(best + 156, &fs->root);
    fs->joliet = 0;
    fs->rock_ridge = rr_has_sp(fs, &fs->root) ? 1 : 0;
    if (!fs->rock_ridge && have_joliet) {
        for (uint32_t i = 0; i < 64; i++) {
            if (read_sector(dev, ISO_VD_START + i, sec) != 0) { kfree(best); kfree(sec); return -1; }
            if (joliet_desc(sec)) { memcpy(best, sec, ISO_SECTOR); fs->joliet = 1; break; }
            if (sec[0] == 255) break;
        }
        fs->block_size = rd16le(best + 128);
        parse_extent(best + 156, &fs->root);
    }
    kfree(best);
    kfree(sec);
    return 0;
}

static int iso_probe(struct vfs_node *dev) {
    struct iso_fs fs;
    memset(&fs, 0, sizeof(fs));
    fs.dev = dev;
    fs.block_size = ISO_SECTOR;
    return parse_vds(dev, &fs, 1);
}

static void attach_ops(struct vfs_node *vnode, uint32_t type);

static struct vfs_node *make_vnode(struct iso_fs *fs, const struct iso_extent *ex, const char *name) {
    struct iso_node *n = (struct iso_node *)kmalloc(sizeof(*n));
    if (!n) return 0;
    memset(n, 0, sizeof(*n));
    n->fs = fs;
    n->ex = *ex;
    n->ino = extent_ino(ex);
    uint32_t type = (ex->flags & ISO_FLAG_DIR) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    struct vfs_node *v = vfs_create_fs_node(name, type, n->ino, type == VFS_NODE_FILE ? ex->size : 0, n);
    if (!v) { kfree(n); return 0; }
    attach_ops(v, type);
    return v;
}

static int scan_dir(struct iso_node *dir, const char *want, uint32_t want_len, uint64_t index,
                    struct iso_extent *ex, char name[ISO_MAX_NAME + 1], uint32_t *name_len) {
    if (!dir || !(dir->ex.flags & ISO_FLAG_DIR) || !ex || !name || !name_len) return -1;
    struct iso_fs *fs = dir->fs;
    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    if (!buf) return -1;
    uint64_t seen = 0;
    uint64_t done = 0;
    uint64_t base = (uint64_t)dir->ex.lba * fs->block_size;
    uint64_t block_mask = (uint64_t)fs->block_size - 1u;
    while (done < dir->ex.size) {
        uint64_t take = fs->block_size;
        if (take > dir->ex.size - done) take = dir->ex.size - done;
        if (read_bytes(fs, base + done, take, buf) != 0) { kfree(buf); return -1; }
        uint32_t off = 0;
        while (off < take) {
            uint8_t len = buf[off];
            if (!len) { off = (uint32_t)((off + fs->block_size) & ~block_mask); break; }
            if (off + len > take || len < 34) { kfree(buf); return -1; }
            const uint8_t *rec = buf + off;
            char nm[ISO_MAX_NAME + 1];
            uint32_t nm_len = 0;
            if (iso_record_name(fs, rec, len, nm, sizeof(nm), &nm_len) == 0) {
                if (!dot_name(nm, nm_len) && !dotdot_name(nm, nm_len)) {
                    int match = want ? (nm_len == want_len && memcmp(nm, want, nm_len) == 0) : (seen == index);
                    if (match) {
                        parse_extent(rec, ex);
                        memcpy(name, nm, nm_len + 1);
                        *name_len = nm_len;
                        kfree(buf);
                        return 0;
                    }
                    seen++;
                }
            }
            off += len;
        }
        done += fs->block_size;
    }
    kfree(buf);
    return -1;
}

static struct vfs_node *iso_finddir(struct vfs_node *parent, const char *name) {
    struct iso_node *dir = (struct iso_node *)vfs_get_fs_data(parent);
    if (!dir || !name) return 0;
    uint32_t name_len = 0;
    while (name[name_len]) {
        if (name_len >= ISO_MAX_NAME) return 0;
        name_len++;
    }
    struct iso_extent ex;
    char nm[ISO_MAX_NAME + 1];
    uint32_t nm_len = 0;
    if (scan_dir(dir, name, name_len, 0, &ex, nm, &nm_len) != 0) return 0;
    struct vfs_node *v = make_vnode(dir->fs, &ex, nm);
    if (v) vfs_add_child(parent, v);
    return v;
}

static int iso_readdir(struct vfs_node *parent, uint64_t index, struct vfs_dirent *out) {
    struct iso_node *dir = (struct iso_node *)vfs_get_fs_data(parent);
    if (!dir || !out) return -1;
    struct iso_extent ex;
    char nm[ISO_MAX_NAME + 1];
    uint32_t nm_len = 0;
    if (scan_dir(dir, 0, 0, index, &ex, nm, &nm_len) != 0) return -1;
    char *dst = out->name;
    uint64_t cap = out->name_capacity;
    uint64_t len = nm_len;
    memset(out, 0, sizeof(*out));
    out->inode = extent_ino(&ex);
    out->type = (ex.flags & ISO_FLAG_DIR) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    out->name_len = len;
    out->name = dst;
    out->name_capacity = cap;
    if (dst && cap > len) memcpy(dst, nm, len + 1);
    return 0;
}

static uint64_t iso_read(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf) {
    struct iso_node *n = (struct iso_node *)vfs_get_fs_data(vnode);
    if (!n || !buf || (n->ex.flags & ISO_FLAG_DIR) || off >= n->ex.size) return 0;
    if (size > n->ex.size - off) size = n->ex.size - off;
    if (read_bytes(n->fs, (uint64_t)n->ex.lba * n->fs->block_size + off, size, buf) != 0) return 0;
    return size;
}

static uint64_t iso_write(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf) {
    (void)vnode; (void)off; (void)size; (void)buf;
    return 0;
}

static int iso_truncate(struct vfs_node *vnode, uint64_t size) { (void)vnode; (void)size; return -1; }
static int iso_create(struct vfs_node *parent, const char *name, uint32_t type, struct vfs_node **out) { (void)parent; (void)name; (void)type; (void)out; return -1; }
static int iso_unlink(struct vfs_node *parent, const char *name) { (void)parent; (void)name; return -1; }
static int iso_rename(struct vfs_node *op, const char *on, struct vfs_node *np, const char *nn) { (void)op; (void)on; (void)np; (void)nn; return -1; }

static void attach_ops(struct vfs_node *vnode, uint32_t type) {
    if (type == VFS_NODE_DIRECTORY) {
        vfs_set_finddir(vnode, iso_finddir);
        vfs_set_readdir(vnode, iso_readdir);
        vfs_set_create(vnode, iso_create);
        vfs_set_unlink(vnode, iso_unlink);
        vfs_set_rename(vnode, iso_rename);
    } else {
        vfs_set_read(vnode, iso_read);
        vfs_set_write(vnode, iso_write);
        vfs_set_truncate(vnode, iso_truncate);
    }
}

static int iso_mount(struct vfs_node *dev, struct vfs_node *mountpoint, const char *flags) {
    (void)flags;
    if (!dev || !mountpoint) return -1;
    struct iso_fs *fs = (struct iso_fs *)kmalloc(sizeof(*fs));
    if (!fs) return -1;
    memset(fs, 0, sizeof(*fs));
    fs->dev = dev;
    fs->block_size = ISO_SECTOR;
    int r = parse_vds(dev, fs, 0);
    if (r != 0) { kfree(fs); return r; }
    struct iso_node *root = (struct iso_node *)kmalloc(sizeof(*root));
    if (!root) { kfree(fs); return -1; }
    memset(root, 0, sizeof(*root));
    root->fs = fs;
    root->ex = fs->root;
    root->ino = ISO_ROOT_INO;
    vfs_set_fs_data(mountpoint, root);
    vfs_set_size(mountpoint, 0);
    attach_ops(mountpoint, VFS_NODE_DIRECTORY);
    klog(LOG_INFO, "iso9660: mounted %s%s\n", fs->rock_ridge ? "rockridge" : "iso9660", fs->joliet ? "+joliet" : "");
    return 0;
}

static int iso_unmount(struct vfs_node *mountpoint) {
    struct iso_node *root = (struct iso_node *)vfs_get_fs_data(mountpoint);
    if (!root) return -1;
    if (root->fs) kfree(root->fs);
    kfree(root);
    return 0;
}

static struct fs_driver iso_driver = { .name = "iso9660", .mount = iso_mount, .next = 0, .probe = iso_probe, .unmount = iso_unmount, .flags = 0 };
static int iso_init(void) { return fs_register(&iso_driver); }
MODULE_INFO("iso9660", iso_init, 0, 0, "fs");
