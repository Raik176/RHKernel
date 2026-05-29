#include "mod/fs.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "string.h"

#define VFAT_ATTR_RO      0x01u
#define VFAT_ATTR_HIDDEN  0x02u
#define VFAT_ATTR_SYSTEM  0x04u
#define VFAT_ATTR_LABEL   0x08u
#define VFAT_ATTR_DIR     0x10u
#define VFAT_ATTR_ARCHIVE 0x20u
#define VFAT_ATTR_LFN     0x0Fu

#define VFAT_EOC12 0x0FF8u
#define VFAT_EOC16 0xFFF8u
#define VFAT_EOC32 0x0FFFFFF8u
#define VFAT_BAD12 0x0FF7u
#define VFAT_BAD16 0xFFF7u
#define VFAT_BAD32 0x0FFFFFF7u
#define VFAT_MAX_LFN_CHARS 255u
#define VFAT_MAX_LFN_SLOTS 20u
#define VFAT_DIR_ENTRY_SIZE 32u
#define VFAT_ROOT_INO 1u

struct vfat_fs {
    struct vfs_node *dev;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint32_t reserved_sectors;
    uint32_t fat_sectors;
    uint32_t total_sectors;
    uint32_t root_dir_sectors;
    uint32_t first_fat_sector;
    uint32_t root_dir_sector;
    uint32_t first_data_sector;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint8_t fat_type;
    uint8_t readonly;
    uint32_t cluster_bytes;
    uint64_t image_bytes;
    uint8_t *fat_cache;
    uint32_t fat_cache_sector;
    uint8_t fat_cache_valid;
    uint8_t free_count_valid;
    uint32_t next_free_cluster;
    uint32_t free_clusters;
};

struct vfat_dir_pos {
    uint8_t fixed_root;
    uint32_t cluster;
    uint32_t off;
    uint64_t abs;
};

struct vfat_node {
    struct vfat_fs *fs;
    uint32_t first_cluster;
    uint64_t size;
    uint8_t attr;
    uint8_t is_root;
    struct vfat_dir_pos pos;
};

struct vfat_dir_entry {
    char name[256];
    uint8_t attr;
    uint32_t first_cluster;
    uint32_t size;
    uint8_t raw[32];
    struct vfat_dir_pos pos;
    uint64_t lfn_abs;
    uint8_t lfn_slots;
};

struct vfat_lfn_state {
    uint16_t chars[VFAT_MAX_LFN_CHARS + 1];
    uint32_t mask;
    uint64_t first_abs;
    uint8_t checksum;
    uint8_t total;
    uint8_t active;
    uint8_t invalid;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int vfat_readdir(struct vfs_node *parent, uint64_t index, struct vfs_dirent *out);
static struct vfs_node *vfat_finddir(struct vfs_node *parent, const char *name);
static uint64_t vfat_read(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t vfat_write(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf);
static int vfat_create(struct vfs_node *parent, const char *name, uint32_t type, struct vfs_node **out);
static int vfat_unlink(struct vfs_node *parent, const char *name);
static int vfat_rename(struct vfs_node *old_parent, const char *old_name, struct vfs_node *new_parent, const char *new_name);
static int vfat_truncate(struct vfs_node *vnode, uint64_t size);

static int is_pow2(uint32_t v) { return v && !(v & (v - 1)); }
static int ascii_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }
static int ascii_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c; }

