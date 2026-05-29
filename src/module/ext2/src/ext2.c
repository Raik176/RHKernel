#include "mod/fs.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "smp/lock.h"
#include "string.h"

#define EXT2_SUPER_OFFSET 1024u
#define EXT2_SUPER_MAGIC  0xEF53u
#define EXT2_ROOT_INO     2u
#define EXT2_N_BLOCKS     15u
#define EXT2_NAME_LEN     255u
#define EXT2_INODE_CACHE_SIZE 128u
#define EXT2_PTR_CACHE_SIZE 8u

#define EXT2_S_IFDIR 0x4000u
#define EXT2_S_IFREG 0x8000u
#define EXT2_FT_REG_FILE 1u
#define EXT2_FT_DIR      2u

#define EXT2_STATE_VALID 1u
#define EXT2_ERRORS_CONTINUE 1u
#define EXT2_ERRORS_RO 2u
#define EXT2_ERRORS_PANIC 3u
#define EXT2_REV_GOOD_OLD 0u
#define EXT2_REV_DYNAMIC 1u
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL 0x0004u
#define EXT2_FEATURE_INCOMPAT_COMPRESSION 0x0001u
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define EXT2_FEATURE_INCOMPAT_RECOVER 0x0004u
#define EXT2_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008u
#define EXT2_FEATURE_INCOMPAT_META_BG 0x0010u
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001u
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE 0x0002u
#define EXT2_FEATURE_RO_COMPAT_BTREE_DIR 0x0004u

#define EXT2_FEATURE_COMPAT_SUPPORTED 0u
#define EXT2_FEATURE_INCOMPAT_SUPPORTED EXT2_FEATURE_INCOMPAT_FILETYPE
#define EXT2_FEATURE_RO_COMPAT_SUPPORTED \
    (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT2_FEATURE_RO_COMPAT_LARGE_FILE | \
     EXT2_FEATURE_RO_COMPAT_BTREE_DIR)

#define EXT2_BGD_BLOCK_BITMAP 0u
#define EXT2_BGD_INODE_BITMAP 4u
#define EXT2_BGD_INODE_TABLE  8u
#define EXT2_BGD_FREE_BLOCKS  12u
#define EXT2_BGD_FREE_INODES  14u
#define EXT2_BGD_USED_DIRS    16u


struct ext2_inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size_lo;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[EXT2_N_BLOCKS];
    uint32_t generation;
    uint32_t file_acl;
    uint32_t size_high;
};

struct ext2_inode_cache_entry {
    uint32_t ino;
    uint32_t valid;
    struct ext2_inode inode;
};

struct ext2_ptr_cache_entry {
    uint32_t block;
    uint32_t last_used;
    uint8_t valid;
    uint8_t loading;
    uint8_t *data;
};

struct ext2_fs {
    struct vfs_node *dev;
    uint32_t block_size;
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t first_data_block;
    uint16_t inode_size;
    uint32_t groups_count;
    uint32_t next_block_alloc_group;
    uint32_t next_inode_alloc_group;
    uint8_t *io_scratch;
    volatile uint32_t io_scratch_busy;
    spinlock_t io_scratch_lock;
    spinlock_t inode_cache_lock;
    spinlock_t ptr_cache_lock;
    uint32_t ptr_cache_clock;
    struct ext2_inode_cache_entry inode_cache[EXT2_INODE_CACHE_SIZE];
    struct ext2_ptr_cache_entry ptr_cache[EXT2_PTR_CACHE_SIZE];
};


struct ext2_node {
    struct ext2_fs *fs;
    uint32_t ino;
    uint16_t mode;
    uint64_t size;
    uint32_t block[EXT2_N_BLOCKS];
};

