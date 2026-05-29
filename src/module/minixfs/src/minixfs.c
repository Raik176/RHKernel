#include "mod/fs.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "string.h"

#define MINIX_SUPER_OFF 1024u
#define MINIX_V1_MAGIC 0x137Fu
#define MINIX_V1_MAGIC_30 0x138Fu
#define MINIX_V2_MAGIC 0x2468u
#define MINIX_V2_MAGIC_30 0x2478u
#define MINIX_V3_MAGIC 0x4D5Au
#define MINIX_ROOT_INO 1u
#define MINIX_NAME_MAX 60u
#define MINIX_MAX_BLOCK 4096u
#define MINIX_MAX_SYMLINK 4096u

#define S_IFMT 00170000u
#define S_IFDIR 0040000u
#define S_IFCHR 0020000u
#define S_IFBLK 0060000u
#define S_IFREG 0100000u
#define S_IFLNK 0120000u

struct minix_mount {
    struct vfs_node *dev;
    uint32_t version;
    uint32_t swapped;
    uint32_t block_size;
    uint32_t zone_shift;
    uint32_t ninodes;
    uint32_t nzones;
    uint32_t imap_blocks;
    uint32_t zmap_blocks;
    uint32_t firstdatazone;
    uint32_t inode_size;
    uint32_t dirent_size;
    uint32_t name_len;
    uint64_t inode_table_off;
    uint64_t imap_off;
    uint64_t zmap_off;
};

struct minix_inode {
    struct minix_mount *mnt;
    uint32_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t links;
    uint64_t size;
    uint32_t zone[10];
};