static int name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (ascii_tolower((unsigned char)*a) != ascii_tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int mount_flags_rw(const char *flags) {
    if (!flags) return 0;
    for (uint32_t i = 0; flags[i]; i++) {
        int start = i == 0 || flags[i - 1] == ',';
        int end = flags[i + 2] == 0 || flags[i + 2] == ',';
        if (start && end && flags[i] == 'r' && flags[i + 1] == 'w') return 1;
    }
    return 0;
}

static uint64_t fs_off(struct vfat_fs *fs, uint32_t sector) { return (uint64_t)sector * fs->bytes_per_sector; }
static uint64_t cluster_off(struct vfat_fs *fs, uint32_t cluster) { return fs_off(fs, fs->first_data_sector + (cluster - 2u) * fs->sectors_per_cluster); }
static int cluster_valid(struct vfat_fs *fs, uint32_t cluster) { return cluster >= 2 && cluster < fs->cluster_count + 2; }

static int read_bytes(struct vfat_fs *fs, uint64_t off, uint64_t size, void *buf) {
    if (!fs || !buf) return -1;
    if (off > fs->image_bytes || size > fs->image_bytes - off) return -1;
    return block_read(fs->dev, off, size, buf) == size ? 0 : -1;
}

static int write_bytes(struct vfat_fs *fs, uint64_t off, uint64_t size, const void *buf) {
    if (!fs || !buf || fs->readonly) return -1;
    if (off > fs->image_bytes || size > fs->image_bytes - off) return -1;
    return block_write(fs->dev, off, size, buf) == size ? 0 : -1;
}

static int zero_bytes(struct vfat_fs *fs, uint64_t off, uint64_t size) {
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    while (size) {
        uint64_t n = size > sizeof(zero) ? sizeof(zero) : size;
        if (write_bytes(fs, off, n, zero) != 0) return -1;
        off += n; size -= n;
    }
    return 0;
}

static uint32_t fat_eoc_value(struct vfat_fs *fs) { return fs->fat_type == 12 ? 0x0FFFu : (fs->fat_type == 16 ? 0xFFFFu : 0x0FFFFFFFu); }
static int fat_eoc(struct vfat_fs *fs, uint32_t v) { return fs->fat_type == 12 ? v >= VFAT_EOC12 : (fs->fat_type == 16 ? v >= VFAT_EOC16 : v >= VFAT_EOC32); }
static int fat_bad(struct vfat_fs *fs, uint32_t v) { return fs->fat_type == 12 ? v == VFAT_BAD12 : (fs->fat_type == 16 ? v == VFAT_BAD16 : v == VFAT_BAD32); }

static int fat_cache_load(struct vfat_fs *fs, uint32_t sector) {
    if (!fs->fat_cache) return -1;
    if (fs->fat_cache_valid && fs->fat_cache_sector == sector) return 0;
    if (sector >= fs->fat_sectors) return -1;
    if (read_bytes(fs, fs_off(fs, fs->first_fat_sector + sector), fs->bytes_per_sector, fs->fat_cache) != 0) return -1;
    fs->fat_cache_sector = sector;
    fs->fat_cache_valid = 1;
    return 0;
}

static int fat_cached_read(struct vfat_fs *fs, uint64_t fat_byte_off, uint32_t size, uint8_t *out) {
    uint32_t bps = fs->bytes_per_sector;
    for (uint32_t done = 0; done < size;) {
        uint32_t sector = (uint32_t)(fat_byte_off / bps);
        uint32_t off = (uint32_t)(fat_byte_off - (uint64_t)sector * bps);
        uint32_t n = bps - off;
        if (n > size - done) n = size - done;
        if (fat_cache_load(fs, sector) != 0) return -1;
        memcpy(out + done, fs->fat_cache + off, n);
        fat_byte_off += n;
        done += n;
    }
    return 0;
}

static void fat_cache_drop(struct vfat_fs *fs) { fs->fat_cache_valid = 0; }

static uint32_t fat_get_raw(struct vfat_fs *fs, uint32_t cluster) {
    uint8_t raw[4];
    uint64_t off;
    uint32_t v;
    if (cluster >= fs->cluster_count + 2) return UINT32_MAX;
    if (fs->fat_type == 12) {
        off = cluster + cluster / 2u;
        if (fat_cached_read(fs, off, 2, raw) != 0) return UINT32_MAX;
        v = rd16(raw);
        return (cluster & 1u) ? ((v >> 4) & 0x0FFFu) : (v & 0x0FFFu);
    }
    if (fs->fat_type == 16) {
        off = (uint64_t)cluster * 2u;
        if (fat_cached_read(fs, off, 2, raw) != 0) return UINT32_MAX;
        return rd16(raw);
    }
    off = (uint64_t)cluster * 4u;
    if (fat_cached_read(fs, off, 4, raw) != 0) return UINT32_MAX;
    return rd32(raw) & 0x0FFFFFFFu;
}

static uint32_t fat_next(struct vfat_fs *fs, uint32_t cluster) {
    uint32_t v;
    if (!cluster_valid(fs, cluster)) return UINT32_MAX;
    v = fat_get_raw(fs, cluster);
    if (v == UINT32_MAX || fat_bad(fs, v)) return UINT32_MAX;
    if (fat_eoc(fs, v)) return 0;
    if (!cluster_valid(fs, v)) return UINT32_MAX;
    return v;
}

static int fat_set_one(struct vfat_fs *fs, uint32_t fat_index, uint32_t cluster, uint32_t value) {
    uint8_t raw[4];
    uint64_t base = fs_off(fs, fs->first_fat_sector + fat_index * fs->fat_sectors);
    uint64_t off;
    if (cluster >= fs->cluster_count + 2) return -1;
    if (fs->fat_type == 12) {
        off = base + cluster + cluster / 2u;
        if (read_bytes(fs, off, 2, raw) != 0) return -1;
        uint16_t v = rd16(raw);
        if (cluster & 1u) v = (uint16_t)((v & 0x000Fu) | ((value & 0x0FFFu) << 4));
        else v = (uint16_t)((v & 0xF000u) | (value & 0x0FFFu));
        wr16(raw, v);
        return write_bytes(fs, off, 2, raw);
    }
    if (fs->fat_type == 16) {
        off = base + (uint64_t)cluster * 2u;
        wr16(raw, (uint16_t)value);
        return write_bytes(fs, off, 2, raw);
    }
    off = base + (uint64_t)cluster * 4u;
    if (read_bytes(fs, off, 4, raw) != 0) return -1;
    uint32_t old = rd32(raw) & 0xF0000000u;
    wr32(raw, old | (value & 0x0FFFFFFFu));
    return write_bytes(fs, off, 4, raw);
}

static int fat_set(struct vfat_fs *fs, uint32_t cluster, uint32_t value) {
    for (uint32_t i = 0; i < fs->fat_count; i++) if (fat_set_one(fs, i, cluster, value) != 0) return -1;
    fat_cache_drop(fs);
    return 0;
}

static int fat_scan_free_range(struct vfat_fs *fs, uint32_t first, uint32_t last, uint32_t *out) {
    for (uint32_t c = first; c < last; c++) {
        uint32_t v = fat_get_raw(fs, c);
        if (v == UINT32_MAX) return -1;
        if (v == 0) { *out = c; return 0; }
    }
    return 1;
}

static int fat_alloc_cluster(struct vfat_fs *fs, uint32_t *out) {
    if (!out || fs->readonly) return -1;
    if (fs->free_count_valid && !fs->free_clusters) return -1;
    uint32_t end = fs->cluster_count + 2u;
    uint32_t start = cluster_valid(fs, fs->next_free_cluster) ? fs->next_free_cluster : 2u;
    uint32_t c = 0;
    int r = fat_scan_free_range(fs, start, end, &c);
    if (r == 1 && start > 2u) r = fat_scan_free_range(fs, 2u, start, &c);
    if (r != 0) return -1;
    if (fat_set(fs, c, fat_eoc_value(fs)) != 0) return -1;
    if (zero_bytes(fs, cluster_off(fs, c), fs->cluster_bytes) != 0) { fat_set(fs, c, 0); return -1; }
    fs->next_free_cluster = (c + 1u < end) ? c + 1u : 2u;
    if (fs->free_count_valid && fs->free_clusters) fs->free_clusters--;
    *out = c;
    return 0;
}

static void fat_free_chain(struct vfat_fs *fs, uint32_t first) {
    uint32_t c = first;
    for (uint32_t guard = 0; cluster_valid(fs, c) && guard < fs->cluster_count; guard++) {
        uint32_t next = fat_next(fs, c);
        if (fat_set(fs, c, 0) != 0) break;
        if (c < fs->next_free_cluster || !cluster_valid(fs, fs->next_free_cluster)) fs->next_free_cluster = c;
        if (fs->free_count_valid && fs->free_clusters < fs->cluster_count) fs->free_clusters++;
        if (!next || next == UINT32_MAX) break;
        c = next;
    }
}

static int vfat_next_cluster(struct vfat_fs *fs, uint32_t current, int alloc, uint32_t *out) {
    uint32_t next = fat_next(fs, current);
    if (next == UINT32_MAX) return -1;
    if (!next) {
        if (!alloc) return -1;
        if (fat_alloc_cluster(fs, &next) != 0) return -1;
        if (fat_set(fs, current, next) != 0) { fat_set(fs, next, 0); return -1; }
    }
    *out = next;
    return 0;
}

static int chain_cluster_at(struct vfat_node *n, uint32_t index, int alloc, uint32_t *out) {
    struct vfat_fs *fs = n->fs;
    uint32_t c = n->first_cluster;
    if (!c) {
        if (!alloc || fat_alloc_cluster(fs, &c) != 0) return -1;
        n->first_cluster = c;
    }
    if (!cluster_valid(fs, c)) return -1;
    for (uint32_t i = 0; i < index; i++) if (vfat_next_cluster(fs, c, alloc, &c) != 0) return -1;
    *out = c;
    return 0;
}

static uint8_t short_checksum(const uint8_t sfn[11]) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + sfn[i]);
    return sum;
}