static struct vfs_node *ext2_finddir(struct vfs_node *parent, const char *name);
static int ext2_readdir(struct vfs_node *parent, uint64_t index, struct vfs_dirent *out);
static uint64_t ext2_file_read(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t ext2_file_write(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf);
static int ext2_create(struct vfs_node *parent, const char *name, uint32_t type, struct vfs_node **out);
static int ext2_unlink(struct vfs_node *parent, const char *name);
static int ext2_rename(struct vfs_node *old_parent, const char *old_name, struct vfs_node *new_parent, const char *new_name);
static int ext2_truncate_vnode(struct vfs_node *vnode, uint64_t size);

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint16_t rec_len_for(uint8_t name_len) { return (uint16_t)((8u + name_len + 3u) & ~3u); }
static int ext2_read_bgd(struct ext2_fs *fs, uint32_t group, uint8_t gd[32]);

static uint32_t ext2_group_first_block(struct ext2_fs *fs, uint32_t group) {
    return fs->first_data_block + group * fs->blocks_per_group;
}

static uint32_t ext2_group_block_count(struct ext2_fs *fs, uint32_t group) {
    uint32_t first = ext2_group_first_block(fs, group);
    if (first >= fs->blocks_count) return 0;
    uint32_t count = fs->blocks_count - first;
    return count < fs->blocks_per_group ? count : fs->blocks_per_group;
}

static uint32_t ext2_group_inode_count(struct ext2_fs *fs, uint32_t group) {
    uint64_t first = (uint64_t)group * fs->inodes_per_group;
    if (first >= fs->inodes_count) return 0;
    uint64_t count = fs->inodes_count - first;
    return count < fs->inodes_per_group ? (uint32_t)count : fs->inodes_per_group;
}

static int ext2_block_valid(struct ext2_fs *fs, uint32_t block_no) {
    return block_no >= fs->first_data_block && block_no < fs->blocks_count;
}

static int ext2_block_range_valid(struct ext2_fs *fs, uint32_t first, uint32_t count) {
    if (!count || !ext2_block_valid(fs, first)) return 0;
    if (UINT32_MAX - first < count - 1) return 0;
    return first + count - 1 < fs->blocks_count;
}

static int ext2_block_in_group(struct ext2_fs *fs, uint32_t group, uint32_t block) {
    uint32_t first = ext2_group_first_block(fs, group);
    uint32_t count = ext2_group_block_count(fs, group);
    return count && block >= first && block < first + count;
}

static int ext2_inode_mode_supported(uint16_t mode) {
    uint16_t type = mode & 0xF000;
    return type == EXT2_S_IFREG || type == EXT2_S_IFDIR;
}

static int ext2_dir_file_type_valid(uint8_t type) {
    return type == 0 || type == EXT2_FT_REG_FILE || type == EXT2_FT_DIR;
}

static int ext2_validate_dirent(struct ext2_fs *fs, const uint8_t *de, uint32_t boff) {
    uint32_t ino = rd32(de);
    uint16_t rec_len = rd16(de + 4);
    uint8_t name_len = de[6];
    uint8_t file_type = de[7];

    if (rec_len < 8 || (rec_len & 3u) || boff + rec_len > fs->block_size) return -1;
    if (name_len > rec_len - 8) return -1;
    if (ino > fs->inodes_count) return -1;
    if (!ext2_dir_file_type_valid(file_type)) return -1;
    if (ino == 0 && name_len != 0) return -1;
    return 0;
}

static int ext2_validate_inode_blocks(struct ext2_fs *fs, const struct ext2_inode *in) {
    uint16_t type = in->mode & 0xF000;
    if (!ext2_inode_mode_supported(in->mode)) return -1;
    if (type == EXT2_S_IFDIR && in->size_high != 0) return -1;
    if (type == EXT2_S_IFDIR && (in->size_lo % fs->block_size) != 0) return -1;
    for (uint32_t i = 0; i < EXT2_N_BLOCKS; i++) {
        if (in->block[i] && !ext2_block_valid(fs, in->block[i])) return -1;
    }
    return 0;
}

static int ext2_validate_bgd(struct ext2_fs *fs) {
    uint64_t inode_table_blocks = ((uint64_t)fs->inodes_per_group * fs->inode_size + fs->block_size - 1) / fs->block_size;
    if (inode_table_blocks == 0 || inode_table_blocks > UINT32_MAX) return -1;

    for (uint32_t g = 0; g < fs->groups_count; g++) {
        uint8_t gd[32];
        if (ext2_read_bgd(fs, g, gd) != 0) return -1;
        uint32_t block_bitmap = rd32(gd + EXT2_BGD_BLOCK_BITMAP);
        uint32_t inode_bitmap = rd32(gd + EXT2_BGD_INODE_BITMAP);
        uint32_t inode_table = rd32(gd + EXT2_BGD_INODE_TABLE);
        uint32_t group_blocks = ext2_group_block_count(fs, g);
        uint32_t group_inodes = ext2_group_inode_count(fs, g);

        if (!group_blocks || !group_inodes) return -1;
        if (!ext2_block_valid(fs, block_bitmap) || !ext2_block_valid(fs, inode_bitmap)) return -1;
        if (!ext2_block_range_valid(fs, inode_table, (uint32_t)inode_table_blocks)) return -1;
        if (!ext2_block_in_group(fs, g, block_bitmap) || !ext2_block_in_group(fs, g, inode_bitmap)) return -1;
        if (!ext2_block_in_group(fs, g, inode_table) ||
            !ext2_block_in_group(fs, g, inode_table + (uint32_t)inode_table_blocks - 1))
            return -1;
        if (rd16(gd + EXT2_BGD_FREE_BLOCKS) > group_blocks) return -1;
        if (rd16(gd + EXT2_BGD_FREE_INODES) > group_inodes) return -1;
        if (rd16(gd + EXT2_BGD_USED_DIRS) > group_inodes) return -1;
    }
    return 0;
}

static int ext2_read_bytes(struct ext2_fs *fs, uint64_t off, uint64_t size, void *buf) { return block_read(fs->dev, off, size, buf) == size ? 0 : -1; }
static int ext2_write_bytes(struct ext2_fs *fs, uint64_t off, uint64_t size, const void *buf) { return block_write(fs->dev, off, size, buf) == size ? 0 : -1; }
static int ext2_read_block(struct ext2_fs *fs, uint32_t block_no, void *buf) { if (!block_no) { memset(buf, 0, fs->block_size); return 0; } if (block_no >= fs->blocks_count) return -1; return ext2_read_bytes(fs, (uint64_t)block_no * fs->block_size, fs->block_size, buf); }
static int ext2_write_block(struct ext2_fs *fs, uint32_t block_no, const void *buf) { if (!block_no || block_no >= fs->blocks_count) return -1; return ext2_write_bytes(fs, (uint64_t)block_no * fs->block_size, fs->block_size, buf); }

static uint64_t ext2_bgdt_off(struct ext2_fs *fs) { return (fs->block_size == 1024) ? 2048 : fs->block_size; }
static int ext2_read_bgd(struct ext2_fs *fs, uint32_t group, uint8_t gd[32]) { if (group >= fs->groups_count) return -1; return ext2_read_bytes(fs, ext2_bgdt_off(fs) + (uint64_t)group * 32, 32, gd); }
static int ext2_write_bgd(struct ext2_fs *fs, uint32_t group, const uint8_t gd[32]) { if (group >= fs->groups_count) return -1; return ext2_write_bytes(fs, ext2_bgdt_off(fs) + (uint64_t)group * 32, 32, gd); }

static uint8_t *ext2_acquire_scratch(struct ext2_fs *fs) {
    if (!fs || !fs->io_scratch) return 0;

    for (;;) {
        uint64_t flags;
        spinlock_acquire(&fs->io_scratch_lock, &flags);
        if (!fs->io_scratch_busy) {
            fs->io_scratch_busy = 1;
            uint8_t *scratch = fs->io_scratch;
            spinlock_release(&fs->io_scratch_lock, flags);
            return scratch;
        }
        spinlock_release(&fs->io_scratch_lock, flags);
        __asm__ volatile("pause");
    }
}

static void ext2_release_scratch(struct ext2_fs *fs) {
    uint64_t flags;
    spinlock_acquire(&fs->io_scratch_lock, &flags);
    fs->io_scratch_busy = 0;
    spinlock_release(&fs->io_scratch_lock, flags);
}

static int ext2_update_super_counts(struct ext2_fs *fs) {
    uint8_t sb[1024];
    if (ext2_read_bytes(fs, EXT2_SUPER_OFFSET, sizeof(sb), sb) != 0) return -1;
    wr32(sb + 12, fs->free_blocks_count);
    wr32(sb + 16, fs->free_inodes_count);
    return ext2_write_bytes(fs, EXT2_SUPER_OFFSET, sizeof(sb), sb);
}

static uint64_t ext2_inode_offset(struct ext2_fs *fs, uint32_t ino) {
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;
    uint8_t gd[32];
    if (ext2_read_bgd(fs, group, gd) != 0) return 0;
    uint32_t inode_table = rd32(gd + EXT2_BGD_INODE_TABLE);
    if (!inode_table) return 0;
    return (uint64_t)inode_table * fs->block_size + (uint64_t)index * fs->inode_size;
}

static void ext2_decode_inode(const uint8_t *raw, struct ext2_inode *out) {
    memset(out, 0, sizeof(*out));
    out->mode = rd16(raw + 0); out->uid = rd16(raw + 2); out->size_lo = rd32(raw + 4);
    out->atime = rd32(raw + 8); out->ctime = rd32(raw + 12); out->mtime = rd32(raw + 16); out->dtime = rd32(raw + 20);
    out->gid = rd16(raw + 24); out->links_count = rd16(raw + 26); out->blocks = rd32(raw + 28); out->flags = rd32(raw + 32); out->osd1 = rd32(raw + 36);
    for (uint32_t i = 0; i < EXT2_N_BLOCKS; i++) out->block[i] = rd32(raw + 40 + i * 4);
    out->generation = rd32(raw + 100); out->file_acl = rd32(raw + 104); out->size_high = rd32(raw + 108);
}

static void ext2_encode_inode(uint8_t *raw, const struct ext2_inode *in) {
    wr16(raw + 0, in->mode); wr16(raw + 2, in->uid); wr32(raw + 4, in->size_lo);
    wr32(raw + 8, in->atime); wr32(raw + 12, in->ctime); wr32(raw + 16, in->mtime); wr32(raw + 20, in->dtime);
    wr16(raw + 24, in->gid); wr16(raw + 26, in->links_count); wr32(raw + 28, in->blocks); wr32(raw + 32, in->flags); wr32(raw + 36, in->osd1);
    for (uint32_t i = 0; i < EXT2_N_BLOCKS; i++) wr32(raw + 40 + i * 4, in->block[i]);
    wr32(raw + 100, in->generation); wr32(raw + 104, in->file_acl); wr32(raw + 108, in->size_high);
}

static int ext2_read_inode_uncached(struct ext2_fs *fs, uint32_t ino, struct ext2_inode *out) {
    uint64_t inode_off = ext2_inode_offset(fs, ino);
    if (!inode_off) return -1;
    uint8_t raw[256];
    uint32_t want = fs->inode_size;
    if (want > sizeof(raw)) want = sizeof(raw);
    memset(raw, 0, sizeof(raw));
    if (ext2_read_bytes(fs, inode_off, want, raw) != 0) return -1;
    ext2_decode_inode(raw, out);
    return 0;
}

static int ext2_read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_inode *out) {
    if (!fs || !out || ino == 0 || ino > fs->inodes_count) return -1;
    uint32_t slot = ino % EXT2_INODE_CACHE_SIZE;
    uint64_t flags;
    spinlock_acquire(&fs->inode_cache_lock, &flags);
    if (fs->inode_cache[slot].valid && fs->inode_cache[slot].ino == ino) {
        *out = fs->inode_cache[slot].inode;
        spinlock_release(&fs->inode_cache_lock, flags);
        return 0;
    }
    spinlock_release(&fs->inode_cache_lock, flags);

    struct ext2_inode in;
    if (ext2_read_inode_uncached(fs, ino, &in) != 0) return -1;

    spinlock_acquire(&fs->inode_cache_lock, &flags);
    fs->inode_cache[slot].ino = ino;
    fs->inode_cache[slot].inode = in;
    fs->inode_cache[slot].valid = 1;
    spinlock_release(&fs->inode_cache_lock, flags);
    *out = in;
    return 0;
}