static uint16_t r16e(const void *p, uint32_t s) {
    const uint8_t *b = (const uint8_t *)p;
    return s ? ((uint16_t)b[1] | ((uint16_t)b[0] << 8)) : ((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t r32e(const void *p, uint32_t s) {
    const uint8_t *b = (const uint8_t *)p;
    return s ? ((uint32_t)b[3] | ((uint32_t)b[2] << 8) | ((uint32_t)b[1] << 16) | ((uint32_t)b[0] << 24))
             : ((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

static void w16e(void *p, uint16_t v, uint32_t s) {
    uint8_t *b = (uint8_t *)p;
    if (s) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
    else { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
}

static void w32e(void *p, uint32_t v, uint32_t s) {
    uint8_t *b = (uint8_t *)p;
    if (s) { b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }
    else { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
}

static uint64_t minu64(uint64_t a, uint64_t b) { return a < b ? a : b; }

static int read_exact(struct minix_mount *mnt, uint64_t off, uint64_t size, void *buf) {
    return block_read(mnt->dev, off, size, buf) == size ? 0 : -1;
}

static int write_exact(struct minix_mount *mnt, uint64_t off, uint64_t size, const void *buf) {
    return block_write(mnt->dev, off, size, (void *)buf) == size ? 0 : -1;
}

static uint64_t zone_size(struct minix_mount *mnt) { return (uint64_t)mnt->block_size << mnt->zone_shift; }
static uint64_t zone_off(struct minix_mount *mnt, uint32_t zone) { return (uint64_t)zone * zone_size(mnt); }
static uint32_t ptr_size(struct minix_mount *mnt) { return mnt->version == 1 ? 2u : 4u; }
static uint32_t ptrs_per_block(struct minix_mount *mnt) { return mnt->block_size / ptr_size(mnt); }

static int name_len_bounded(struct minix_mount *mnt, const char *name, uint64_t *len_out) {
    if (!mnt || !name || !*name || !len_out) return 0;
    uint64_t len = 0;
    while (name[len]) {
        if (len == mnt->name_len || name[len] == '/') return 0;
        len++;
    }
    *len_out = len;
    return 1;
}

static int name_len_valid(struct minix_mount *mnt, const char *name, uint64_t *len_out) {
    if (!name_len_bounded(mnt, name, len_out)) return 0;
    return !((*len_out == 1 && name[0] == '.') || (*len_out == 2 && name[0] == '.' && name[1] == '.'));
}

static int name_valid(struct minix_mount *mnt, const char *name) {
    uint64_t len;
    return name_len_valid(mnt, name, &len);
}

static uint32_t magic_host(uint16_t v) { return ((uint32_t)(v & 0xffu) << 8) | (uint32_t)(v >> 8); }

static int parse_super(struct vfs_node *dev, struct minix_mount *mnt) {
    uint8_t buf[64];
    memset(mnt, 0, sizeof(*mnt));
    mnt->dev = dev;
    if (!dev || block_read(dev, MINIX_SUPER_OFF, sizeof(buf), buf) != sizeof(buf)) return -1;

    uint16_t raw = r16e(buf + 16, 0);
    uint32_t sw = 0;
    uint16_t magic = raw;
    if (magic_host(raw) == MINIX_V1_MAGIC || magic_host(raw) == MINIX_V1_MAGIC_30 ||
        magic_host(raw) == MINIX_V2_MAGIC || magic_host(raw) == MINIX_V2_MAGIC_30) {
        sw = 1;
        magic = (uint16_t)magic_host(raw);
    }

    if (magic == MINIX_V1_MAGIC || magic == MINIX_V1_MAGIC_30 || magic == MINIX_V2_MAGIC || magic == MINIX_V2_MAGIC_30) {
        mnt->swapped = sw;
        mnt->version = (magic == MINIX_V1_MAGIC || magic == MINIX_V1_MAGIC_30) ? 1u : 2u;
        mnt->block_size = 1024u;
        mnt->ninodes = r16e(buf + 0, sw);
        uint16_t zones16 = r16e(buf + 2, sw);
        mnt->imap_blocks = r16e(buf + 4, sw);
        mnt->zmap_blocks = r16e(buf + 6, sw);
        mnt->firstdatazone = r16e(buf + 8, sw);
        mnt->zone_shift = r16e(buf + 10, sw);
        mnt->nzones = (mnt->version == 1) ? zones16 : r32e(buf + 20, sw);
        mnt->inode_size = (mnt->version == 1) ? 32u : 64u;
        mnt->dirent_size = (magic == MINIX_V1_MAGIC || magic == MINIX_V2_MAGIC) ? 16u : 32u;
        mnt->name_len = mnt->dirent_size - 2u;
    } else {
        raw = r16e(buf + 24, 0);
        sw = 0;
        magic = raw;
        if (magic_host(raw) == MINIX_V3_MAGIC) { sw = 1; magic = (uint16_t)magic_host(raw); }
        if (magic != MINIX_V3_MAGIC) return 0;
        mnt->swapped = sw;
        mnt->version = 3u;
        mnt->ninodes = r32e(buf + 0, sw);
        mnt->imap_blocks = r16e(buf + 6, sw);
        mnt->zmap_blocks = r16e(buf + 8, sw);
        mnt->firstdatazone = r16e(buf + 10, sw);
        mnt->zone_shift = r16e(buf + 12, sw);
        mnt->nzones = r32e(buf + 20, sw);
        mnt->block_size = r16e(buf + 28, sw);
        if (!mnt->block_size) mnt->block_size = 1024u;
        mnt->inode_size = 64u;
        mnt->dirent_size = 64u;
        mnt->name_len = 60u;
    }

    if (!mnt->ninodes || !mnt->nzones || !mnt->imap_blocks || !mnt->zmap_blocks) return -1;
    if (mnt->block_size < 1024u || mnt->block_size > MINIX_MAX_BLOCK) return -1;
    if (mnt->zone_shift > 8u) return -1;
    if (mnt->firstdatazone >= mnt->nzones) return -1;
    mnt->imap_off = 2ull * mnt->block_size;
    mnt->zmap_off = mnt->imap_off + (uint64_t)mnt->imap_blocks * mnt->block_size;
    mnt->inode_table_off = mnt->zmap_off + (uint64_t)mnt->zmap_blocks * mnt->block_size;
    return 1;
}

static int read_inode(struct minix_mount *mnt, uint32_t ino, struct minix_inode *out) {
    if (!mnt || !out || ino == 0 || ino > mnt->ninodes) return -1;
    uint8_t raw[64];
    uint64_t off = mnt->inode_table_off + (uint64_t)(ino - 1u) * mnt->inode_size;
    if (read_exact(mnt, off, mnt->inode_size, raw) != 0) return -1;
    memset(out, 0, sizeof(*out));
    out->mnt = mnt;
    out->ino = ino;
    if (mnt->version == 1) {
        out->mode = r16e(raw + 0, mnt->swapped);
        out->uid = r16e(raw + 2, mnt->swapped);
        out->size = r32e(raw + 4, mnt->swapped);
        out->gid = raw[12];
        out->links = raw[13];
        for (uint32_t i = 0; i < 9; i++) out->zone[i] = r16e(raw + 14 + i * 2, mnt->swapped);
    } else {
        out->mode = r16e(raw + 0, mnt->swapped);
        out->links = r16e(raw + 2, mnt->swapped);
        out->uid = r16e(raw + 4, mnt->swapped);
        out->gid = r16e(raw + 6, mnt->swapped);
        out->size = r32e(raw + 8, mnt->swapped);
        for (uint32_t i = 0; i < 10; i++) out->zone[i] = r32e(raw + 24 + i * 4, mnt->swapped);
    }
    return 0;
}

static int write_inode(struct minix_inode *ino) {
    if (!ino || !ino->mnt || ino->ino == 0 || ino->ino > ino->mnt->ninodes) return -1;
    struct minix_mount *mnt = ino->mnt;
    uint8_t raw[64];
    memset(raw, 0, sizeof(raw));
    if (mnt->version == 1) {
        if (ino->size > 0xffffffffull) return -1;
        w16e(raw + 0, (uint16_t)ino->mode, mnt->swapped);
        w16e(raw + 2, (uint16_t)ino->uid, mnt->swapped);
        w32e(raw + 4, (uint32_t)ino->size, mnt->swapped);
        raw[12] = (uint8_t)ino->gid;
        raw[13] = (uint8_t)ino->links;
        for (uint32_t i = 0; i < 9; i++) w16e(raw + 14 + i * 2, (uint16_t)ino->zone[i], mnt->swapped);
    } else {
        if (ino->size > 0xffffffffull) return -1;
        w16e(raw + 0, (uint16_t)ino->mode, mnt->swapped);
        w16e(raw + 2, (uint16_t)ino->links, mnt->swapped);
        w16e(raw + 4, (uint16_t)ino->uid, mnt->swapped);
        w16e(raw + 6, (uint16_t)ino->gid, mnt->swapped);
        w32e(raw + 8, (uint32_t)ino->size, mnt->swapped);
        for (uint32_t i = 0; i < 10; i++) w32e(raw + 24 + i * 4, ino->zone[i], mnt->swapped);
    }
    return write_exact(mnt, mnt->inode_table_off + (uint64_t)(ino->ino - 1u) * mnt->inode_size, mnt->inode_size, raw);
}

static int bitmap_get(struct minix_mount *mnt, uint64_t off, uint32_t bit) {
    uint8_t v;
    if (read_exact(mnt, off + bit / 8u, 1, &v) != 0) return -1;
    return (v >> (bit & 7u)) & 1u;
}

static int bitmap_set(struct minix_mount *mnt, uint64_t off, uint32_t bit, int val) {
    uint8_t v;
    if (read_exact(mnt, off + bit / 8u, 1, &v) != 0) return -1;
    if (val) v |= (uint8_t)(1u << (bit & 7u)); else v &= (uint8_t)~(1u << (bit & 7u));
    return write_exact(mnt, off + bit / 8u, 1, &v);
}

static int zero_zone(struct minix_mount *mnt, uint32_t zone) {
    uint8_t *buf = (uint8_t *)kmalloc(mnt->block_size);
    if (!buf) return -1;
    memset(buf, 0, mnt->block_size);
    uint64_t zs = zone_size(mnt);
    for (uint64_t off = 0; off < zs; off += mnt->block_size) {
        if (write_exact(mnt, zone_off(mnt, zone) + off, mnt->block_size, buf) != 0) { kfree(buf); return -1; }
    }
    kfree(buf);
    return 0;
}

static uint32_t alloc_inode_num(struct minix_mount *mnt) {
    for (uint32_t ino = 1; ino <= mnt->ninodes; ino++) {
        int r = bitmap_get(mnt, mnt->imap_off, ino);
        if (r == 0 && bitmap_set(mnt, mnt->imap_off, ino, 1) == 0) return ino;
        if (r < 0) return 0;
    }
    return 0;
}

static void free_inode_num(struct minix_mount *mnt, uint32_t ino) {
    if (ino && ino <= mnt->ninodes) bitmap_set(mnt, mnt->imap_off, ino, 0);
}

static uint32_t alloc_zone(struct minix_mount *mnt) {
    uint32_t count = mnt->nzones - mnt->firstdatazone;
    for (uint32_t bit = 0; bit < count; bit++) {
        int r = bitmap_get(mnt, mnt->zmap_off, bit);
        if (r == 0) {
            uint32_t zone = mnt->firstdatazone + bit;
            if (bitmap_set(mnt, mnt->zmap_off, bit, 1) == 0 && zero_zone(mnt, zone) == 0) return zone;
            return 0;
        }
        if (r < 0) return 0;
    }
    return 0;
}

static void free_zone(struct minix_mount *mnt, uint32_t zone) {
    if (zone >= mnt->firstdatazone && zone < mnt->nzones) bitmap_set(mnt, mnt->zmap_off, zone - mnt->firstdatazone, 0);
}

static uint32_t read_ptr(struct minix_mount *mnt, uint32_t zone, uint32_t index) {
    if (!zone) return 0;
    uint8_t raw[4];
    uint32_t ps = ptr_size(mnt);
    if (read_exact(mnt, zone_off(mnt, zone) + (uint64_t)index * ps, ps, raw) != 0) return 0;
    return ps == 2u ? r16e(raw, mnt->swapped) : r32e(raw, mnt->swapped);
}

static int write_ptr(struct minix_mount *mnt, uint32_t zone, uint32_t index, uint32_t val) {
    if (!zone) return -1;
    uint8_t raw[4];
    uint32_t ps = ptr_size(mnt);
    if (ps == 2u) {
        if (val > 0xffffu) return -1;
        w16e(raw, (uint16_t)val, mnt->swapped);
    } else w32e(raw, val, mnt->swapped);
    return write_exact(mnt, zone_off(mnt, zone) + (uint64_t)index * ps, ps, raw);
}

static uint32_t data_zone_for(struct minix_inode *ino, uint64_t file_block) {
    struct minix_mount *mnt = ino->mnt;
    uint32_t ppb = ptrs_per_block(mnt);
    if (file_block < 7u) return ino->zone[file_block];
    file_block -= 7u;
    if (file_block < ppb) return read_ptr(mnt, ino->zone[7], (uint32_t)file_block);
    file_block -= ppb;
    if (file_block < (uint64_t)ppb * ppb) {
        uint32_t outer = read_ptr(mnt, ino->zone[8], (uint32_t)(file_block / ppb));
        return read_ptr(mnt, outer, (uint32_t)(file_block % ppb));
    }
    if (mnt->version > 1) {
        file_block -= (uint64_t)ppb * ppb;
        uint32_t l2 = read_ptr(mnt, ino->zone[9], (uint32_t)(file_block / ((uint64_t)ppb * ppb)));
        uint32_t rem = (uint32_t)(file_block % ((uint64_t)ppb * ppb));
        uint32_t l1 = read_ptr(mnt, l2, rem / ppb);
        return read_ptr(mnt, l1, rem % ppb);
    }
    return 0;
}

static int get_or_alloc_zone(struct minix_inode *ino, uint64_t file_block, uint32_t *out) {
    struct minix_mount *mnt = ino->mnt;
    uint32_t ppb = ptrs_per_block(mnt);
    if (file_block < 7u) {
        if (!ino->zone[file_block]) { ino->zone[file_block] = alloc_zone(mnt); if (!ino->zone[file_block]) return -1; }
        *out = ino->zone[file_block]; return 0;
    }
    file_block -= 7u;
    if (file_block < ppb) {
        if (!ino->zone[7]) { ino->zone[7] = alloc_zone(mnt); if (!ino->zone[7]) return -1; }
        uint32_t z = read_ptr(mnt, ino->zone[7], (uint32_t)file_block);
        if (!z) { z = alloc_zone(mnt); if (!z || write_ptr(mnt, ino->zone[7], (uint32_t)file_block, z) != 0) return -1; }
        *out = z; return 0;
    }
    file_block -= ppb;
    if (file_block < (uint64_t)ppb * ppb) {
        if (!ino->zone[8]) { ino->zone[8] = alloc_zone(mnt); if (!ino->zone[8]) return -1; }
        uint32_t oi = (uint32_t)(file_block / ppb), ii = (uint32_t)(file_block % ppb);
        uint32_t outer = read_ptr(mnt, ino->zone[8], oi);
        if (!outer) { outer = alloc_zone(mnt); if (!outer || write_ptr(mnt, ino->zone[8], oi, outer) != 0) return -1; }
        uint32_t z = read_ptr(mnt, outer, ii);
        if (!z) { z = alloc_zone(mnt); if (!z || write_ptr(mnt, outer, ii, z) != 0) return -1; }
        *out = z; return 0;
    }
    if (mnt->version <= 1) return -1;
    file_block -= (uint64_t)ppb * ppb;
    uint32_t a = (uint32_t)(file_block / ((uint64_t)ppb * ppb));
    uint32_t rem = (uint32_t)(file_block % ((uint64_t)ppb * ppb));
    uint32_t b = rem / ppb, c = rem % ppb;
    if (!ino->zone[9]) { ino->zone[9] = alloc_zone(mnt); if (!ino->zone[9]) return -1; }
    uint32_t l2 = read_ptr(mnt, ino->zone[9], a);
    if (!l2) { l2 = alloc_zone(mnt); if (!l2 || write_ptr(mnt, ino->zone[9], a, l2) != 0) return -1; }
    uint32_t l1 = read_ptr(mnt, l2, b);
    if (!l1) { l1 = alloc_zone(mnt); if (!l1 || write_ptr(mnt, l2, b, l1) != 0) return -1; }
    uint32_t z = read_ptr(mnt, l1, c);
    if (!z) { z = alloc_zone(mnt); if (!z || write_ptr(mnt, l1, c, z) != 0) return -1; }
    *out = z; return 0;
}

static void free_indirect(struct minix_mount *mnt, uint32_t zone, uint32_t depth) {
    if (!zone) return;
    if (depth) {
        uint32_t ppb = ptrs_per_block(mnt);
        for (uint32_t i = 0; i < ppb; i++) free_indirect(mnt, read_ptr(mnt, zone, i), depth - 1u);
    }
    free_zone(mnt, zone);
}

static void free_inode_data(struct minix_inode *ino) {
    for (uint32_t i = 0; i < 7; i++) { if (ino->zone[i]) free_zone(ino->mnt, ino->zone[i]); ino->zone[i] = 0; }
    free_indirect(ino->mnt, ino->zone[7], 1); ino->zone[7] = 0;
    free_indirect(ino->mnt, ino->zone[8], 2); ino->zone[8] = 0;
    if (ino->mnt->version > 1) { free_indirect(ino->mnt, ino->zone[9], 3); ino->zone[9] = 0; }
}

static uint64_t minix_read(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    struct minix_inode *ino = (struct minix_inode *)vfs_get_fs_data(node);
    if (!ino || !buffer || offset >= ino->size) return 0;
    size = minu64(size, ino->size - offset);
    uint64_t done = 0, zs = zone_size(ino->mnt);
    while (done < size) {
        uint64_t pos = offset + done;
        uint64_t in_zone = pos % zs;
        uint64_t chunk = minu64(size - done, zs - in_zone);
        uint32_t zone = data_zone_for(ino, pos / zs);
        if (!zone) memset(buffer + done, 0, chunk);
        else if (read_exact(ino->mnt, zone_off(ino->mnt, zone) + in_zone, chunk, buffer + done) != 0) break;
        done += chunk;
    }
    return done;
}

static uint64_t minix_write_inode_data(struct minix_inode *ino, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!ino || !buffer || ((ino->mode & S_IFMT) != S_IFREG && (ino->mode & S_IFMT) != S_IFLNK)) return 0;
    uint64_t done = 0, zs = zone_size(ino->mnt);
    while (done < size) {
        uint64_t pos = offset + done;
        uint64_t in_zone = pos % zs;
        uint64_t chunk = minu64(size - done, zs - in_zone);
        uint32_t zone = 0;
        if (get_or_alloc_zone(ino, pos / zs, &zone) != 0) break;
        if (write_exact(ino->mnt, zone_off(ino->mnt, zone) + in_zone, chunk, buffer + done) != 0) break;
        done += chunk;
    }
    if (done && offset + done > ino->size) ino->size = offset + done;
    write_inode(ino);
    return done;
}

static uint64_t minix_write(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    struct minix_inode *ino = (struct minix_inode *)vfs_get_fs_data(node);
    uint64_t done = minix_write_inode_data(ino, offset, size, buffer);
    if (ino && done && offset + done >= ino->size) vfs_set_size(node, ino->size);
    return done;
}

static uint32_t vfs_type_for_mode(uint32_t mode) {
    switch (mode & S_IFMT) {
        case S_IFDIR: return VFS_NODE_DIRECTORY;
        case S_IFCHR: return VFS_NODE_CHAR_DEVICE;
        case S_IFBLK: return VFS_NODE_BLOCK_DEVICE;
        default: return VFS_NODE_FILE;
    }
}

static struct vfs_node *minix_finddir(struct vfs_node *parent, const char *name);
static int minix_readdir(struct vfs_node *node, uint64_t index, struct vfs_dirent *out);
static int minix_create(struct vfs_node *parent, const char *name, uint32_t type, struct vfs_node **out);
static int minix_unlink(struct vfs_node *parent, const char *name);
static int minix_rename(struct vfs_node *old_parent, const char *old_name, struct vfs_node *new_parent, const char *new_name);
static int minix_truncate(struct vfs_node *node, uint64_t size);

static void attach_ops(struct vfs_node *node, uint32_t type) {
    if (type == VFS_NODE_DIRECTORY) {
        vfs_set_finddir(node, minix_finddir);
        vfs_set_readdir(node, minix_readdir);
        vfs_set_create(node, minix_create);
        vfs_set_unlink(node, minix_unlink);
        vfs_set_rename(node, minix_rename);
    } else if (type == VFS_NODE_FILE) {
        vfs_set_read(node, minix_read);
        vfs_set_write(node, minix_write);
        vfs_set_truncate(node, minix_truncate);
    }
}

static struct vfs_node *make_node(struct minix_mount *mnt, uint32_t ino_num, const char *name) {
    struct minix_inode disk;
    if (read_inode(mnt, ino_num, &disk) != 0 || disk.mode == 0) return 0;
    struct minix_inode *ino = (struct minix_inode *)kmalloc(sizeof(*ino));
    if (!ino) return 0;
    *ino = disk;
    uint32_t type = vfs_type_for_mode(ino->mode);
    struct vfs_node *node = vfs_create_fs_node(name, type, ino_num, ino->size, ino);
    if (!node) { kfree(ino); return 0; }
    attach_ops(node, type);
    return node;
}

static int read_dirent_at(struct minix_inode *dir, uint64_t index, uint32_t *ino_out, char *name_out, uint64_t name_cap, uint64_t *len_out) {
    uint8_t raw[64];
    uint64_t off = index * dir->mnt->dirent_size;
    if (off + dir->mnt->dirent_size > dir->size) return -1;
    uint64_t got = 0, zs = zone_size(dir->mnt);
    while (got < dir->mnt->dirent_size) {
        uint64_t pos = off + got;
        uint64_t in_zone = pos % zs;
        uint64_t chunk = minu64(dir->mnt->dirent_size - got, zs - in_zone);
        uint32_t zone = data_zone_for(dir, pos / zs);
        if (!zone) memset(raw + got, 0, chunk);
        else if (read_exact(dir->mnt, zone_off(dir->mnt, zone) + in_zone, chunk, raw + got) != 0) return -1;
        got += chunk;
    }
    uint32_t ino = dir->mnt->version == 3 ? r32e(raw, dir->mnt->swapped) : r16e(raw, dir->mnt->swapped);
    const char *nm = (const char *)(raw + (dir->mnt->version == 3 ? 4u : 2u));
    uint64_t len = 0;
    while (len < dir->mnt->name_len && nm[len]) len++;
    if (ino_out) *ino_out = ino;
    if (len_out) *len_out = len;
    if (name_out && name_cap) {
        uint64_t c = len;
        if (c >= name_cap) c = name_cap - 1;
        memcpy(name_out, nm, c);
        name_out[c] = 0;
    }
    return ino ? 0 : 1;
}

static int write_dirent_at(struct minix_inode *dir, uint64_t index, uint32_t ino, const char *name) {
    uint8_t raw[64];
    memset(raw, 0, sizeof(raw));
    if (dir->mnt->version == 3) w32e(raw, ino, dir->mnt->swapped); else w16e(raw, (uint16_t)ino, dir->mnt->swapped);
    if (name) {
        uint64_t len;
        if (!name_len_bounded(dir->mnt, name, &len)) return -1;
        memcpy(raw + (dir->mnt->version == 3 ? 4u : 2u), name, len);
    }
    uint64_t off = index * dir->mnt->dirent_size;
    uint64_t got = 0, zs = zone_size(dir->mnt);
    while (got < dir->mnt->dirent_size) {
        uint64_t pos = off + got;
        uint64_t in_zone = pos % zs;
        uint64_t chunk = minu64(dir->mnt->dirent_size - got, zs - in_zone);
        uint32_t zone = 0;
        if (get_or_alloc_zone(dir, pos / zs, &zone) != 0) return -1;
        if (write_exact(dir->mnt, zone_off(dir->mnt, zone) + in_zone, chunk, raw + got) != 0) return -1;
        got += chunk;
    }
    if (off + dir->mnt->dirent_size > dir->size) dir->size = off + dir->mnt->dirent_size;
    return write_inode(dir);
}

static int dir_find_entry(struct minix_inode *dir, const char *name, uint64_t *idx_out, uint32_t *ino_out) {
    if (!dir || !name || ((dir->mode & S_IFMT) != S_IFDIR)) return -1;
    uint64_t name_len;
    if (!name_len_valid(dir->mnt, name, &name_len)) return -1;
    uint64_t entries = dir->size / dir->mnt->dirent_size;
    char tmp[MINIX_NAME_MAX + 1];
    for (uint64_t i = 0; i < entries; i++) {
        uint32_t ino = 0;
        uint64_t len = 0;
        int r = read_dirent_at(dir, i, &ino, tmp, sizeof(tmp), &len);
        if (r < 0) return -1;
        if (r > 0 || !ino || len != name_len) continue;
        if (memcmp(tmp, name, name_len) == 0) { if (idx_out) *idx_out = i; if (ino_out) *ino_out = ino; return 0; }
    }
    return -1;
}

static int dir_add_entry(struct minix_inode *dir, const char *name, uint32_t ino) {
    if (!name_valid(dir->mnt, name)) return -1;
    if (dir_find_entry(dir, name, 0, 0) == 0) return -1;
    uint64_t entries = dir->size / dir->mnt->dirent_size;
    for (uint64_t i = 0; i < entries; i++) {
        uint32_t old = 0;
        int r = read_dirent_at(dir, i, &old, 0, 0, 0);
        if (r > 0 || old == 0) return write_dirent_at(dir, i, ino, name);
        if (r < 0) return -1;
    }
    return write_dirent_at(dir, entries, ino, name);
}

static struct vfs_node *minix_finddir(struct vfs_node *parent, const char *name) {
    struct minix_inode *dir = (struct minix_inode *)vfs_get_fs_data(parent);
    uint32_t ino = 0;
    if (!dir || dir_find_entry(dir, name, 0, &ino) != 0) return 0;
    return make_node(dir->mnt, ino, name);
}

static int minix_readdir(struct vfs_node *node, uint64_t index, struct vfs_dirent *out) {
    struct minix_inode *dir = (struct minix_inode *)vfs_get_fs_data(node);
    if (!dir || !out || ((dir->mode & S_IFMT) != S_IFDIR)) return -1;
    uint64_t entries = dir->size / dir->mnt->dirent_size, seen = 0;
    char tmp[MINIX_NAME_MAX + 1];
    for (uint64_t i = 0; i < entries; i++) {
        uint32_t ino_num = 0; uint64_t len = 0;
        int r = read_dirent_at(dir, i, &ino_num, tmp, sizeof(tmp), &len);
        if (r < 0) return -1;
        if (r > 0 || !ino_num || (len == 1 && tmp[0] == '.') || (len == 2 && tmp[0] == '.' && tmp[1] == '.')) continue;
        if (seen++ != index) continue;
        struct minix_inode child;
        uint32_t type = VFS_NODE_FILE;
        if (read_inode(dir->mnt, ino_num, &child) == 0) type = vfs_type_for_mode(child.mode);
        char *dst = out->name; uint64_t cap = out->name_capacity;
        memset(out, 0, sizeof(*out));
        out->inode = ino_num; out->type = type; out->name_len = len; out->name = dst; out->name_capacity = cap;
        if (dst && cap > len) memcpy(dst, tmp, len + 1);
        return 0;
    }
    return -1;
}

static int init_dir(struct minix_inode *dir, uint32_t self, uint32_t parent) {
    if (write_dirent_at(dir, 0, self, ".") != 0) return -1;
    if (write_dirent_at(dir, 1, parent, "..") != 0) return -1;
    dir->links = 2;
    return write_inode(dir);
}

static int minix_create_raw(struct minix_inode *parent, const char *name, uint32_t mode, const uint8_t *data, uint64_t len, struct vfs_node **out) {
    if (!parent || !name_valid(parent->mnt, name)) return -1;
    uint32_t ino_num = alloc_inode_num(parent->mnt);
    if (!ino_num) return -1;
    struct minix_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.mnt = parent->mnt; ino.ino = ino_num; ino.mode = mode; ino.links = 1;
    if ((mode & S_IFMT) == S_IFDIR) ino.links = 2;
    if (write_inode(&ino) != 0) { free_inode_num(parent->mnt, ino_num); return -1; }
    if ((mode & S_IFMT) == S_IFDIR && init_dir(&ino, ino_num, parent->ino) != 0) { free_inode_num(parent->mnt, ino_num); return -1; }
    if (data && len) {
        if (minix_write_inode_data(&ino, 0, len, (uint8_t *)data) != len) { free_inode_data(&ino); free_inode_num(parent->mnt, ino_num); return -1; }
    }
    if (dir_add_entry(parent, name, ino_num) != 0) { free_inode_data(&ino); free_inode_num(parent->mnt, ino_num); return -1; }
    if (out) *out = make_node(parent->mnt, ino_num, name);
    return 0;
}

static int minix_create(struct vfs_node *parent, const char *name, uint32_t type, struct vfs_node **out) {
    struct minix_inode *dir = (struct minix_inode *)vfs_get_fs_data(parent);
    if (!dir) return -1;
    uint32_t mode = 0;
    if (type == VFS_NODE_DIRECTORY) mode = S_IFDIR | 0755u;
    else if (type == VFS_NODE_FILE) mode = S_IFREG | 0644u;
    else if (type == VFS_NODE_CHAR_DEVICE) mode = S_IFCHR | 0600u;
    else if (type == VFS_NODE_BLOCK_DEVICE) mode = S_IFBLK | 0600u;
    else return -1;
    return minix_create_raw(dir, name, mode, 0, 0, out);
}

static int minix_unlink(struct vfs_node *parent, const char *name) {
    struct minix_inode *dir = (struct minix_inode *)vfs_get_fs_data(parent);
    uint64_t idx = 0; uint32_t ino_num = 0;
    if (!dir || !name_valid(dir->mnt, name) || dir_find_entry(dir, name, &idx, &ino_num) != 0) return -1;
    struct minix_inode ino;
    if (read_inode(dir->mnt, ino_num, &ino) != 0) return -1;
    if ((ino.mode & S_IFMT) == S_IFDIR && ino.size > 2ull * ino.mnt->dirent_size) {
        uint64_t entries = ino.size / ino.mnt->dirent_size;
        for (uint64_t i = 2; i < entries; i++) { uint32_t x = 0; if (read_dirent_at(&ino, i, &x, 0, 0, 0) == 0 && x) return -1; }
    }
    if (write_dirent_at(dir, idx, 0, 0) != 0) return -1;
    if (ino.links) ino.links--;
    if (ino.links == 0) { free_inode_data(&ino); memset(&ino, 0, sizeof(ino)); ino.mnt = dir->mnt; ino.ino = ino_num; write_inode(&ino); free_inode_num(dir->mnt, ino_num); }
    else write_inode(&ino);
    return 0;
}

static int minix_rename(struct vfs_node *old_parent, const char *old_name, struct vfs_node *new_parent, const char *new_name) {
    struct minix_inode *od = (struct minix_inode *)vfs_get_fs_data(old_parent);
    struct minix_inode *nd = (struct minix_inode *)vfs_get_fs_data(new_parent);
    uint64_t idx = 0; uint32_t ino = 0;
    if (!od || !nd || !name_valid(od->mnt, old_name) || !name_valid(nd->mnt, new_name)) return -1;
    if (od->mnt != nd->mnt) return -1;
    if (dir_find_entry(od, old_name, &idx, &ino) != 0) return -1;
    if (dir_find_entry(nd, new_name, 0, 0) == 0) return -1;
    if (dir_add_entry(nd, new_name, ino) != 0) return -1;
    return write_dirent_at(od, idx, 0, 0);
}

static int minix_truncate(struct vfs_node *node, uint64_t size) {
    struct minix_inode *ino = (struct minix_inode *)vfs_get_fs_data(node);
    if (!ino || ((ino->mode & S_IFMT) != S_IFREG && (ino->mode & S_IFMT) != S_IFLNK)) return -1;
    if (size == 0) {
        free_inode_data(ino);
        ino->size = 0;
        vfs_set_size(node, 0);
        return write_inode(ino);
    }
    if (size < ino->size) ino->size = size;
    else if (size > ino->size) {
        uint8_t zero = 0;
        if (minix_write_inode_data(ino, size - 1, 1, &zero) != 1) return -1;
    }
    vfs_set_size(node, ino->size);
    return write_inode(ino);
}

static int minix_probe(struct vfs_node *blockdev) {
    struct minix_mount mnt;
    int r = parse_super(blockdev, &mnt);
    if (r == 1) return FS_PROBE_YES;
    if (r == 0) return FS_PROBE_NO;
    return FS_PROBE_ERR;
}

static int minix_mount(struct vfs_node *blockdev, struct vfs_node *mountpoint, const char *flags) {
    (void)flags;
    if (!blockdev || !mountpoint) return -1;
    struct minix_mount *mnt = (struct minix_mount *)kmalloc(sizeof(*mnt));
    if (!mnt) return -1;
    if (parse_super(blockdev, mnt) != 1) { kfree(mnt); return -1; }
    struct vfs_node *root = make_node(mnt, MINIX_ROOT_INO, "minix");
    if (!root) { kfree(mnt); return -1; }
    struct minix_inode *root_inode = (struct minix_inode *)vfs_get_fs_data(root);
    vfs_set_size(mountpoint, root_inode ? root_inode->size : 0);
    vfs_set_fs_data(mountpoint, vfs_get_fs_data(root));
    attach_ops(mountpoint, VFS_NODE_DIRECTORY);
    kfree(root);
    klog(LOG_INFO, "minixfs: mounted v%d%s block=%d name=%d rw\n", mnt->version, mnt->swapped ? " swapped" : "", mnt->block_size, mnt->name_len);
    return 0;
}

static int minix_unmount(struct vfs_node *mountpoint) {
    struct minix_inode *root = (struct minix_inode *)vfs_get_fs_data(mountpoint);
    if (root) {
        struct minix_mount *mnt = root->mnt;
        kfree(root);
        if (mnt) kfree(mnt);
    }
    return 0;
}

static struct fs_driver minix_driver = { .name = "minixfs", .mount = minix_mount, .next = 0, .probe = minix_probe, .unmount = minix_unmount, .flags = 0 };

static int minixfs_init(void) { return fs_register(&minix_driver); }

MODULE_INFO("minixfs", minixfs_init, 0, 0, "fs");