static void lfn_reset(struct vfat_lfn_state *st) { memset(st, 0, sizeof(*st)); }

static uint16_t lfn_char_at(const uint8_t *e, uint32_t i) {
    static const uint8_t off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    return rd16(e + off[i]);
}

static void lfn_take(struct vfat_lfn_state *st, const uint8_t *e, uint64_t abs) {
    uint8_t ord = e[0] & 0x1Fu;
    uint8_t sum = e[13];
    if (!ord || ord > VFAT_MAX_LFN_SLOTS || e[12] != 0 || rd16(e + 26) != 0) { lfn_reset(st); st->invalid = 1; return; }
    if (e[0] & 0x40u) {
        lfn_reset(st);
        st->active = 1; st->total = ord; st->checksum = sum; st->first_abs = abs;
    } else if (!st->active || st->checksum != sum || ord >= st->total || abs != st->first_abs + (uint64_t)(st->total - ord) * 32u) {
        lfn_reset(st); st->invalid = 1; return;
    }
    uint32_t base = (ord - 1u) * 13u;
    for (uint32_t i = 0; i < 13 && base + i < VFAT_MAX_LFN_CHARS; i++) st->chars[base + i] = lfn_char_at(e, i);
    st->mask |= 1u << (ord - 1u);
}

static int utf16_to_utf8(const uint16_t *in, char out[256]) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < VFAT_MAX_LFN_CHARS; i++) {
        uint32_t c = in[i];
        if (c == 0x0000u || c == 0xFFFFu) break;
        if (c >= 0xD800u && c <= 0xDFFFu) return -1;
        if (c < 0x80u) { if (o + 1 >= 256) return -1; out[o++] = (char)c; }
        else if (c < 0x800u) { if (o + 2 >= 256) return -1; out[o++] = (char)(0xC0u | (c >> 6)); out[o++] = (char)(0x80u | (c & 0x3Fu)); }
        else { if (o + 3 >= 256) return -1; out[o++] = (char)(0xE0u | (c >> 12)); out[o++] = (char)(0x80u | ((c >> 6) & 0x3Fu)); out[o++] = (char)(0x80u | (c & 0x3Fu)); }
    }
    if (!o) return -1;
    out[o] = 0;
    return 0;
}

static int lfn_emit(struct vfat_lfn_state *st, const uint8_t *sfn, char out[256]) {
    if (!st->active || st->invalid || st->checksum != short_checksum(sfn)) return -1;
    if (st->total == 0 || st->total > VFAT_MAX_LFN_SLOTS) return -1;
    uint32_t need = (1u << st->total) - 1u;
    if ((st->mask & need) != need) return -1;
    return utf16_to_utf8(st->chars, out);
}

static void short_name(const uint8_t *e, char out[256]) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < 8 && e[i] != ' '; i++) out[o++] = (char)e[i];
    if (e[8] != ' ') {
        out[o++] = '.';
        for (uint32_t i = 8; i < 11 && e[i] != ' '; i++) out[o++] = (char)e[i];
    }
    out[o] = 0;
    if (e[12] & 0x08u) for (uint32_t i = 0; out[i] && out[i] != '.'; i++) out[i] = (char)ascii_tolower(out[i]);
    if (e[12] & 0x10u) { char *dot = strchr(out, '.'); if (dot) for (uint32_t i = 1; dot[i]; i++) dot[i] = (char)ascii_tolower(dot[i]); }
}

static void decode_entry(struct vfat_dir_entry *out, const uint8_t *e, struct vfat_lfn_state *lfn, struct vfat_dir_pos pos) {
    memset(out, 0, sizeof(*out));
    out->attr = e[11];
    out->first_cluster = (((uint32_t)rd16(e + 20) << 16) | rd16(e + 26)) & 0x0FFFFFFFu;
    out->size = rd32(e + 28);
    memcpy(out->raw, e, 32);
    out->pos = pos;
    out->lfn_abs = lfn->active ? lfn->first_abs : pos.abs;
    out->lfn_slots = lfn->active ? lfn->total : 0;
    if (lfn_emit(lfn, e, out->name) != 0) short_name(e, out->name);
    lfn_reset(lfn);
}

static int entry_usable(const uint8_t *e) {
    if (e[0] == 0x00u || e[0] == 0xE5u) return 0;
    if (e[11] == VFAT_ATTR_LFN) return 0;
    if ((e[11] & VFAT_ATTR_LABEL) && !(e[11] & VFAT_ATTR_DIR)) return 0;
    return 1;
}
static int dot_name(const char *s) { return strcmp(s, ".") == 0 || strcmp(s, "..") == 0; }

static int scan_entries(const uint8_t *buf, uint32_t bytes, struct vfat_dir_pos base, const char *want, uint64_t want_index, uint64_t *seen, struct vfat_dir_entry *out, struct vfat_lfn_state *lfn) {
    for (uint32_t off = 0; off + VFAT_DIR_ENTRY_SIZE <= bytes; off += VFAT_DIR_ENTRY_SIZE) {
        const uint8_t *e = buf + off;
        struct vfat_dir_pos pos = base;
        pos.off = base.off + off;
        pos.abs = base.abs + off;
        if (e[0] == 0x00u) return 1;
        if (e[0] == 0xE5u) { lfn_reset(lfn); continue; }
        if (e[11] == VFAT_ATTR_LFN) { lfn_take(lfn, e, pos.abs); continue; }
        if (!entry_usable(e)) { lfn_reset(lfn); continue; }
        struct vfat_dir_entry de;
        decode_entry(&de, e, lfn, pos);
        if (!de.name[0] || dot_name(de.name)) continue;
        if (want) { if (name_eq(de.name, want)) { *out = de; return 2; } }
        else { if (*seen == want_index) { *out = de; return 2; } (*seen)++; }
    }
    return 0;
}