static int ext2_write_inode(struct ext2_fs *fs, uint32_t ino, const struct ext2_inode *in) {
    if (!fs || !in || ino == 0 || ino > fs->inodes_count) return -1;
    uint64_t inode_off = ext2_inode_offset(fs, ino);
    if (!inode_off) return -1;
    uint8_t raw[256];
    uint32_t want = fs->inode_size;
    if (want > sizeof(raw)) want = sizeof(raw);
    memset(raw, 0, sizeof(raw));
    if (want > 0 && ext2_read_bytes(fs, inode_off, want, raw) != 0) return -1;
    ext2_encode_inode(raw, in);
    if (ext2_write_bytes(fs, inode_off, want, raw) != 0) return -1;

    uint32_t slot = ino % EXT2_INODE_CACHE_SIZE;
    uint64_t flags;
    spinlock_acquire(&fs->inode_cache_lock, &flags);
    fs->inode_cache[slot].ino = ino;
    fs->inode_cache[slot].inode = *in;
    fs->inode_cache[slot].valid = 1;
    spinlock_release(&fs->inode_cache_lock, flags);
    return 0;
}

static void ext2_node_from_inode(struct ext2_node *n, struct ext2_fs *fs, uint32_t ino, const struct ext2_inode *in) {
    memset(n, 0, sizeof(*n)); n->fs = fs; n->ino = ino; n->mode = in->mode; n->size = in->size_lo;
    if ((in->mode & 0xF000) == EXT2_S_IFREG) n->size |= ((uint64_t)in->size_high << 32);
    for (uint32_t i = 0; i < EXT2_N_BLOCKS; i++) n->block[i] = in->block[i];
}

static struct ext2_node *ext2_make_node(struct ext2_fs *fs, uint32_t ino) {
    struct ext2_inode in;
    if (ext2_read_inode(fs, ino, &in) != 0) return 0;
    if (ext2_validate_inode_blocks(fs, &in) != 0) return 0;
    struct ext2_node *n = (struct ext2_node *)kmalloc(sizeof(*n));
    if (!n) return 0;
    ext2_node_from_inode(n, fs, ino, &in);
    return n;
}

static uint32_t ext2_count_allocated_blocks(struct ext2_node *node);

static void ext2_sync_node_inode(struct ext2_node *node, struct ext2_inode *in) {
    in->mode = node->mode; in->size_lo = (uint32_t)node->size; in->size_high = (uint32_t)(node->size >> 32);
    if ((node->mode & 0xF000) != EXT2_S_IFREG) in->size_high = 0;
    for (uint32_t i = 0; i < EXT2_N_BLOCKS; i++) in->block[i] = node->block[i];
    uint64_t sectors = (uint64_t)ext2_count_allocated_blocks(node) * (node->fs->block_size / 512);
    in->blocks = sectors > UINT32_MAX ? UINT32_MAX : (uint32_t)sectors;
}

static uint32_t ext2_alloc_block(struct ext2_fs *fs);
static int ext2_free_block(struct ext2_fs *fs, uint32_t block_no);

static void ext2_ptr_cache_invalidate(struct ext2_fs *fs, uint32_t block) {
    if (!fs || !block) return;
    for (;;) {
        uint64_t flags;
        int wait = 0;
        spinlock_acquire(&fs->ptr_cache_lock, &flags);
        for (uint32_t i = 0; i < EXT2_PTR_CACHE_SIZE; i++) {
            struct ext2_ptr_cache_entry *e = &fs->ptr_cache[i];
            if (e->block == block && e->loading) {
                wait = 1;
                break;
            }
        }
        if (!wait) {
            for (uint32_t i = 0; i < EXT2_PTR_CACHE_SIZE; i++) {
                struct ext2_ptr_cache_entry *e = &fs->ptr_cache[i];
                if (e->block == block) {
                    e->valid = 0;
                    e->block = 0;
                }
            }
            spinlock_release(&fs->ptr_cache_lock, flags);
            return;
        }
        spinlock_release(&fs->ptr_cache_lock, flags);
        __asm__ volatile("pause");
    }
}

static uint32_t ext2_read_ptr_block(struct ext2_fs *fs, uint32_t block, uint32_t index) {
    if (!fs || !block || index >= fs->block_size / 4 || !ext2_block_valid(fs, block)) return 0;

    for (;;) {
        uint64_t flags;
        spinlock_acquire(&fs->ptr_cache_lock, &flags);
        int wait = 0;
        for (uint32_t i = 0; i < EXT2_PTR_CACHE_SIZE; i++) {
            struct ext2_ptr_cache_entry *e = &fs->ptr_cache[i];
            if (e->block == block && e->valid && !e->loading) {
                e->last_used = ++fs->ptr_cache_clock;
                uint32_t out = rd32(e->data + index * 4);
                spinlock_release(&fs->ptr_cache_lock, flags);
                return out && ext2_block_valid(fs, out) ? out : 0;
            }
            if (e->block == block && e->loading) {
                wait = 1;
                break;
            }
        }
        if (wait) {
            spinlock_release(&fs->ptr_cache_lock, flags);
            __asm__ volatile("pause");
            continue;
        }

        struct ext2_ptr_cache_entry *victim = 0;
        for (uint32_t i = 0; i < EXT2_PTR_CACHE_SIZE; i++) {
            struct ext2_ptr_cache_entry *e = &fs->ptr_cache[i];
            if (!e->data) continue;
            if (!e->valid && !e->loading) { victim = e; break; }
            if (!e->loading && (!victim || e->last_used < victim->last_used)) victim = e;
        }
        if (!victim) {
            spinlock_release(&fs->ptr_cache_lock, flags);
            uint8_t *tmp = (uint8_t *)kmalloc(fs->block_size);
            if (!tmp) return 0;
            uint32_t out = 0;
            if (ext2_read_block(fs, block, tmp) == 0) out = rd32(tmp + index * 4);
            kfree(tmp);
            return out && ext2_block_valid(fs, out) ? out : 0;
        }
        victim->block = block;
        victim->valid = 0;
        victim->loading = 1;
        victim->last_used = ++fs->ptr_cache_clock;
        uint8_t *data = victim->data;
        spinlock_release(&fs->ptr_cache_lock, flags);

        int ok = ext2_read_block(fs, block, data) == 0;

        spinlock_acquire(&fs->ptr_cache_lock, &flags);
        if (victim->block == block && victim->loading) {
            victim->valid = ok ? 1 : 0;
            victim->loading = 0;
            if (!ok) victim->block = 0;
        }
        uint32_t out = ok ? rd32(data + index * 4) : 0;
        spinlock_release(&fs->ptr_cache_lock, flags);
        return out && ext2_block_valid(fs, out) ? out : 0;
    }
}

static uint32_t ext2_bmap_indirect(struct ext2_fs *fs, uint32_t root, uint32_t depth, uint32_t index) {
    if (!root) return 0;
    uint32_t ptrs = fs->block_size / 4;
    if (index >= ptrs && depth == 1) return 0;
    if (depth == 1) return ext2_read_ptr_block(fs, root, index);
    uint32_t span = 1;
    for (uint32_t i = 1; i < depth; i++) span *= ptrs;
    uint32_t slot = index / span;
    uint32_t rem = index % span;
    uint32_t next = ext2_read_ptr_block(fs, root, slot);
    return ext2_bmap_indirect(fs, next, depth - 1, rem);
}

static uint64_t ext2_max_file_blocks(struct ext2_fs *fs) {
    uint64_t ptrs = fs->block_size / 4;
    return 12 + ptrs + ptrs * ptrs + ptrs * ptrs * ptrs;
}

static uint64_t ext2_max_file_size(struct ext2_fs *fs) {
    uint64_t blocks = ext2_max_file_blocks(fs);
    if (blocks > UINT64_MAX / fs->block_size) return UINT64_MAX;
    return blocks * fs->block_size;
}

static uint32_t ext2_bmap(struct ext2_node *node, uint32_t file_block) {
    struct ext2_fs *fs = node->fs;
    uint32_t ptrs = fs->block_size / 4;
    if ((uint64_t)file_block >= ext2_max_file_blocks(fs)) return 0;
    if (file_block < 12) return node->block[file_block];
    file_block -= 12;
    if (file_block < ptrs) return ext2_bmap_indirect(fs, node->block[12], 1, file_block);
    file_block -= ptrs;
    uint32_t dbl_span = ptrs * ptrs;
    if (file_block < dbl_span) return ext2_bmap_indirect(fs, node->block[13], 2, file_block);
    file_block -= dbl_span;
    return ext2_bmap_indirect(fs, node->block[14], 3, file_block);
}