static int scan_dir(struct vfat_node *dir, const char *want, uint64_t index, struct vfat_dir_entry *out) {
    struct vfat_fs *fs = dir->fs;
    uint64_t seen = 0;
    if (dir->is_root && fs->fat_type != 32) {
        uint32_t bytes = fs->root_dir_sectors * fs->bytes_per_sector;
        uint8_t *buf = (uint8_t *)kmalloc(bytes);
        if (!buf) return -1;
        if (read_bytes(fs, fs_off(fs, fs->root_dir_sector), bytes, buf) != 0) { kfree(buf); return -1; }
        struct vfat_lfn_state lfn; lfn_reset(&lfn);
        struct vfat_dir_pos base = {1, 0, 0, fs_off(fs, fs->root_dir_sector)};
        int r = scan_entries(buf, bytes, base, want, index, &seen, out, &lfn);
        kfree(buf);
        return r == 2 ? 0 : -1;
    }
    uint32_t cluster = dir->is_root ? fs->root_cluster : dir->first_cluster;
    if (!cluster_valid(fs, cluster)) return -1;
    uint8_t *buf = (uint8_t *)kmalloc(fs->cluster_bytes);
    if (!buf) return -1;
    struct vfat_lfn_state lfn; lfn_reset(&lfn);
    for (uint32_t guard = 0; guard < fs->cluster_count; guard++) {
        if (read_bytes(fs, cluster_off(fs, cluster), fs->cluster_bytes, buf) != 0) { kfree(buf); return -1; }
        struct vfat_dir_pos base = {0, cluster, 0, cluster_off(fs, cluster)};
        int r = scan_entries(buf, fs->cluster_bytes, base, want, index, &seen, out, &lfn);
        if (r == 2) { kfree(buf); return 0; }
        if (r == 1) { kfree(buf); return -1; }
        uint32_t next = fat_next(fs, cluster);
        if (next == UINT32_MAX) { kfree(buf); return -1; }
        if (!next) break;
        cluster = next;
    }
    kfree(buf);
    return -1;
}

static int dir_empty(struct vfat_node *dir) {
    struct vfat_dir_entry de;
    return scan_dir(dir, 0, 0, &de) != 0;
}

static void attach_ops(struct vfs_node *vnode, uint32_t type) {
    if (type == VFS_NODE_DIRECTORY) {
        vfs_set_finddir(vnode, vfat_finddir);
        vfs_set_readdir(vnode, vfat_readdir);
        vfs_set_create(vnode, vfat_create);
        vfs_set_unlink(vnode, vfat_unlink);
        vfs_set_rename(vnode, vfat_rename);
    } else {
        vfs_set_read(vnode, vfat_read);
        vfs_set_write(vnode, vfat_write);
        vfs_set_truncate(vnode, vfat_truncate);
    }
}

static struct vfs_node *make_vnode(struct vfat_fs *fs, const struct vfat_dir_entry *de) {
    struct vfat_node *n = (struct vfat_node *)kmalloc(sizeof(*n));
    if (!n) return 0;
    memset(n, 0, sizeof(*n));
    n->fs = fs; n->first_cluster = de->first_cluster; n->size = de->size; n->attr = de->attr; n->pos = de->pos;
    uint32_t type = (de->attr & VFAT_ATTR_DIR) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    struct vfs_node *v = vfs_create_fs_node(de->name, type, de->first_cluster ? de->first_cluster : VFAT_ROOT_INO, type == VFS_NODE_DIRECTORY ? 0 : de->size, n);
    if (!v) { kfree(n); return 0; }
    attach_ops(v, type);
    return v;
}

static struct vfs_node *vfat_finddir(struct vfs_node *parent, const char *name) {
    struct vfat_node *dir = (struct vfat_node *)vfs_get_fs_data(parent);
    if (!dir || !name || !(dir->attr & VFAT_ATTR_DIR)) return 0;
    struct vfat_dir_entry de;
    if (scan_dir(dir, name, 0, &de) != 0) return 0;
    struct vfs_node *v = make_vnode(dir->fs, &de);
    if (v) vfs_add_child(parent, v);
    return v;
}

static int vfat_readdir(struct vfs_node *parent, uint64_t index, struct vfs_dirent *out) {
    struct vfat_node *dir = (struct vfat_node *)vfs_get_fs_data(parent);
    if (!dir || !out || !(dir->attr & VFAT_ATTR_DIR)) return -1;
    struct vfat_dir_entry de;
    if (scan_dir(dir, 0, index, &de) != 0) return -1;
    char *dst = out->name;
    uint64_t cap = out->name_capacity;
    uint64_t len = strlen(de.name);
    memset(out, 0, sizeof(*out));
    out->inode = de.first_cluster ? de.first_cluster : VFAT_ROOT_INO;
    out->type = (de.attr & VFAT_ATTR_DIR) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    out->name_len = len;
    out->name = dst;
    out->name_capacity = cap;
    if (dst && cap > len) memcpy(dst, de.name, len + 1);
    return 0;
}


static int update_entry(struct vfat_node *n) {
    uint8_t e[32];
    if (n->is_root) return 0;
    if (read_bytes(n->fs, n->pos.abs, sizeof(e), e) != 0) return -1;
    if (e[0] == 0x00u || e[0] == 0xE5u || e[11] == VFAT_ATTR_LFN) return -1;
    wr16(e + 20, (uint16_t)((n->first_cluster >> 16) & 0xFFFFu));
    wr16(e + 26, (uint16_t)(n->first_cluster & 0xFFFFu));
    wr32(e + 28, (n->attr & VFAT_ATTR_DIR) ? 0 : (uint32_t)n->size);
    return write_bytes(n->fs, n->pos.abs, sizeof(e), e);
}

static uint64_t vfat_read(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf) {
    struct vfat_node *n = (struct vfat_node *)vfs_get_fs_data(vnode);
    if (!n || !buf || (n->attr & VFAT_ATTR_DIR)) return 0;
    if (off >= n->size) return 0;
    if (size > n->size - off) size = n->size - off;
    if (!size) return 0;
    struct vfat_fs *fs = n->fs;
    if (!n->first_cluster) return 0;
    uint32_t cluster_index = (uint32_t)(off / fs->cluster_bytes);
    uint32_t cluster;
    if (chain_cluster_at(n, cluster_index, 0, &cluster) != 0) return 0;
    uint64_t done = 0;
    uint64_t in_cluster = off % fs->cluster_bytes;
    while (done < size) {
        uint64_t take = fs->cluster_bytes - in_cluster;
        if (take > size - done) take = size - done;
        if (read_bytes(fs, cluster_off(fs, cluster) + in_cluster, take, buf + done) != 0) break;
        done += take;
        in_cluster = 0;
        if (done < size && vfat_next_cluster(fs, cluster, 0, &cluster) != 0) break;
    }
    return done;
}

static uint64_t vfat_write(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf) {
    struct vfat_node *n = (struct vfat_node *)vfs_get_fs_data(vnode);
    if (!n || !buf || (n->attr & VFAT_ATTR_DIR) || n->fs->readonly) return 0;
    if (off > 0xFFFFFFFFULL || size > 0xFFFFFFFFULL - off) return 0;
    struct vfat_fs *fs = n->fs;
    uint32_t cluster_index = (uint32_t)(off / fs->cluster_bytes);
    uint32_t cluster;
    if (chain_cluster_at(n, cluster_index, 1, &cluster) != 0) return 0;
    uint64_t done = 0;
    uint64_t in_cluster = off % fs->cluster_bytes;
    while (done < size) {
        uint64_t take = fs->cluster_bytes - in_cluster;
        if (take > size - done) take = size - done;
        if (write_bytes(fs, cluster_off(fs, cluster) + in_cluster, take, buf + done) != 0) break;
        done += take;
        in_cluster = 0;
        if (done < size && vfat_next_cluster(fs, cluster, 1, &cluster) != 0) break;
    }
    if (done && off + done > n->size) {
        n->size = off + done;
        vfs_set_size(vnode, n->size);
        if (update_entry(n) != 0) return 0;
    } else if (done && update_entry(n) != 0) return 0;
    return done;
}