static int ext2_ptr_block_empty(struct ext2_fs *fs, uint32_t block) {
    if (!block) return 1;
    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    if (!buf) return 0;
    int empty = 1;
    if (ext2_read_block(fs, block, buf) != 0) empty = 0;
    else for (uint32_t i = 0; i < fs->block_size / 4; i++) if (rd32(buf + i * 4)) { empty = 0; break; }
    kfree(buf);
    return empty;
}

static int ext2_set_indirect(struct ext2_fs *fs, uint32_t *root, uint32_t depth, uint32_t index, uint32_t disk_block) {
    uint32_t ptrs = fs->block_size / 4;
    if (!*root) {
        if (!disk_block) return 0;
        *root = ext2_alloc_block(fs);
        if (!*root) return -1;
    }
    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    if (!buf) return -1;
    if (ext2_read_block(fs, *root, buf) != 0) { kfree(buf); return -1; }
    if (depth == 1) {
        wr32(buf + index * 4, disk_block);
        int rc = ext2_write_block(fs, *root, buf);
        if (rc == 0) ext2_ptr_cache_invalidate(fs, *root);
        kfree(buf);
        if (rc == 0 && !disk_block && ext2_ptr_block_empty(fs, *root)) { ext2_free_block(fs, *root); *root = 0; }
        return rc;
    }
    uint32_t span = 1;
    for (uint32_t i = 1; i < depth; i++) span *= ptrs;
    uint32_t slot = index / span;
    uint32_t rem = index % span;
    uint32_t child = rd32(buf + slot * 4);
    int rc = ext2_set_indirect(fs, &child, depth - 1, rem, disk_block);
    if (rc == 0) {
        wr32(buf + slot * 4, child);
        rc = ext2_write_block(fs, *root, buf);
        if (rc == 0) ext2_ptr_cache_invalidate(fs, *root);
    }
    kfree(buf);
    if (rc == 0 && !disk_block && ext2_ptr_block_empty(fs, *root)) { ext2_free_block(fs, *root); *root = 0; }
    return rc;
}

static int ext2_set_bmap(struct ext2_node *node, uint32_t file_block, uint32_t disk_block) {
    struct ext2_fs *fs = node->fs;
    uint32_t ptrs = fs->block_size / 4;
    if ((uint64_t)file_block >= ext2_max_file_blocks(fs)) return -1;
    if (file_block < 12) { node->block[file_block] = disk_block; return 0; }
    file_block -= 12;
    if (file_block < ptrs) return ext2_set_indirect(fs, &node->block[12], 1, file_block, disk_block);
    file_block -= ptrs;
    uint32_t dbl_span = ptrs * ptrs;
    if (file_block < dbl_span) return ext2_set_indirect(fs, &node->block[13], 2, file_block, disk_block);
    file_block -= dbl_span;
    return ext2_set_indirect(fs, &node->block[14], 3, file_block, disk_block);
}


static uint32_t ext2_alloc_block(struct ext2_fs *fs) {
    uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
    uint8_t *zero = (uint8_t *)kmalloc(fs->block_size);
    if (!bm || !zero) { if (bm) kfree(bm); if (zero) kfree(zero); return 0; }
    memset(zero, 0, fs->block_size);
    uint32_t start_group = fs->groups_count ? fs->next_block_alloc_group % fs->groups_count : 0;
    for (uint32_t scan = 0; scan < fs->groups_count; scan++) {
        uint32_t g = (start_group + scan) % fs->groups_count;
        uint8_t gd[32]; if (ext2_read_bgd(fs, g, gd) != 0) continue;
        uint16_t free_blocks = rd16(gd + EXT2_BGD_FREE_BLOCKS); if (!free_blocks) continue;
        uint32_t bitmap = rd32(gd + EXT2_BGD_BLOCK_BITMAP); if (!bitmap || ext2_read_block(fs, bitmap, bm) != 0) continue;
        uint32_t group_blocks = ext2_group_block_count(fs, g);
        for (uint32_t bit = 0; bit < group_blocks; bit++) {
            uint32_t byte = bit / 8, mask = 1u << (bit % 8); if (bm[byte] & mask) continue;
            uint32_t block_no = ext2_group_first_block(fs, g) + bit;
            if (!ext2_block_valid(fs, block_no)) continue;
            bm[byte] |= mask;
            if (ext2_write_block(fs, bitmap, bm) != 0) break;
            wr16(gd + EXT2_BGD_FREE_BLOCKS, free_blocks - 1); ext2_write_bgd(fs, g, gd);
            if (fs->free_blocks_count) fs->free_blocks_count--;
            ext2_update_super_counts(fs);
            ext2_write_block(fs, block_no, zero);
            fs->next_block_alloc_group = g;
            kfree(bm); kfree(zero); return block_no;
        }
    }
    kfree(bm); kfree(zero); return 0;
}

static int ext2_free_block(struct ext2_fs *fs, uint32_t block_no) {
    if (!block_no || block_no >= fs->blocks_count || block_no < fs->first_data_block) return -1;
    uint32_t g = (block_no - fs->first_data_block) / fs->blocks_per_group;
    uint32_t bit = (block_no - fs->first_data_block) % fs->blocks_per_group;
    uint8_t gd[32]; uint8_t *bm = (uint8_t *)kmalloc(fs->block_size); if (!bm) return -1;
    if (ext2_read_bgd(fs, g, gd) != 0) { kfree(bm); return -1; }
    uint32_t bitmap = rd32(gd + EXT2_BGD_BLOCK_BITMAP); if (ext2_read_block(fs, bitmap, bm) != 0) { kfree(bm); return -1; }
    uint32_t byte = bit / 8, mask = 1u << (bit % 8);
    if (bm[byte] & mask) {
        bm[byte] &= (uint8_t)~mask; ext2_write_block(fs, bitmap, bm);
        wr16(gd + EXT2_BGD_FREE_BLOCKS, rd16(gd + EXT2_BGD_FREE_BLOCKS) + 1); ext2_write_bgd(fs, g, gd);
        fs->free_blocks_count++; ext2_update_super_counts(fs);
    }
    kfree(bm); return 0;
}

static uint32_t ext2_alloc_inode(struct ext2_fs *fs, uint16_t mode) {
    uint8_t *bm = (uint8_t *)kmalloc(fs->block_size); if (!bm) return 0;
    uint32_t start_group = fs->groups_count ? fs->next_block_alloc_group % fs->groups_count : 0;
    for (uint32_t scan = 0; scan < fs->groups_count; scan++) {
        uint32_t g = (start_group + scan) % fs->groups_count;
        uint8_t gd[32]; if (ext2_read_bgd(fs, g, gd) != 0) continue;
        uint16_t free_inodes = rd16(gd + EXT2_BGD_FREE_INODES); if (!free_inodes) continue;
        uint32_t bitmap = rd32(gd + EXT2_BGD_INODE_BITMAP); if (!bitmap || ext2_read_block(fs, bitmap, bm) != 0) continue;
        uint32_t group_inodes = ext2_group_inode_count(fs, g);
        for (uint32_t bit = 0; bit < group_inodes; bit++) {
            uint32_t byte = bit / 8, mask = 1u << (bit % 8); if (bm[byte] & mask) continue;
            uint32_t ino = g * fs->inodes_per_group + bit + 1; if (ino < 11 || ino > fs->inodes_count) continue;
            bm[byte] |= mask; if (ext2_write_block(fs, bitmap, bm) != 0) break;
            wr16(gd + EXT2_BGD_FREE_INODES, free_inodes - 1);
            if ((mode & 0xF000) == EXT2_S_IFDIR) wr16(gd + EXT2_BGD_USED_DIRS, rd16(gd + EXT2_BGD_USED_DIRS) + 1);
            ext2_write_bgd(fs, g, gd); if (fs->free_inodes_count) fs->free_inodes_count--; ext2_update_super_counts(fs);
            fs->next_inode_alloc_group = g;
            kfree(bm); return ino;
        }
    }
    kfree(bm); return 0;
}

static int ext2_free_inode(struct ext2_fs *fs, uint32_t ino, uint16_t mode) {
    if (ino < 11 || ino > fs->inodes_count) return -1;
    uint32_t g = (ino - 1) / fs->inodes_per_group; uint32_t bit = (ino - 1) % fs->inodes_per_group;
    uint8_t gd[32]; uint8_t *bm = (uint8_t *)kmalloc(fs->block_size); if (!bm) return -1;
    if (ext2_read_bgd(fs, g, gd) != 0) { kfree(bm); return -1; }
    uint32_t bitmap = rd32(gd + EXT2_BGD_INODE_BITMAP); if (ext2_read_block(fs, bitmap, bm) != 0) { kfree(bm); return -1; }
    uint32_t byte = bit / 8, mask = 1u << (bit % 8);
    if (bm[byte] & mask) {
        bm[byte] &= (uint8_t)~mask; ext2_write_block(fs, bitmap, bm);
        wr16(gd + EXT2_BGD_FREE_INODES, rd16(gd + EXT2_BGD_FREE_INODES) + 1);
        if ((mode & 0xF000) == EXT2_S_IFDIR && rd16(gd + EXT2_BGD_USED_DIRS)) wr16(gd + EXT2_BGD_USED_DIRS, rd16(gd + EXT2_BGD_USED_DIRS) - 1);
        ext2_write_bgd(fs, g, gd); fs->free_inodes_count++; ext2_update_super_counts(fs);
    }
    kfree(bm); return 0;
}

static int ext2_get_or_alloc_block(struct ext2_node *node, uint32_t file_block, uint32_t *out) {
    uint32_t b = ext2_bmap(node, file_block);
    if (!b) {
        b = ext2_alloc_block(node->fs);
        if (!b) return -1;
        if (ext2_set_bmap(node, file_block, b) != 0) { ext2_free_block(node->fs, b); return -1; }
    }
    *out = b; return 0;
}

static uint32_t ext2_count_ptr_tree(struct ext2_fs *fs, uint32_t block, uint32_t depth) {
    if (!block || !ext2_block_valid(fs, block)) return 0;
    uint32_t total = 1;
    if (depth == 0) return total;
    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    if (!buf) return total;
    if (ext2_read_block(fs, block, buf) == 0) {
        uint32_t ptrs = fs->block_size / 4;
        for (uint32_t i = 0; i < ptrs; i++) {
            uint32_t child = rd32(buf + i * 4);
            if (child) total += ext2_count_ptr_tree(fs, child, depth - 1);
        }
    }
    kfree(buf);
    return total;
}

static uint32_t ext2_count_allocated_blocks(struct ext2_node *node) {
    uint32_t blocks = 0;
    for (uint32_t i = 0; i < 12; i++) if (node->block[i]) blocks++;
    blocks += ext2_count_ptr_tree(node->fs, node->block[12], 1);
    blocks += ext2_count_ptr_tree(node->fs, node->block[13], 2);
    blocks += ext2_count_ptr_tree(node->fs, node->block[14], 3);
    return blocks;
}

static int ext2_save_node(struct ext2_node *node) {
    struct ext2_inode in;
    if (ext2_read_inode(node->fs, node->ino, &in) != 0) return -1;
    ext2_sync_node_inode(node, &in);
    return ext2_write_inode(node->fs, node->ino, &in);
}

static void ext2_attach_ops(struct vfs_node *vnode, uint32_t type) {
    if (type == VFS_NODE_DIRECTORY) {
        vfs_set_finddir(vnode, ext2_finddir); vfs_set_readdir(vnode, ext2_readdir); vfs_set_create(vnode, ext2_create); vfs_set_unlink(vnode, ext2_unlink); vfs_set_rename(vnode, ext2_rename);
    } else {
        vfs_set_read(vnode, ext2_file_read); vfs_set_write(vnode, ext2_file_write); vfs_set_truncate(vnode, ext2_truncate_vnode);
    }
}

static struct vfs_node *ext2_vnode_from_ino(struct ext2_fs *fs, uint32_t ino, const char *name) {
    struct ext2_node *child = ext2_make_node(fs, ino);
    if (!child) return 0;
    uint32_t type = ((child->mode & 0xF000) == EXT2_S_IFDIR) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    struct vfs_node *vnode = vfs_create_fs_node(name, type, ino, child->size, child);
    if (!vnode) { kfree(child); return 0; }
    ext2_attach_ops(vnode, type);
    return vnode;
}

static uint64_t ext2_file_read(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf) {
    struct ext2_node *node = (struct ext2_node *)vfs_get_fs_data(vnode);
    if (!node || !buf || (node->mode & 0xF000) != EXT2_S_IFREG) return 0;
    if (off > ext2_max_file_size(node->fs)) return 0;
    if (off >= node->size) return 0;
    if (size > node->size - off) size = node->size - off;
    struct ext2_fs *fs = node->fs;
    uint8_t *scratch = 0;
    uint64_t done = 0;
    while (done < size) {
        uint64_t abs = off + done;
        if (abs / fs->block_size > UINT32_MAX) break;
        uint32_t file_block = (uint32_t)(abs / fs->block_size);
        uint32_t block_off = (uint32_t)(abs % fs->block_size);
        uint64_t chunk = fs->block_size - block_off;
        if (chunk > size - done) chunk = size - done;
        uint32_t disk_block = ext2_bmap(node, file_block);
        if (!disk_block) {
            memset(buf + done, 0, chunk);
        } else if (block_off == 0 && chunk == fs->block_size) {
            if (ext2_read_bytes(fs, (uint64_t)disk_block * fs->block_size, fs->block_size, buf + done) != 0) break;
        } else {
            if (!scratch) { scratch = ext2_acquire_scratch(fs); if (!scratch) break; }
            if (ext2_read_block(fs, disk_block, scratch) != 0) break;
            memcpy(buf + done, scratch + block_off, chunk);
        }
        done += chunk;
    }
    if (scratch) ext2_release_scratch(fs);
    return done;
}

static uint64_t ext2_file_write(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf) {
    struct ext2_node *node = (struct ext2_node *)vfs_get_fs_data(vnode);
    if (!node || !buf || (node->mode & 0xF000) != EXT2_S_IFREG) return 0;
    if (off > ext2_max_file_size(node->fs)) return 0;
    struct ext2_fs *fs = node->fs;
    uint8_t *scratch = 0;
    uint64_t done = 0;
    while (done < size) {
        uint64_t abs = off + done;
        if (abs / fs->block_size > UINT32_MAX) break;
        uint32_t file_block = (uint32_t)(abs / fs->block_size);
        uint32_t block_off = (uint32_t)(abs % fs->block_size);
        uint64_t chunk = fs->block_size - block_off;
        if (chunk > size - done) chunk = size - done;
        uint32_t disk_block = 0;
        if (ext2_get_or_alloc_block(node, file_block, &disk_block) != 0) break;
        if (block_off == 0 && chunk == fs->block_size) {
            if (ext2_write_bytes(fs, (uint64_t)disk_block * fs->block_size, fs->block_size, buf + done) != 0) break;
        } else {
            if (!scratch) { scratch = ext2_acquire_scratch(fs); if (!scratch) break; }
            if (ext2_read_block(fs, disk_block, scratch) != 0) break;
            memcpy(scratch + block_off, buf + done, chunk);
            if (ext2_write_block(fs, disk_block, scratch) != 0) break;
        }
        done += chunk;
    }
    if (scratch) ext2_release_scratch(fs);
    if (done && off + done > node->size) { node->size = off + done; vfs_set_size(vnode, node->size); ext2_save_node(node); }
    return done;
}

static int ext2_name_len_checked(const char *name, uint8_t *len_out) {
    if (!name || !len_out) return 0;
    uint32_t len = 0;
    while (name[len]) {
        if (len == EXT2_NAME_LEN) return 0;
        len++;
    }
    if (!len) return 0;
    *len_out = (uint8_t)len;
    return 1;
}

static int ext2_name_eq_len(const char *want, uint8_t want_len, const uint8_t *name, uint8_t len) {
    return want_len == len && memcmp(want, name, len) == 0;
}