static int trim_chain(struct vfat_node *n, uint32_t keep) {
    struct vfat_fs *fs = n->fs;
    if (keep == 0) {
        if (n->first_cluster) fat_free_chain(fs, n->first_cluster);
        n->first_cluster = 0;
        return 0;
    }
    uint32_t c;
    if (chain_cluster_at(n, keep - 1u, 0, &c) != 0) return -1;
    uint32_t next = fat_next(fs, c);
    if (fat_set(fs, c, fat_eoc_value(fs)) != 0) return -1;
    if (next && next != UINT32_MAX) fat_free_chain(fs, next);
    return 0;
}

static int vfat_truncate(struct vfs_node *vnode, uint64_t size) {
    struct vfat_node *n = (struct vfat_node *)vfs_get_fs_data(vnode);
    if (!n || (n->attr & VFAT_ATTR_DIR) || n->fs->readonly || size > 0xFFFFFFFFULL) return -1;
    uint32_t need = size ? (uint32_t)((size + n->fs->cluster_bytes - 1u) / n->fs->cluster_bytes) : 0;
    if (need) { uint32_t dummy; if (chain_cluster_at(n, need - 1u, 1, &dummy) != 0) return -1; }
    if (trim_chain(n, need) != 0) return -1;
    n->size = size;
    vfs_set_size(vnode, size);
    return update_entry(n);
}

static int invalid_lfn_char(unsigned char c) {
    if (c < 0x20) return 1;
    return c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|';
}

static int valid_name(const char *name) {
    size_t len = strlen(name);
    if (!len || len > VFAT_MAX_LFN_CHARS || dot_name(name)) return 0;
    if (name[len - 1] == ' ' || name[len - 1] == '.') return 0;
    for (size_t i = 0; i < len; i++) if (invalid_lfn_char((unsigned char)name[i])) return 0;
    return 1;
}

static const char *last_dot(const char *s) {
    const char *dot = 0;
    for (; *s; s++) if (*s == '.') dot = s;
    return dot;
}

static int sfn_valid_char(int c) {
    if (c <= 0x20 || c == 0x7F) return 0;
    if (c == '"' || c == '*' || c == '+' || c == ',' || c == '.' || c == '/' || c == ':' || c == ';' || c == '<' || c == '=' || c == '>' || c == '?' || c == '[' || c == '\\' || c == ']' || c == '|') return 0;
    return 1;
}

static int make_exact_sfn(const char *name, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char *dot = last_dot(name);
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext_len = dot ? strlen(dot + 1) : 0;
    if (!base_len || base_len > 8 || ext_len > 3) return -1;
    for (size_t i = 0; i < base_len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'a' && c <= 'z') return -1;
        if (!sfn_valid_char(c)) return -1;
        out[i] = (uint8_t)c;
    }
    for (size_t i = 0; i < ext_len; i++) {
        unsigned char c = (unsigned char)dot[1 + i];
        if (c >= 'a' && c <= 'z') return -1;
        if (!sfn_valid_char(c)) return -1;
        out[8 + i] = (uint8_t)c;
    }
    return 0;
}

static void sanitize_part(const char *s, size_t len, uint8_t *out, size_t outlen) {
    size_t o = 0;
    for (size_t i = 0; i < len && o < outlen; i++) {
        unsigned char c = (unsigned char)ascii_toupper((unsigned char)s[i]);
        if (c == ' ') c = '_';
        if (!sfn_valid_char(c)) c = '_';
        out[o++] = (uint8_t)c;
    }
    while (o < outlen) out[o++] = ' ';
}

static int sfn_exists(struct vfat_node *dir, const uint8_t sfn[11]) {
    struct vfat_fs *fs = dir->fs;
    uint32_t bytes;
    if (dir->is_root && fs->fat_type != 32) {
        bytes = fs->root_dir_sectors * fs->bytes_per_sector;
        uint8_t *buf = (uint8_t *)kmalloc(bytes);
        if (!buf) return 1;
        if (read_bytes(fs, fs_off(fs, fs->root_dir_sector), bytes, buf) != 0) { kfree(buf); return 1; }
        for (uint32_t off = 0; off + 32 <= bytes; off += 32) { uint8_t *e = buf + off; if (e[0] == 0x00u) break; if (e[0] != 0xE5u && e[11] != VFAT_ATTR_LFN && memcmp(e, sfn, 11) == 0) { kfree(buf); return 1; } }
        kfree(buf); return 0;
    }
    uint8_t *buf = (uint8_t *)kmalloc(fs->cluster_bytes);
    if (!buf) return 1;
    uint32_t cluster = dir->is_root ? fs->root_cluster : dir->first_cluster;
    for (uint32_t guard = 0; cluster_valid(fs, cluster) && guard < fs->cluster_count; guard++) {
        if (read_bytes(fs, cluster_off(fs, cluster), fs->cluster_bytes, buf) != 0) { kfree(buf); return 1; }
        for (uint32_t off = 0; off + 32 <= fs->cluster_bytes; off += 32) { uint8_t *e = buf + off; if (e[0] == 0x00u) { kfree(buf); return 0; } if (e[0] != 0xE5u && e[11] != VFAT_ATTR_LFN && memcmp(e, sfn, 11) == 0) { kfree(buf); return 1; } }
        uint32_t next = fat_next(fs, cluster); if (!next || next == UINT32_MAX) break; cluster = next;
    }
    kfree(buf); return 0;
}

static int make_sfn(struct vfat_node *dir, const char *name, uint8_t out[11], int *need_lfn) {
    if (make_exact_sfn(name, out) == 0 && !sfn_exists(dir, out)) { *need_lfn = 0; return 0; }
    *need_lfn = 1;
    const char *dot = last_dot(name);
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext_len = dot ? strlen(dot + 1) : 0;
    uint8_t base[6], ext[3];
    sanitize_part(name, base_len, base, sizeof(base));
    sanitize_part(dot ? dot + 1 : "", ext_len, ext, sizeof(ext));
    for (uint32_t n = 1; n < 1000000u; n++) {
        memset(out, ' ', 11);
        char tail[8];
        uint32_t t = n; uint32_t m = 0; char rev[8];
        do { rev[m++] = (char)('0' + (t % 10)); t /= 10; } while (t && m < sizeof(rev));
        if (m + 1 > 6) continue;
        uint32_t keep = 8u - (m + 1u); if (keep > 6) keep = 6;
        memcpy(out, base, keep); out[keep] = '~';
        for (uint32_t i = 0; i < m; i++) tail[i] = rev[m - 1u - i];
        memcpy(out + keep + 1u, tail, m);
        memcpy(out + 8, ext, 3);
        if (!sfn_exists(dir, out)) return 0;
    }
    return -1;
}

static int utf8_to_utf16_units(const char *name, uint16_t units[VFAT_MAX_LFN_CHARS + 1], uint32_t *count) {
    uint32_t n = 0;
    for (size_t i = 0; name[i]; ) {
        unsigned char c = (unsigned char)name[i++];
        uint32_t cp;
        if (c < 0x80) cp = c;
        else if ((c & 0xE0u) == 0xC0u) { unsigned char c2 = (unsigned char)name[i++]; if ((c2 & 0xC0u) != 0x80u) return -1; cp = ((c & 0x1Fu) << 6) | (c2 & 0x3Fu); if (cp < 0x80u) return -1; }
        else if ((c & 0xF0u) == 0xE0u) { unsigned char c2 = (unsigned char)name[i++], c3 = (unsigned char)name[i++]; if ((c2 & 0xC0u) != 0x80u || (c3 & 0xC0u) != 0x80u) return -1; cp = ((c & 0x0Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu); if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) return -1; }
        else return -1;
        if (cp > 0xFFFFu || n >= VFAT_MAX_LFN_CHARS) return -1;
        units[n++] = (uint16_t)cp;
    }
    units[n] = 0;
    *count = n;
    return n ? 0 : -1;
}

static void build_lfn_slot(uint8_t e[32], uint8_t ord, uint8_t total, uint8_t sum, const uint16_t *units) {
    static const uint8_t pos[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    memset(e, 0xFF, 32);
    e[0] = ord | (ord == total ? 0x40u : 0u); e[11] = VFAT_ATTR_LFN; e[12] = 0; e[13] = sum; wr16(e + 26, 0);
    for (uint32_t i = 0; i < 13; i++) {
        uint32_t idx = (ord - 1u) * 13u + i;
        uint16_t ch = units[idx];
        if (ch == 0) { wr16(e + pos[i], 0); for (i++; i < 13; i++) wr16(e + pos[i], 0xFFFFu); break; }
        wr16(e + pos[i], ch);
    }
}

static int build_name_entries(struct vfat_node *dir, const char *name, uint8_t attr, uint32_t first_cluster, uint32_t size, uint8_t **entries_out, uint32_t *bytes_out) {
    if (!valid_name(name) || !entries_out || !bytes_out) return -1;
    uint8_t sfn[11]; int need_lfn = 0;
    if (make_sfn(dir, name, sfn, &need_lfn) != 0) return -1;
    uint16_t units[VFAT_MAX_LFN_CHARS + 1]; uint32_t chars = 0;
    uint8_t slots = 0;
    if (need_lfn) {
        if (utf8_to_utf16_units(name, units, &chars) != 0) return -1;
        slots = (uint8_t)((chars + 1u + 12u) / 13u);
        if (!slots || slots > VFAT_MAX_LFN_SLOTS) return -1;
    }
    uint32_t total = ((uint32_t)slots + 1u) * 32u;
    uint8_t *entries = (uint8_t *)kmalloc(total);
    if (!entries) return -1;
    memset(entries, 0, total);
    uint8_t sum = short_checksum(sfn);
    for (uint8_t i = 0; i < slots; i++) build_lfn_slot(entries + i * 32u, (uint8_t)(slots - i), slots, sum, units);
    uint8_t *e = entries + slots * 32u;
    memcpy(e, sfn, 11); e[11] = attr; e[12] = 0;
    wr16(e + 20, (uint16_t)((first_cluster >> 16) & 0xFFFFu));
    wr16(e + 26, (uint16_t)(first_cluster & 0xFFFFu));
    wr32(e + 28, size);
    *entries_out = entries; *bytes_out = total;
    return 0;
}

static int dir_find_free_fixed(struct vfat_fs *fs, uint32_t need, struct vfat_dir_pos *pos) {
    uint32_t bytes = fs->root_dir_sectors * fs->bytes_per_sector;
    uint8_t *buf = (uint8_t *)kmalloc(bytes);
    if (!buf) return -1;
    if (read_bytes(fs, fs_off(fs, fs->root_dir_sector), bytes, buf) != 0) { kfree(buf); return -1; }
    uint32_t run = 0, start = 0;
    for (uint32_t off = 0; off + 32 <= bytes; off += 32) {
        if (buf[off] == 0x00u || buf[off] == 0xE5u) { if (!run) start = off; run++; if (run == need) { pos->fixed_root = 1; pos->cluster = 0; pos->off = start; pos->abs = fs_off(fs, fs->root_dir_sector) + start; kfree(buf); return 0; } }
        else run = 0;
    }
    kfree(buf); return -1;
}

static int dir_find_free_chain(struct vfat_node *dir, uint32_t need, struct vfat_dir_pos *pos) {
    struct vfat_fs *fs = dir->fs;
    uint32_t cluster = dir->is_root ? fs->root_cluster : dir->first_cluster;
    uint8_t *buf = (uint8_t *)kmalloc(fs->cluster_bytes);
    if (!buf) return -1;
    uint32_t prev = 0;
    for (uint32_t guard = 0; guard < fs->cluster_count; guard++) {
        if (!cluster_valid(fs, cluster)) { kfree(buf); return -1; }
        if (read_bytes(fs, cluster_off(fs, cluster), fs->cluster_bytes, buf) != 0) { kfree(buf); return -1; }
        uint32_t run = 0, start = 0;
        for (uint32_t off = 0; off + 32 <= fs->cluster_bytes; off += 32) {
            if (buf[off] == 0x00u || buf[off] == 0xE5u) { if (!run) start = off; run++; if (run == need) { pos->fixed_root = 0; pos->cluster = cluster; pos->off = start; pos->abs = cluster_off(fs, cluster) + start; kfree(buf); return 0; } }
            else run = 0;
        }
        prev = cluster;
        uint32_t next = fat_next(fs, cluster);
        if (next == UINT32_MAX) { kfree(buf); return -1; }
        if (!next) break;
        cluster = next;
    }
    uint32_t nc;
    if (fat_alloc_cluster(fs, &nc) != 0) { kfree(buf); return -1; }
    if (prev && fat_set(fs, prev, nc) != 0) { fat_set(fs, nc, 0); kfree(buf); return -1; }
    if (!prev && !dir->is_root) dir->first_cluster = nc;
    pos->fixed_root = 0; pos->cluster = nc; pos->off = 0; pos->abs = cluster_off(fs, nc);
    kfree(buf); return 0;
}

static int dir_write_entries(struct vfat_node *dir, const uint8_t *entries, uint32_t bytes, struct vfat_dir_pos *out) {
    if (!dir || !entries || !bytes || bytes % 32) return -1;
    uint32_t need = bytes / 32u;
    struct vfat_dir_pos pos;
    if (dir->is_root && dir->fs->fat_type != 32) { if (dir_find_free_fixed(dir->fs, need, &pos) != 0) return -1; }
    else if (dir_find_free_chain(dir, need, &pos) != 0) return -1;
    if (write_bytes(dir->fs, pos.abs, bytes, entries) != 0) return -1;
    if (out) *out = pos;
    return 0;
}

static int dir_mark_deleted(struct vfat_fs *fs, uint64_t abs, uint8_t slots) {
    uint8_t e5 = 0xE5;
    for (uint32_t i = 0; i < (uint32_t)slots + 1u; i++) if (write_bytes(fs, abs + i * 32u, 1, &e5) != 0) return -1;
    return 0;
}

static int vfat_create(struct vfs_node *parent, const char *name, uint32_t type, struct vfs_node **out) {
    struct vfat_node *dir = (struct vfat_node *)vfs_get_fs_data(parent);
    if (!dir || !(dir->attr & VFAT_ATTR_DIR) || dir->fs->readonly || type != VFS_NODE_FILE) return -1;
    struct vfat_dir_entry exists;
    memset(&exists, 0, sizeof(exists));
    if (scan_dir(dir, name, 0, &exists) == 0) return -1;
    uint8_t *entries = 0; uint32_t bytes = 0;
    if (build_name_entries(dir, name, VFAT_ATTR_ARCHIVE, 0, 0, &entries, &bytes) != 0) return -1;
    struct vfat_dir_pos pos;
    int rc = dir_write_entries(dir, entries, bytes, &pos);
    kfree(entries);
    if (rc != 0) return -1;
    struct vfat_dir_entry de;
    if (scan_dir(dir, name, 0, &de) != 0) return -1;
    struct vfs_node *v = make_vnode(dir->fs, &de);
    if (!v) return -1;
    vfs_add_child(parent, v);
    if (out) *out = v;
    return 0;
}

static int vfat_unlink(struct vfs_node *parent, const char *name) {
    struct vfat_node *dir = (struct vfat_node *)vfs_get_fs_data(parent);
    if (!dir || !(dir->attr & VFAT_ATTR_DIR) || dir->fs->readonly) return -1;
    struct vfat_dir_entry de;
    if (scan_dir(dir, name, 0, &de) != 0) return -1;
    if (de.attr & VFAT_ATTR_DIR) {
        struct vfat_node tmp; memset(&tmp, 0, sizeof(tmp)); tmp.fs = dir->fs; tmp.first_cluster = de.first_cluster; tmp.attr = de.attr;
        if (!dir_empty(&tmp)) return -1;
    }
    uint64_t first_abs = de.lfn_slots ? de.lfn_abs : de.pos.abs;
    if (dir_mark_deleted(dir->fs, first_abs, de.lfn_slots) != 0) return -1;
    if (de.first_cluster) fat_free_chain(dir->fs, de.first_cluster);
    return 0;
}

static int vfat_rename(struct vfs_node *old_parent, const char *old_name, struct vfs_node *new_parent, const char *new_name) {
    struct vfat_node *odir = (struct vfat_node *)vfs_get_fs_data(old_parent);
    struct vfat_node *ndir = (struct vfat_node *)vfs_get_fs_data(new_parent);
    if (!odir || !ndir || !(odir->attr & VFAT_ATTR_DIR) || !(ndir->attr & VFAT_ATTR_DIR) || odir->fs != ndir->fs || odir->fs->readonly) return -1;
    struct vfat_dir_entry olde;
    if (scan_dir(odir, old_name, 0, &olde) != 0) return -1;
    struct vfat_dir_entry exists;
    memset(&exists, 0, sizeof(exists));
    if (scan_dir(ndir, new_name, 0, &exists) == 0) return -1;
    if (!valid_name(new_name)) return -1;
    uint8_t *entries = 0; uint32_t bytes = 0;
    uint32_t size = (olde.attr & VFAT_ATTR_DIR) ? 0 : olde.size;
    if (build_name_entries(ndir, new_name, olde.attr, olde.first_cluster, size, &entries, &bytes) != 0) return -1;
    struct vfat_dir_pos newpos;
    if (dir_write_entries(ndir, entries, bytes, &newpos) != 0) { kfree(entries); return -1; }
    kfree(entries);
    uint64_t old_abs = olde.lfn_slots ? olde.lfn_abs : olde.pos.abs;
    if (dir_mark_deleted(odir->fs, old_abs, olde.lfn_slots) != 0) return -1;
    return 0;
}

static int parse_bpb(struct vfat_fs *fs, const uint8_t bs[512]) {
    if (bs[510] != 0x55u || bs[511] != 0xAAu) return -1;
    fs->bytes_per_sector = rd16(bs + 11);
    fs->sectors_per_cluster = bs[13];
    fs->reserved_sectors = rd16(bs + 14);
    fs->fat_count = bs[16];
    fs->root_entry_count = rd16(bs + 17);
    uint32_t total16 = rd16(bs + 19);
    uint32_t fatsz16 = rd16(bs + 22);
    fs->total_sectors = total16 ? total16 : rd32(bs + 32);
    fs->fat_sectors = fatsz16 ? fatsz16 : rd32(bs + 36);
    fs->root_cluster = rd32(bs + 44);
    if (!is_pow2(fs->bytes_per_sector) || fs->bytes_per_sector < 512 || fs->bytes_per_sector > 4096) return -1;
    if (!is_pow2(fs->sectors_per_cluster) || fs->sectors_per_cluster > 128) return -1;
    if (!fs->reserved_sectors || !fs->fat_count || fs->fat_count > 2 || !fs->fat_sectors || !fs->total_sectors) return -1;
    fs->root_dir_sectors = ((uint32_t)fs->root_entry_count * 32u + fs->bytes_per_sector - 1u) / fs->bytes_per_sector;
    uint64_t overhead = (uint64_t)fs->reserved_sectors + (uint64_t)fs->fat_count * fs->fat_sectors + fs->root_dir_sectors;
    if (overhead >= fs->total_sectors) return -1;
    uint32_t data_sectors = fs->total_sectors - (uint32_t)overhead;
    fs->cluster_count = data_sectors / fs->sectors_per_cluster;
    if (fs->cluster_count < 1) return -1;
    if (fs->cluster_count < 4085u) fs->fat_type = 12;
    else if (fs->cluster_count < 65525u) fs->fat_type = 16;
    else fs->fat_type = 32;
    if (fs->fat_type == 32) {
        if (fatsz16 || fs->root_entry_count || rd16(bs + 42) != 0 || !cluster_valid(fs, fs->root_cluster)) return -1;
    } else {
        if (!fatsz16 || !fs->root_entry_count) return -1;
        fs->root_cluster = 0;
    }
    fs->first_fat_sector = fs->reserved_sectors;
    fs->root_dir_sector = fs->reserved_sectors + (uint32_t)fs->fat_count * fs->fat_sectors;
    fs->first_data_sector = fs->root_dir_sector + fs->root_dir_sectors;
    fs->cluster_bytes = (uint32_t)fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->image_bytes = (uint64_t)fs->total_sectors * fs->bytes_per_sector;
    if (fs->fat_type == 12 && ((uint64_t)fs->cluster_count * 3u + 1u) / 2u > (uint64_t)fs->fat_sectors * fs->bytes_per_sector) return -1;
    if (fs->fat_type == 16 && (uint64_t)(fs->cluster_count + 2u) * 2u > (uint64_t)fs->fat_sectors * fs->bytes_per_sector) return -1;
    if (fs->fat_type == 32 && (uint64_t)(fs->cluster_count + 2u) * 4u > (uint64_t)fs->fat_sectors * fs->bytes_per_sector) return -1;
    return 0;
}

static int vfat_probe_bpb(struct vfs_node *blockdev, const uint8_t bs[512]) {
    if (bs[510] != 0x55u || bs[511] != 0xAAu) return FS_PROBE_NO;
    struct vfat_fs fs;
    memset(&fs, 0, sizeof(fs));
    fs.dev = blockdev;
    if (parse_bpb(&fs, bs) != 0) return FS_PROBE_ERR;
    uint64_t dev_size = block_size(blockdev);
    if (dev_size && fs.image_bytes > dev_size) return FS_PROBE_ERR;
    return FS_PROBE_YES;
}

static int vfat_probe(struct vfs_node *blockdev) {
    uint8_t bs[512];
    if (block_read(blockdev, 0, sizeof(bs), bs) != sizeof(bs)) return FS_PROBE_ERR;
    return vfat_probe_bpb(blockdev, bs);
}

static int vfat_mount(struct vfs_node *blockdev, struct vfs_node *mountpoint, const char *flags) {
    uint8_t bs[512];
    if (block_read(blockdev, 0, sizeof(bs), bs) != sizeof(bs)) return -1;
    int probe = vfat_probe_bpb(blockdev, bs);
    if (probe == FS_PROBE_NO) return -2;
    if (probe != FS_PROBE_YES) return -3;
    struct vfat_fs *fs = (struct vfat_fs *)kmalloc(sizeof(*fs));
    if (!fs) return -1;
    memset(fs, 0, sizeof(*fs));
    fs->dev = blockdev;
    fs->readonly = !mount_flags_rw(flags);
    if (parse_bpb(fs, bs) != 0) { kfree(fs); return -2; }
    fs->fat_cache = (uint8_t *)kmalloc(fs->bytes_per_sector);
    if (!fs->fat_cache) { kfree(fs); return -4; }
    fs->next_free_cluster = 2;
    uint64_t dev_size = block_size(blockdev);
    if (dev_size && fs->image_bytes > dev_size) { kfree(fs->fat_cache); kfree(fs); return -3; }
    uint32_t first_data = 0;
    if (fs->fat_type == 32) first_data = fs->root_cluster;
    struct vfat_node *root = (struct vfat_node *)kmalloc(sizeof(*root));
    if (!root) { kfree(fs->fat_cache); kfree(fs); return -4; }
    memset(root, 0, sizeof(*root));
    root->fs = fs;
    root->first_cluster = first_data;
    root->attr = VFAT_ATTR_DIR;
    root->is_root = 1;
    vfs_set_fs_data(mountpoint, root);
    attach_ops(mountpoint, VFS_NODE_DIRECTORY);
    vfs_set_size(mountpoint, 0);
    klog(LOG_INFO, "vfat: mounted FAT%d bps=%d spc=%d clusters=%d %s\n", fs->fat_type, fs->bytes_per_sector, fs->sectors_per_cluster, fs->cluster_count, fs->readonly ? "ro" : "rw");
    return 0;
}

static void vfat_copy_text(char *dst, uint64_t cap, const char *src) {
    if (!dst || !cap) return;
    uint64_t i = 0;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int vfat_info(struct vfs_node *mountpoint, struct fs_info *out) {
    if (!mountpoint || !out) return -1;
    struct vfat_node *root = (struct vfat_node *)vfs_get_fs_data(mountpoint);
    if (!root || !root->fs) return -1;
    struct vfat_fs *fs = root->fs;
    memset(out, 0, sizeof(*out));
    out->version = FS_INFO_VERSION;
    out->block_size = fs->cluster_bytes;
    out->total_bytes = (uint64_t)fs->cluster_count * fs->cluster_bytes;
    out->max_file_size = 0xFFFFFFFFULL;
    if (fs->readonly) out->flags |= FS_INFO_FLAG_READONLY;
    vfat_copy_text(out->driver, sizeof(out->driver), "vfat");
    if (fs->fat_type == 12) vfat_copy_text(out->type, sizeof(out->type), "FAT12");
    else if (fs->fat_type == 16) vfat_copy_text(out->type, sizeof(out->type), "FAT16");
    else vfat_copy_text(out->type, sizeof(out->type), "FAT32");

    if (!fs->free_count_valid) {
        uint32_t free_clusters = 0;
        for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
            uint32_t raw = fat_get_raw(fs, c);
            if (raw == UINT32_MAX) return -1;
            if (raw == 0) free_clusters++;
        }
        fs->free_clusters = free_clusters;
        fs->free_count_valid = 1;
    }
    out->free_bytes = (uint64_t)fs->free_clusters * fs->cluster_bytes;
    out->used_bytes = out->total_bytes >= out->free_bytes ? out->total_bytes - out->free_bytes : 0;
    return 0;
}

static int vfat_unmount(struct vfs_node *mountpoint) {
    struct vfat_node *root = (struct vfat_node *)vfs_get_fs_data(mountpoint);
    if (!root) return -1;
    if (root->fs) {
        if (root->fs->fat_cache) kfree(root->fs->fat_cache);
        kfree(root->fs);
    }
    kfree(root);
    return 0;
}

static struct fs_driver vfat_driver = { .name = "vfat", .mount = vfat_mount, .next = 0, .probe = vfat_probe, .unmount = vfat_unmount, .info = vfat_info };
static int vfat_init(void) { return fs_register(&vfat_driver); }
MODULE_INFO("vfat", vfat_init, 0, 0, "fs");