static int ext2_dir_find_entry_len(struct ext2_node *dir, const char *name, uint8_t want_len, uint32_t *ino_out, uint32_t *block_out, uint32_t *off_out, uint16_t *rec_out, uint8_t *type_out) {
    if (!dir || !name || !want_len || (dir->mode & 0xF000) != EXT2_S_IFDIR) return -1;
    struct ext2_fs *fs = dir->fs; uint8_t *buf = (uint8_t *)kmalloc(fs->block_size); if (!buf) return -1;
    uint64_t off = 0;
    while (off < dir->size) {
        uint32_t file_block = (uint32_t)(off / fs->block_size); uint32_t disk_block = ext2_bmap(dir, file_block); if (!disk_block) { kfree(buf); return -1; }
        if (ext2_read_block(fs, disk_block, buf) != 0) { kfree(buf); return -1; }
        uint32_t boff = 0;
        while (boff + 8 <= fs->block_size && off + boff < dir->size) {
            uint8_t *de = buf + boff; uint32_t ino = rd32(de); uint16_t rec_len = rd16(de + 4); uint8_t name_len = de[6]; uint8_t ft = de[7];
            if (ext2_validate_dirent(fs, de, boff) != 0) { kfree(buf); return -1; }
            if (ino && name_len && ext2_name_eq_len(name, want_len, de + 8, name_len)) {
                if (ino_out) *ino_out = ino;
                if (block_out) *block_out = disk_block;
                if (off_out) *off_out = boff;
                if (rec_out) *rec_out = rec_len;
                if (type_out) *type_out = ft;
                kfree(buf); return 0;
            }
            boff += rec_len;
        }
        off += fs->block_size;
    }
    kfree(buf); return -1;
}

static int ext2_dir_find_entry(struct ext2_node *dir, const char *name, uint32_t *ino_out, uint32_t *block_out, uint32_t *off_out, uint16_t *rec_out, uint8_t *type_out) {
    uint8_t len = 0;
    if (!ext2_name_len_checked(name, &len)) return -1;
    return ext2_dir_find_entry_len(dir, name, len, ino_out, block_out, off_out, rec_out, type_out);
}

static struct vfs_node *ext2_lookup_in_dir(struct ext2_node *dir, const char *name) {
    uint8_t name_len = 0;
    if (!ext2_name_len_checked(name, &name_len)) return 0;
    if ((name_len == 1 && name[0] == '.') || (name_len == 2 && name[0] == '.' && name[1] == '.')) return 0;
    uint32_t ino = 0;
    if (ext2_dir_find_entry_len(dir, name, name_len, &ino, 0, 0, 0, 0) != 0) return 0;
    return ext2_vnode_from_ino(dir->fs, ino, name);
}

static struct vfs_node *ext2_finddir(struct vfs_node *parent, const char *name) {
    struct ext2_node *dir = (struct ext2_node *)vfs_get_fs_data(parent);
    struct vfs_node *child = ext2_lookup_in_dir(dir, name);
    if (child) vfs_add_child(parent, child);
    return child;
}

static int ext2_readdir(struct vfs_node *parent, uint64_t index, struct vfs_dirent *out) {
    struct ext2_node *dir = (struct ext2_node *)vfs_get_fs_data(parent);
    if (!dir || !out || (dir->mode & 0xF000) != EXT2_S_IFDIR) return -1;

    struct ext2_fs *fs = dir->fs;
    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    if (!buf) return -1;

    uint64_t seen = 0;
    uint64_t off = 0;
    while (off < dir->size) {
        uint32_t file_block = (uint32_t)(off / fs->block_size);
        uint32_t disk_block = ext2_bmap(dir, file_block);
        if (!disk_block) { kfree(buf); return -1; }
        if (ext2_read_block(fs, disk_block, buf) != 0) { kfree(buf); return -1; }

        uint32_t boff = 0;
        while (boff + 8 <= fs->block_size && off + boff < dir->size) {
            uint8_t *de = buf + boff;
            uint32_t ino = rd32(de);
            uint16_t rec_len = rd16(de + 4);
            uint8_t name_len = de[6];
            uint8_t file_type = de[7];
            if (ext2_validate_dirent(fs, de, boff) != 0) {
                kfree(buf);
                return -1;
            }

            if (ino && name_len) {
                if (seen == index) {
                    char *dst = out->name;
                    uint64_t cap = out->name_capacity;
                    memset(out, 0, sizeof(*out));
                    out->inode = ino;
                    out->type = (file_type == EXT2_FT_DIR) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
                    out->name_len = name_len;
                    out->name = dst;
                    out->name_capacity = cap;
                    if (dst && cap > name_len) {
                        memcpy(dst, de + 8, name_len);
                        dst[name_len] = 0;
                    }
                    kfree(buf);
                    return 0;
                }
                seen++;
            }
            boff += rec_len;
        }
        off += fs->block_size;
    }

    kfree(buf);
    return -1;
}

static int ext2_dir_add_entry(struct ext2_node *dir, const char *name, uint32_t ino, uint8_t file_type) {
    uint8_t name_len = 0;
    if (!dir || !name || !ino || !ext2_name_len_checked(name, &name_len)) return -1;
    if (ext2_dir_find_entry_len(dir, name, name_len, 0, 0, 0, 0, 0) == 0) return -1;
    struct ext2_fs *fs = dir->fs; uint16_t need = rec_len_for(name_len);
    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size); if (!buf) return -1;
    uint32_t blocks = (uint32_t)((dir->size + fs->block_size - 1) / fs->block_size);
    for (uint32_t fb = 0; fb <= blocks; fb++) {
        uint32_t disk_block = ext2_bmap(dir, fb);
        int new_block = 0;
        if (!disk_block) { if (ext2_get_or_alloc_block(dir, fb, &disk_block) != 0) { kfree(buf); return -1; } memset(buf, 0, fs->block_size); wr32(buf, ino); wr16(buf + 4, fs->block_size); buf[6] = name_len; buf[7] = file_type; memcpy(buf + 8, name, name_len); new_block = 1; }
        else if (ext2_read_block(fs, disk_block, buf) != 0) { kfree(buf); return -1; }
        if (new_block) {
            if (ext2_write_block(fs, disk_block, buf) != 0) { kfree(buf); return -1; }
            if ((uint64_t)(fb + 1) * fs->block_size > dir->size) dir->size = (uint64_t)(fb + 1) * fs->block_size;
            ext2_save_node(dir); kfree(buf); return 0;
        }
        uint32_t boff = 0;
        while (boff + 8 <= fs->block_size) {
            uint8_t *de = buf + boff; uint32_t cur_ino = rd32(de); uint16_t rec_len = rd16(de + 4); uint8_t cur_name_len = de[6];
            if (ext2_validate_dirent(fs, de, boff) != 0) break;
            if (!cur_ino && rec_len >= need) {
                wr32(de, ino); wr16(de + 4, rec_len); de[6] = name_len; de[7] = file_type; memcpy(de + 8, name, name_len);
                if (ext2_write_block(fs, disk_block, buf) != 0) { kfree(buf); return -1; } kfree(buf); return 0;
            }
            uint16_t ideal = rec_len_for(cur_name_len);
            if (cur_ino && rec_len >= ideal + need) {
                uint16_t remain = rec_len - ideal; wr16(de + 4, ideal);
                uint8_t *ne = de + ideal; wr32(ne, ino); wr16(ne + 4, remain); ne[6] = name_len; ne[7] = file_type; memcpy(ne + 8, name, name_len);
                if (ext2_write_block(fs, disk_block, buf) != 0) { kfree(buf); return -1; } kfree(buf); return 0;
            }
            boff += rec_len;
        }
    }
    kfree(buf); return -1;
}

static int ext2_dir_remove_entry(struct ext2_node *dir, const char *name, uint32_t *ino_out) {
    uint8_t want_len = 0;
    if (!ext2_name_len_checked(name, &want_len)) return -1;
    struct ext2_fs *fs = dir->fs; uint8_t *buf = (uint8_t *)kmalloc(fs->block_size); if (!buf) return -1;
    uint64_t off = 0;
    while (off < dir->size) {
        uint32_t fb = (uint32_t)(off / fs->block_size); uint32_t disk_block = ext2_bmap(dir, fb); if (!disk_block) { kfree(buf); return -1; }
        if (ext2_read_block(fs, disk_block, buf) != 0) { kfree(buf); return -1; }
        uint32_t boff = 0, prev = 0xffffffff;
        while (boff + 8 <= fs->block_size) {
            uint8_t *de = buf + boff; uint32_t ino = rd32(de); uint16_t rec_len = rd16(de + 4); uint8_t name_len = de[6];
            if (ext2_validate_dirent(fs, de, boff) != 0) break;
            if (ino && name_len && ext2_name_eq_len(name, want_len, de + 8, name_len)) {
                if (ino_out) *ino_out = ino;
                if (prev != 0xffffffff) wr16(buf + prev + 4, rd16(buf + prev + 4) + rec_len);
                else wr32(de, 0);
                int rc = ext2_write_block(fs, disk_block, buf); kfree(buf); return rc;
            }
            prev = boff; boff += rec_len;
        }
        off += fs->block_size;
    }
    kfree(buf); return -1;
}

static int ext2_dir_is_empty(struct ext2_node *dir) {
    struct ext2_fs *fs = dir->fs; uint8_t *buf = (uint8_t *)kmalloc(fs->block_size); if (!buf) return 0;
    uint64_t off = 0; int empty = 1;
    while (off < dir->size && empty) {
        uint32_t disk_block = ext2_bmap(dir, (uint32_t)(off / fs->block_size)); if (!disk_block || ext2_read_block(fs, disk_block, buf) != 0) break;
        uint32_t boff = 0;
        while (boff + 8 <= fs->block_size) {
            uint8_t *de = buf + boff; uint32_t ino = rd32(de); uint16_t rec_len = rd16(de + 4); uint8_t nl = de[6];
            if (ext2_validate_dirent(fs, de, boff) != 0) { empty = 0; break; }
            if (ino && !(nl == 1 && de[8] == '.') && !(nl == 2 && de[8] == '.' && de[9] == '.')) { empty = 0; break; }
            boff += rec_len;
        }
        off += fs->block_size;
    }
    kfree(buf); return empty;
}

static int ext2_truncate_node(struct ext2_node *node, uint64_t new_size) {
    struct ext2_fs *fs = node->fs;
    if ((node->mode & 0xF000) != EXT2_S_IFREG) return -1;
    uint32_t old_blocks = (uint32_t)((node->size + fs->block_size - 1) / fs->block_size);
    uint32_t new_blocks = (uint32_t)((new_size + fs->block_size - 1) / fs->block_size);
    if (new_size > ext2_max_file_size(fs)) return -1;
    if (new_blocks < old_blocks) {
        for (uint32_t fb = new_blocks; fb < old_blocks; fb++) { uint32_t db = ext2_bmap(node, fb); if (db) { ext2_free_block(fs, db); ext2_set_bmap(node, fb, 0); } }
        if (node->block[12]) {
            uint8_t *buf = (uint8_t *)kmalloc(fs->block_size); int any = 0;
            if (buf && ext2_read_block(fs, node->block[12], buf) == 0) { for (uint32_t i=0;i<fs->block_size/4;i++) if (rd32(buf+i*4)) { any=1; break; } }
            if (!any) { ext2_free_block(fs, node->block[12]); node->block[12] = 0; }
            if (buf) kfree(buf);
        }
    }
    node->size = new_size; return ext2_save_node(node);
}

static int ext2_truncate_vnode(struct vfs_node *vnode, uint64_t size) {
    struct ext2_node *node = (struct ext2_node *)vfs_get_fs_data(vnode); if (!node) return -1;
    int rc = ext2_truncate_node(node, size); if (rc == 0) vfs_set_size(vnode, size); return rc;
}

static int ext2_create(struct vfs_node *parent, const char *name, uint32_t type, struct vfs_node **out) {
    uint8_t name_len = 0;
    if (!parent || !ext2_name_len_checked(name, &name_len) || type != VFS_NODE_FILE) return -1;
    struct ext2_node *dir = (struct ext2_node *)vfs_get_fs_data(parent); if (!dir) return -1;
    if (ext2_dir_find_entry_len(dir, name, name_len, 0, 0, 0, 0, 0) == 0) return -1;
    uint32_t ino = ext2_alloc_inode(dir->fs, EXT2_S_IFREG | 0644); if (!ino) return -1;
    struct ext2_inode in; memset(&in, 0, sizeof(in)); in.mode = EXT2_S_IFREG | 0644; in.links_count = 1;
    if (ext2_write_inode(dir->fs, ino, &in) != 0 || ext2_dir_add_entry(dir, name, ino, EXT2_FT_REG_FILE) != 0) { ext2_free_inode(dir->fs, ino, in.mode); return -1; }
    struct vfs_node *vnode = ext2_vnode_from_ino(dir->fs, ino, name); if (!vnode) return -1;
    vfs_add_child(parent, vnode); if (out) *out = vnode; return 0;
}

static int ext2_unlink(struct vfs_node *parent, const char *name) {
    uint8_t name_len = 0;
    struct ext2_node *dir = (struct ext2_node *)vfs_get_fs_data(parent); if (!dir || !ext2_name_len_checked(name, &name_len)) return -1;
    uint32_t ino = 0; if (ext2_dir_find_entry_len(dir, name, name_len, &ino, 0, 0, 0, 0) != 0) return -1;
    struct ext2_inode in; if (ext2_read_inode(dir->fs, ino, &in) != 0) return -1;
    struct ext2_node tmp; ext2_node_from_inode(&tmp, dir->fs, ino, &in);
    if ((tmp.mode & 0xF000) == EXT2_S_IFDIR && !ext2_dir_is_empty(&tmp)) return -1;
    if (ext2_dir_remove_entry(dir, name, 0) != 0) return -1;
    if (in.links_count > 0) in.links_count--;
    in.dtime = 1;
    if (in.links_count == 0) { if ((tmp.mode & 0xF000) == EXT2_S_IFREG) ext2_truncate_node(&tmp, 0); ext2_free_inode(dir->fs, ino, tmp.mode); memset(&in, 0, sizeof(in)); }
    return ext2_write_inode(dir->fs, ino, &in);
}

static int ext2_rename(struct vfs_node *old_parent, const char *old_name, struct vfs_node *new_parent, const char *new_name) {
    uint8_t old_len = 0;
    uint8_t new_len = 0;
    struct ext2_node *odir = (struct ext2_node *)vfs_get_fs_data(old_parent); struct ext2_node *ndir = (struct ext2_node *)vfs_get_fs_data(new_parent);
    if (!odir || !ndir || odir->fs != ndir->fs || !ext2_name_len_checked(old_name, &old_len) || !ext2_name_len_checked(new_name, &new_len)) return -1;
    uint32_t ino = 0; uint8_t ft = 0; if (ext2_dir_find_entry_len(odir, old_name, old_len, &ino, 0, 0, 0, &ft) != 0) return -1;
    struct ext2_inode moved; if (ext2_read_inode(odir->fs, ino, &moved) != 0) return -1;
    if ((moved.mode & 0xF000) == EXT2_S_IFDIR && odir != ndir) return -1;
    uint32_t existing = 0; if (ext2_dir_find_entry_len(ndir, new_name, new_len, &existing, 0, 0, 0, 0) == 0) { if (ext2_unlink(new_parent, new_name) != 0) return -1; }
    if (ext2_dir_add_entry(ndir, new_name, ino, ft ? ft : EXT2_FT_REG_FILE) != 0) return -1;
    if (ext2_dir_remove_entry(odir, old_name, 0) != 0) { ext2_dir_remove_entry(ndir, new_name, 0); return -1; }
    return 0;
}

static int ext2_probe_superblock(const uint8_t sb[1024], int quiet) {
    if (rd16(sb + 56) != EXT2_SUPER_MAGIC) return FS_PROBE_NO;

    uint32_t rev = rd32(sb + 76);
    uint16_t state = rd16(sb + 58);
    uint16_t errors = rd16(sb + 60);
    uint32_t compat = rd32(sb + 92);
    uint32_t incompat = rd32(sb + 96);
    uint32_t ro_compat = rd32(sb + 100);
    uint32_t log_block_size = rd32(sb + 24);

    if (rev != EXT2_REV_GOOD_OLD && rev != EXT2_REV_DYNAMIC) return FS_PROBE_ERR;
    if (state != EXT2_STATE_VALID || errors == EXT2_ERRORS_PANIC) return FS_PROBE_ERR;
    if ((compat & ~EXT2_FEATURE_COMPAT_SUPPORTED) != 0 ||
        (incompat & ~EXT2_FEATURE_INCOMPAT_SUPPORTED) != 0 ||
        (ro_compat & ~EXT2_FEATURE_RO_COMPAT_SUPPORTED) != 0) {
        if (!quiet) klog(LOG_ERR, "ext2: unsupported features compat=%x incompat=%x ro_compat=%x\n", compat, incompat, ro_compat);
        return FS_PROBE_ERR;
    }
    if ((compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) || (incompat & EXT2_FEATURE_INCOMPAT_RECOVER) ||
        (incompat & EXT2_FEATURE_INCOMPAT_JOURNAL_DEV))
        return FS_PROBE_ERR;
    if (log_block_size > 2) return FS_PROBE_ERR;
    return FS_PROBE_YES;
}

static int ext2_probe(struct vfs_node *blockdev) {
    uint8_t sb[1024];
    if (block_read(blockdev, EXT2_SUPER_OFFSET, sizeof(sb), sb) != sizeof(sb)) return FS_PROBE_ERR;
    return ext2_probe_superblock(sb, 1);
}

static int ext2_mount(struct vfs_node *blockdev, struct vfs_node *mountpoint, const char *flags) {
    (void)flags;
    uint8_t sb[1024]; if (block_read(blockdev, EXT2_SUPER_OFFSET, sizeof(sb), sb) != sizeof(sb)) return -1;
    int probe = ext2_probe_superblock(sb, 0);
    if (probe == FS_PROBE_NO) return -2;
    if (probe != FS_PROBE_YES) return -3;

    uint32_t rev = rd32(sb + 76);
    uint32_t first_ino = rev == EXT2_REV_GOOD_OLD ? 11 : rd32(sb + 84);
    uint32_t log_block_size = rd32(sb + 24);

    struct ext2_fs *fs = (struct ext2_fs *)kmalloc(sizeof(*fs)); if (!fs) return -3; memset(fs, 0, sizeof(*fs));
    fs->dev = blockdev; fs->inodes_count = rd32(sb + 0); fs->blocks_count = rd32(sb + 4); fs->free_blocks_count = rd32(sb + 12); fs->free_inodes_count = rd32(sb + 16);
    spinlock_init(&fs->io_scratch_lock); spinlock_init(&fs->inode_cache_lock); spinlock_init(&fs->ptr_cache_lock);
    fs->first_data_block = rd32(sb + 20); fs->block_size = 1024u << log_block_size; fs->blocks_per_group = rd32(sb + 32); fs->inodes_per_group = rd32(sb + 40); fs->inode_size = rev == EXT2_REV_GOOD_OLD ? 128 : rd16(sb + 88); if (!fs->inode_size) fs->inode_size = 128;
    uint32_t max_blocks_per_group = fs->block_size * 8u;
    if (fs->inodes_count < EXT2_ROOT_INO || fs->blocks_count == 0 || first_ino < 11 ||
        first_ino > fs->inodes_count || fs->first_data_block != (fs->block_size == 1024 ? 1u : 0u) ||
        fs->inodes_per_group == 0 || fs->blocks_per_group == 0 || fs->blocks_per_group > max_blocks_per_group ||
        fs->inodes_per_group > fs->block_size * 8u || fs->free_blocks_count > fs->blocks_count ||
        fs->free_inodes_count > fs->inodes_count || fs->inode_size < 128 || fs->inode_size > 256 ||
        (fs->inode_size & 3u) != 0) {
        klog(LOG_ERR, "ext2: invalid superblock block_size=%d blocks=%d inodes=%d bpg=%d ipg=%d free_blocks=%d free_inodes=%d inode_size=%d\n",
             fs->block_size, fs->blocks_count, fs->inodes_count, fs->blocks_per_group, fs->inodes_per_group,
             fs->free_blocks_count, fs->free_inodes_count, fs->inode_size);
        kfree(fs);
        return -4;
    }
    fs->groups_count = (fs->blocks_count - fs->first_data_block + fs->blocks_per_group - 1) / fs->blocks_per_group;
    if (fs->groups_count == 0 || ext2_validate_bgd(fs) != 0) { kfree(fs); return -5; }
    fs->io_scratch = (uint8_t *)kmalloc(fs->block_size);
    if (!fs->io_scratch) { kfree(fs); return -5; }
    for (uint32_t i = 0; i < EXT2_PTR_CACHE_SIZE; i++) {
        fs->ptr_cache[i].data = (uint8_t *)kmalloc(fs->block_size);
        if (!fs->ptr_cache[i].data) {
            for (uint32_t j = 0; j < i; j++) kfree(fs->ptr_cache[j].data);
            kfree(fs->io_scratch);
            kfree(fs);
            return -5;
        }
    }
    struct ext2_node *root = ext2_make_node(fs, EXT2_ROOT_INO);
    if (!root || (root->mode & 0xF000) != EXT2_S_IFDIR) {
        if (root) kfree(root);
        for (uint32_t i = 0; i < EXT2_PTR_CACHE_SIZE; i++) if (fs->ptr_cache[i].data) kfree(fs->ptr_cache[i].data);
        kfree(fs->io_scratch);
        kfree(fs);
        return -6;
    }
    vfs_set_fs_data(mountpoint, root); vfs_set_finddir(mountpoint, ext2_finddir); vfs_set_readdir(mountpoint, ext2_readdir); vfs_set_create(mountpoint, ext2_create); vfs_set_unlink(mountpoint, ext2_unlink); vfs_set_rename(mountpoint, ext2_rename); vfs_set_size(mountpoint, root->size);
    klog(LOG_INFO, "ext2: mounted rw block_size=%d blocks=%d inodes=%d free_blocks=%d free_inodes=%d\n", fs->block_size, fs->blocks_count, fs->inodes_count, fs->free_blocks_count, fs->free_inodes_count);
    return 0;
}

static void ext2_copy_text(char *dst, uint64_t cap, const char *src) {
    if (!dst || !cap) return;
    uint64_t i = 0;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int ext2_info(struct vfs_node *mountpoint, struct fs_info *out) {
    if (!mountpoint || !out) return -1;
    struct ext2_node *root = (struct ext2_node *)vfs_get_fs_data(mountpoint);
    if (!root || !root->fs) return -1;
    struct ext2_fs *fs = root->fs;
    memset(out, 0, sizeof(*out));
    out->version = FS_INFO_VERSION;
    out->block_size = fs->block_size;
    out->total_bytes = (uint64_t)fs->blocks_count * fs->block_size;
    out->free_bytes = (uint64_t)fs->free_blocks_count * fs->block_size;
    out->used_bytes = out->total_bytes >= out->free_bytes ? out->total_bytes - out->free_bytes : 0;
    out->max_file_size = 0xFFFFFFFFULL;
    ext2_copy_text(out->driver, sizeof(out->driver), "ext2");
    ext2_copy_text(out->type, sizeof(out->type), "ext2");
    return 0;
}

static int ext2_unmount(struct vfs_node *mountpoint) {
    struct ext2_node *root = (struct ext2_node *)vfs_get_fs_data(mountpoint);
    if (!root) return -1;
    struct ext2_fs *fs = root->fs;
    if (fs) {
        for (uint32_t i = 0; i < EXT2_PTR_CACHE_SIZE; i++) if (fs->ptr_cache[i].data) kfree(fs->ptr_cache[i].data);
        if (fs->io_scratch) kfree(fs->io_scratch);
        kfree(fs);
    }
    kfree(root);
    return 0;
}

static struct fs_driver ext2_driver = { .name = "ext2", .mount = ext2_mount, .next = 0, .probe = ext2_probe, .unmount = ext2_unmount, .info = ext2_info };
static int ext2_init(void) { return fs_register(&ext2_driver); }
MODULE_INFO("ext2", ext2_init, 0, 0, "fs");
