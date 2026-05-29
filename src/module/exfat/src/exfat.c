#include "mod/fs.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "string.h"

#define EXFAT_BOOT_REGION_SECTORS 12u
#define EXFAT_BOOT_SIG 0xAA55u
#define EXFAT_ROOT_INO 1u
#define EXFAT_ENTRY_SIZE 32u
#define EXFAT_MAX_NAME_UNITS 255u
#define EXFAT_MAX_NAME_UTF8 1024u
#define EXFAT_MAX_SECONDARIES 18u
#define EXFAT_CLUSTER_FIRST 2u
#define EXFAT_EOC_MIN 0xFFFFFFF8u
#define EXFAT_BAD_CLUSTER 0xFFFFFFF7u

#define EXFAT_ENTRY_EOD 0x00u
#define EXFAT_ENTRY_BITMAP 0x81u
#define EXFAT_ENTRY_UPCASE 0x82u
#define EXFAT_ENTRY_LABEL 0x83u
#define EXFAT_ENTRY_FILE 0x85u
#define EXFAT_ENTRY_STREAM 0xC0u
#define EXFAT_ENTRY_NAME 0xC1u

#define EXFAT_ATTR_READONLY 0x0001u
#define EXFAT_ATTR_HIDDEN 0x0002u
#define EXFAT_ATTR_SYSTEM 0x0004u
#define EXFAT_ATTR_DIRECTORY 0x0010u
#define EXFAT_ATTR_ARCHIVE 0x0020u

#define EXFAT_STREAM_NO_FAT_CHAIN 0x02u
#define EXFAT_MAX_SET_ENTRIES (EXFAT_MAX_SECONDARIES + 1u)
#define EXFAT_FAT_FREE 0x00000000u
#define EXFAT_FAT_EOC 0xFFFFFFFFu

struct exfat_fs {
    struct vfs_node *dev;
    uint32_t sector_size;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t active_fat_offset;
    uint64_t volume_length;
    uint64_t image_bytes;
    uint8_t fat_count;
    uint8_t active_fat;
    uint8_t readonly;
    uint32_t bitmap_cluster;
    uint64_t bitmap_length;
    uint16_t *upcase;
    uint8_t *bitmap_cache;
    uint64_t bitmap_cache_index;
    uint32_t bitmap_cache_bytes;
    uint8_t bitmap_cache_valid;
    uint8_t free_count_valid;
    uint32_t next_free_cluster;
    uint32_t free_clusters;
};

struct exfat_node {
    struct exfat_fs *fs;
    uint32_t first_cluster;
    uint64_t data_length;
    uint64_t valid_length;
    uint16_t attr;
    uint8_t flags;
    uint8_t is_root;
    uint64_t dir_index;
    uint64_t parent_data_length;
    uint32_t parent_first_cluster;
    uint8_t parent_flags;
    uint8_t parent_is_root;
    uint8_t secondary_count;
};

struct exfat_dir_entry {
    char name[EXFAT_MAX_NAME_UTF8];
    uint16_t name16[EXFAT_MAX_NAME_UNITS];
    uint8_t name_len;
    uint16_t attr;
    uint8_t flags;
    uint32_t first_cluster;
    uint64_t data_length;
    uint64_t valid_length;
    uint64_t dir_index;
    uint8_t secondary_count;
};

struct exfat_upcase_ref {
    uint32_t checksum;
    uint32_t first_cluster;
    uint64_t data_length;
};

static struct vfs_node *exfat_finddir(struct vfs_node *parent, const char *name);
static int exfat_readdir(struct vfs_node *parent, uint64_t index, struct vfs_dirent *out);
static uint64_t exfat_read(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf);
static uint64_t exfat_write(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf);
static int exfat_create(struct vfs_node *parent, const char *name, uint32_t type, struct vfs_node **out);
static int exfat_unlink(struct vfs_node *parent, const char *name);
static int exfat_rename(struct vfs_node *old_parent, const char *old_name, struct vfs_node *new_parent, const char *new_name);
static int exfat_truncate(struct vfs_node *vnode, uint64_t size);
static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

static uint16_t checksum16_byte(uint16_t c, uint8_t b) { return (uint16_t)(((c & 1u) ? 0x8000u : 0u) + (c >> 1) + b); }
static uint32_t checksum32_byte(uint32_t c, uint8_t b) { return ((c & 1u) ? 0x80000000u : 0u) + (c >> 1) + b; }

static int is_pow2_u32(uint32_t v) { return v && !(v & (v - 1u)); }
static int cluster_valid(const struct exfat_fs *fs, uint32_t c) { return c >= EXFAT_CLUSTER_FIRST && c < fs->cluster_count + EXFAT_CLUSTER_FIRST; }
static int fat_eoc(uint32_t c) { return c >= EXFAT_EOC_MIN; }

static int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (UINT64_MAX - a < b) return 1;
    *out = a + b;
    return 0;
}

static int mul_overflow_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a && b > UINT64_MAX / a) return 1;
    *out = a * b;
    return 0;
}

static int read_bytes(struct exfat_fs *fs, uint64_t off, uint64_t size, void *buf) {
    uint64_t end;
    if (add_overflow_u64(off, size, &end) || (fs->image_bytes && end > fs->image_bytes)) return -1;
    return block_read(fs->dev, off, size, buf) == size ? 0 : -1;
}

static int write_bytes(struct exfat_fs *fs, uint64_t off, uint64_t size, const void *buf) {
    uint64_t end;
    if (!fs || !buf || fs->readonly) return -1;
    if (add_overflow_u64(off, size, &end) || (fs->image_bytes && end > fs->image_bytes)) return -1;
    return block_write(fs->dev, off, size, buf) == size ? 0 : -1;
}

static int zero_bytes(struct exfat_fs *fs, uint64_t off, uint64_t size) {
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    while (size) {
        uint64_t n = size > sizeof(zero) ? sizeof(zero) : size;
        if (write_bytes(fs, off, n, zero) != 0) return -1;
        off += n;
        size -= n;
    }
    return 0;
}

static uint64_t cluster_off(struct exfat_fs *fs, uint32_t cluster) {
    uint64_t rel = (uint64_t)(cluster - EXFAT_CLUSTER_FIRST) * fs->sectors_per_cluster;
    return (uint64_t)(fs->cluster_heap_offset + rel) * fs->sector_size;
}

static int stream_layout_valid(struct exfat_fs *fs, uint32_t first_cluster, uint64_t data_length, uint8_t flags) {
    if (!data_length) return first_cluster == 0;
    if (!cluster_valid(fs, first_cluster)) return 0;
    uint64_t clusters = (data_length + fs->cluster_size - 1u) / fs->cluster_size;
    if (!clusters || clusters > fs->cluster_count) return 0;
    if (flags & EXFAT_STREAM_NO_FAT_CHAIN) {
        if ((uint64_t)first_cluster + clusters > (uint64_t)fs->cluster_count + EXFAT_CLUSTER_FIRST) return 0;
        uint64_t end;
        if (add_overflow_u64(cluster_off(fs, first_cluster), data_length, &end) || end > fs->image_bytes) return 0;
    }
    return 1;
}

static int fat_read(struct exfat_fs *fs, uint32_t cluster, uint32_t *out) {
    if (!cluster_valid(fs, cluster)) return -1;
    uint64_t off = (uint64_t)fs->active_fat_offset * fs->sector_size + (uint64_t)cluster * 4u;
    uint8_t e[4];
    if (read_bytes(fs, off, sizeof(e), e) != 0) return -1;
    uint32_t v = rd32(e);
    if (v == EXFAT_BAD_CLUSTER || v == 0 || v == 1) return -1;
    if (!fat_eoc(v) && !cluster_valid(fs, v)) return -1;
    *out = v;
    return 0;
}

static uint64_t fat_entry_off(struct exfat_fs *fs, uint32_t fat, uint32_t cluster) {
    return (uint64_t)(fs->fat_offset + fat * fs->fat_length) * fs->sector_size + (uint64_t)cluster * 4u;
}

static int fat_read_from(struct exfat_fs *fs, uint32_t fat, uint32_t cluster, uint32_t *out) {
    uint8_t e[4];
    if (!cluster_valid(fs, cluster) || fat >= fs->fat_count) return -1;
    if (read_bytes(fs, fat_entry_off(fs, fat, cluster), sizeof(e), e) != 0) return -1;
    *out = rd32(e);
    return 0;
}

static int fat_write_one(struct exfat_fs *fs, uint32_t fat, uint32_t cluster, uint32_t value) {
    uint8_t e[4];
    if (!cluster_valid(fs, cluster) || fat >= fs->fat_count) return -1;
    wr32(e, value);
    return write_bytes(fs, fat_entry_off(fs, fat, cluster), sizeof(e), e);
}

static int fat_set(struct exfat_fs *fs, uint32_t cluster, uint32_t value) {
    if (!cluster_valid(fs, cluster)) return -1;
    if (fs->fat_count == 2) {
        uint32_t inactive = fs->active_fat ^ 1u;
        if (fat_write_one(fs, inactive, cluster, value) != 0) return -1;
    }
    return fat_write_one(fs, fs->active_fat, cluster, value);
}

static int stream_cluster_at(struct exfat_node *n, uint64_t index, uint32_t *out);

static int stream_next_cluster(struct exfat_node *n, uint32_t current, uint32_t *out) {
    struct exfat_fs *fs = n->fs;
    if (n->flags & EXFAT_STREAM_NO_FAT_CHAIN) {
        uint64_t c = (uint64_t)current + 1u;
        if (c > UINT32_MAX || !cluster_valid(fs, (uint32_t)c)) return -1;
        *out = (uint32_t)c;
        return 0;
    }
    uint32_t next;
    if (fat_read(fs, current, &next) != 0 || fat_eoc(next) || !cluster_valid(fs, next)) return -1;
    *out = next;
    return 0;
}

static int stream_cluster_at(struct exfat_node *n, uint64_t index, uint32_t *out) {
    struct exfat_fs *fs = n->fs;
    if (!cluster_valid(fs, n->first_cluster)) return -1;
    if (n->flags & EXFAT_STREAM_NO_FAT_CHAIN) {
        uint64_t c = (uint64_t)n->first_cluster + index;
        if (c > UINT32_MAX || !cluster_valid(fs, (uint32_t)c)) return -1;
        *out = (uint32_t)c;
        return 0;
    }
    uint32_t c = n->first_cluster;
    for (uint64_t i = 0; i < index; i++) {
        if (i >= fs->cluster_count || stream_next_cluster(n, c, &c) != 0) return -1;
    }
    *out = c;
    return 0;
}

static int dir_read_entry(struct exfat_node *dir, uint64_t index, uint8_t out[EXFAT_ENTRY_SIZE]) {
    uint64_t off = index * EXFAT_ENTRY_SIZE;
    if (!dir->is_root && off + EXFAT_ENTRY_SIZE > dir->data_length) return 1;
    uint64_t ci = off / dir->fs->cluster_size;
    uint64_t co = off % dir->fs->cluster_size;
    uint32_t c;
    if (ci >= dir->fs->cluster_count || stream_cluster_at(dir, ci, &c) != 0) return -1;
    if (read_bytes(dir->fs, cluster_off(dir->fs, c) + co, EXFAT_ENTRY_SIZE, out) != 0) return -1;
    return 0;
}

static int dir_write_entry(struct exfat_node *dir, uint64_t index, const uint8_t in[EXFAT_ENTRY_SIZE]) {
    uint64_t off = index * EXFAT_ENTRY_SIZE;
    if (!dir->is_root && off + EXFAT_ENTRY_SIZE > dir->data_length) return -1;
    uint64_t ci = off / dir->fs->cluster_size;
    uint64_t co = off % dir->fs->cluster_size;
    uint32_t c;
    if (ci >= dir->fs->cluster_count || stream_cluster_at(dir, ci, &c) != 0) return -1;
    return write_bytes(dir->fs, cluster_off(dir->fs, c) + co, EXFAT_ENTRY_SIZE, in);
}

static int utf16_to_utf8(const uint16_t *in, uint32_t len, char out[EXFAT_MAX_NAME_UTF8]) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            if (i + 1 >= len) return -1;
            uint32_t lo = in[++i];
            if (lo < 0xDC00u || lo > 0xDFFFu) return -1;
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) return -1;
        if (cp < 0x80u) {
            if (o + 1 >= EXFAT_MAX_NAME_UTF8) return -1;
            out[o++] = (char)cp;
        } else if (cp < 0x800u) {
            if (o + 2 >= EXFAT_MAX_NAME_UTF8) return -1;
            out[o++] = (char)(0xC0u | (cp >> 6));
            out[o++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            if (o + 3 >= EXFAT_MAX_NAME_UTF8) return -1;
            out[o++] = (char)(0xE0u | (cp >> 12));
            out[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[o++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp <= 0x10FFFFu) {
            if (o + 4 >= EXFAT_MAX_NAME_UTF8) return -1;
            out[o++] = (char)(0xF0u | (cp >> 18));
            out[o++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            out[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[o++] = (char)(0x80u | (cp & 0x3Fu));
        } else return -1;
    }
    out[o] = 0;
    return 0;
}

static int utf8_to_utf16(const char *s, uint16_t out[EXFAT_MAX_NAME_UNITS], uint32_t *out_len) {
    uint32_t n = 0;
    for (uint32_t i = 0; s[i];) {
        uint8_t c = (uint8_t)s[i++];
        uint32_t cp;
        if (c < 0x80u) cp = c;
        else if ((c & 0xE0u) == 0xC0u) {
            uint8_t b1 = (uint8_t)s[i++];
            if ((b1 & 0xC0u) != 0x80u) return -1;
            cp = ((uint32_t)(c & 0x1Fu) << 6) | (b1 & 0x3Fu);
            if (cp < 0x80u) return -1;
        } else if ((c & 0xF0u) == 0xE0u) {
            uint8_t b1 = (uint8_t)s[i++], b2 = (uint8_t)s[i++];
            if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) return -1;
            cp = ((uint32_t)(c & 0x0Fu) << 12) | ((uint32_t)(b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
            if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) return -1;
        } else if ((c & 0xF8u) == 0xF0u) {
            uint8_t b1 = (uint8_t)s[i++], b2 = (uint8_t)s[i++], b3 = (uint8_t)s[i++];
            if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) return -1;
            cp = ((uint32_t)(c & 0x07u) << 18) | ((uint32_t)(b1 & 0x3Fu) << 12) | ((uint32_t)(b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
            if (cp < 0x10000u || cp > 0x10FFFFu) return -1;
        } else return -1;
        if (cp < 0x10000u) {
            if (n >= EXFAT_MAX_NAME_UNITS) return -1;
            out[n++] = (uint16_t)cp;
        } else {
            if (n + 1 >= EXFAT_MAX_NAME_UNITS) return -1;
            cp -= 0x10000u;
            out[n++] = (uint16_t)(0xD800u + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
        }
    }
    *out_len = n;
    return n ? 0 : -1;
}

static uint16_t upcase_unit(struct exfat_fs *fs, uint16_t c) {
    return fs->upcase ? fs->upcase[c] : c;
}

static int name16_eq(struct exfat_fs *fs, const uint16_t *a, uint32_t alen, const uint16_t *b, uint32_t blen) {
    if (alen != blen) return 0;
    for (uint32_t i = 0; i < alen; i++) if (upcase_unit(fs, a[i]) != upcase_unit(fs, b[i])) return 0;
    return 1;
}

static uint16_t name_hash(struct exfat_fs *fs, const uint16_t *name, uint32_t len) {
    uint16_t h = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint16_t c = upcase_unit(fs, name[i]);
        h = checksum16_byte(h, (uint8_t)c);
        h = checksum16_byte(h, (uint8_t)(c >> 8));
    }
    return h;
}

static uint16_t file_set_checksum(const uint8_t set[][EXFAT_ENTRY_SIZE], uint32_t entries) {
    uint16_t c = 0;
    for (uint32_t e = 0; e < entries; e++) {
        for (uint32_t i = 0; i < EXFAT_ENTRY_SIZE; i++) {
            if (e == 0 && (i == 2 || i == 3)) continue;
            c = checksum16_byte(c, set[e][i]);
        }
    }
    return c;
}

static int parse_file_set(struct exfat_node *dir, uint64_t index, struct exfat_dir_entry *out, uint32_t *consumed) {
    uint8_t set[EXFAT_MAX_SET_ENTRIES][EXFAT_ENTRY_SIZE];
    int r = dir_read_entry(dir, index, set[0]);
    if (r != 0) return r;
    if (set[0][0] == EXFAT_ENTRY_EOD) return 1;
    if (set[0][0] != EXFAT_ENTRY_FILE) return -2;
    uint32_t secondary_count = set[0][1];
    if (secondary_count < 2 || secondary_count > EXFAT_MAX_SECONDARIES) return -1;
    for (uint32_t i = 1; i <= secondary_count; i++) {
        r = dir_read_entry(dir, index + i, set[i]);
        if (r != 0 || !(set[i][0] & 0x80u)) return -1;
    }
    *consumed = secondary_count + 1;
    if (file_set_checksum(set, secondary_count + 1) != rd16(set[0] + 2)) return -1;

    const uint8_t *stream = 0;
    uint16_t name16[EXFAT_MAX_NAME_UNITS];
    uint32_t name_units = 0;
    uint32_t stream_count = 0, name_entries = 0;
    for (uint32_t i = 1; i <= secondary_count; i++) {
        if (set[i][0] == EXFAT_ENTRY_STREAM) {
            if (stream) return -1;
            stream = set[i];
            stream_count++;
        } else if (set[i][0] == EXFAT_ENTRY_NAME) {
            name_entries++;
            for (uint32_t j = 0; j < 15 && name_units < EXFAT_MAX_NAME_UNITS; j++) name16[name_units++] = rd16(set[i] + 2 + j * 2);
        }
    }
    if (!stream || stream_count != 1) return -1;
    uint32_t name_len = stream[3];
    if (!name_len || name_len > EXFAT_MAX_NAME_UNITS || name_entries != (name_len + 14u) / 15u) return -1;
    if (name_units < name_len) return -1;
    name_units = name_len;

    memset(out, 0, sizeof(*out));
    out->attr = rd16(set[0] + 4);
    out->flags = stream[1];
    out->valid_length = rd64(stream + 8);
    out->first_cluster = rd32(stream + 20);
    out->data_length = rd64(stream + 24);
    out->name_len = (uint8_t)name_len;
    out->dir_index = index;
    out->secondary_count = (uint8_t)secondary_count;
    memcpy(out->name16, name16, name_len * sizeof(uint16_t));
    if (out->valid_length > out->data_length) return -1;
    if ((out->attr & ~(EXFAT_ATTR_READONLY | EXFAT_ATTR_HIDDEN | EXFAT_ATTR_SYSTEM | EXFAT_ATTR_DIRECTORY | EXFAT_ATTR_ARCHIVE)) != 0) return -1;
    if (!stream_layout_valid(dir->fs, out->first_cluster, out->data_length, out->flags)) return -1;
    if ((out->attr & EXFAT_ATTR_DIRECTORY) && (out->valid_length != out->data_length || (out->data_length & (EXFAT_ENTRY_SIZE - 1u)))) return -1;
    if (name_hash(dir->fs, out->name16, out->name_len) != rd16(stream + 4)) return -1;
    if (utf16_to_utf8(out->name16, out->name_len, out->name) != 0) return -1;
    return 0;
}

static int scan_dir(struct exfat_node *dir, const uint16_t *want, uint32_t want_len, uint64_t want_index, struct exfat_dir_entry *out) {
    uint64_t entry_limit = dir->is_root ? (uint64_t)dir->fs->cluster_count * (dir->fs->cluster_size / EXFAT_ENTRY_SIZE) : dir->data_length / EXFAT_ENTRY_SIZE;
    uint64_t seen = 0;
    for (uint64_t i = 0; i < entry_limit;) {
        uint8_t e[EXFAT_ENTRY_SIZE];
        int r = dir_read_entry(dir, i, e);
        if (r != 0) return -1;
        if (e[0] == EXFAT_ENTRY_EOD) return -1;
        if (!(e[0] & 0x80u)) { i++; continue; }
        if (e[0] != EXFAT_ENTRY_FILE) { i++; continue; }
        struct exfat_dir_entry de;
        uint32_t consumed = 0;
        r = parse_file_set(dir, i, &de, &consumed);
        if (r != 0) return -1;
        if (want) {
            if (name16_eq(dir->fs, de.name16, de.name_len, want, want_len)) { *out = de; return 0; }
        } else {
            if (seen == want_index) { *out = de; return 0; }
            seen++;
        }
        i += consumed;
    }
    return -1;
}

static int find_upcase_ref(struct exfat_fs *fs, struct exfat_upcase_ref *ref) {
    struct exfat_node root;
    memset(&root, 0, sizeof(root));
    root.fs = fs;
    root.first_cluster = fs->root_cluster;
    root.flags = 0;
    root.is_root = 1;
    uint64_t entry_limit = (uint64_t)fs->cluster_count * (fs->cluster_size / EXFAT_ENTRY_SIZE);
    for (uint64_t i = 0; i < entry_limit; i++) {
        uint8_t e[EXFAT_ENTRY_SIZE];
        int r = dir_read_entry(&root, i, e);
        if (r != 0) return -1;
        if (e[0] == EXFAT_ENTRY_EOD) break;
        if (e[0] != EXFAT_ENTRY_UPCASE) continue;
        memset(ref, 0, sizeof(*ref));
        ref->checksum = rd32(e + 4);
        ref->first_cluster = rd32(e + 20);
        ref->data_length = rd64(e + 24);
        return stream_layout_valid(fs, ref->first_cluster, ref->data_length, 0) ? 0 : -1;
    }
    return -1;
}

static int find_bitmap_ref(struct exfat_fs *fs) {
    struct exfat_node root;
    memset(&root, 0, sizeof(root));
    root.fs = fs;
    root.first_cluster = fs->root_cluster;
    root.flags = 0;
    root.is_root = 1;
    uint64_t entry_limit = (uint64_t)fs->cluster_count * (fs->cluster_size / EXFAT_ENTRY_SIZE);
    for (uint64_t i = 0; i < entry_limit; i++) {
        uint8_t e[EXFAT_ENTRY_SIZE];
        int r = dir_read_entry(&root, i, e);
        if (r != 0) return -1;
        if (e[0] == EXFAT_ENTRY_EOD) break;
        if (e[0] != EXFAT_ENTRY_BITMAP) continue;
        if ((e[1] & 1u) != fs->active_fat) continue;
        fs->bitmap_cluster = rd32(e + 20);
        fs->bitmap_length = rd64(e + 24);
        if (!fs->bitmap_length || fs->bitmap_length < ((uint64_t)fs->cluster_count + 7u) / 8u) return -1;
        return stream_layout_valid(fs, fs->bitmap_cluster, fs->bitmap_length, 0) ? 0 : -1;
    }
    return -1;
}

static int read_stream(struct exfat_node *n, uint64_t off, uint64_t size, uint8_t *buf) {
    if (off > n->data_length || size > n->data_length - off) return -1;
    if (!size) return 0;
    uint32_t cluster;
    if (stream_cluster_at(n, off / n->fs->cluster_size, &cluster) != 0) return -1;
    uint64_t done = 0;
    uint64_t in_cluster = off % n->fs->cluster_size;
    while (done < size) {
        uint64_t take = n->fs->cluster_size - in_cluster;
        if (take > size - done) take = size - done;
        if (read_bytes(n->fs, cluster_off(n->fs, cluster) + in_cluster, take, buf + done) != 0) return -1;
        done += take;
        in_cluster = 0;
        if (done < size && stream_next_cluster(n, cluster, &cluster) != 0) return -1;
    }
    return 0;
}

static int bitmap_cache_load(struct exfat_fs *fs, uint64_t index) {
    if (!fs->bitmap_cache || !fs->bitmap_cluster) return -1;
    if (fs->bitmap_cache_valid && fs->bitmap_cache_index == index) return 0;
    uint64_t off = index * fs->cluster_size;
    if (off >= fs->bitmap_length) return -1;
    struct exfat_node bm;
    memset(&bm, 0, sizeof(bm));
    bm.fs = fs;
    bm.first_cluster = fs->bitmap_cluster;
    bm.data_length = fs->bitmap_length;
    bm.valid_length = fs->bitmap_length;
    uint32_t c;
    if (stream_cluster_at(&bm, index, &c) != 0) return -1;
    uint64_t bytes = fs->bitmap_length - off;
    if (bytes > fs->cluster_size) bytes = fs->cluster_size;
    if (read_bytes(fs, cluster_off(fs, c), bytes, fs->bitmap_cache) != 0) return -1;
    fs->bitmap_cache_index = index;
    fs->bitmap_cache_bytes = (uint32_t)bytes;
    fs->bitmap_cache_valid = 1;
    return 0;
}

static int bitmap_rw(struct exfat_fs *fs, uint32_t cluster, uint8_t *byte, uint8_t write) {
    if (!cluster_valid(fs, cluster) || !byte || !fs->bitmap_cluster) return -1;
    uint32_t rel = cluster - EXFAT_CLUSTER_FIRST;
    uint64_t off = rel / 8u;
    if (off >= fs->bitmap_length) return -1;
    uint64_t index = off / fs->cluster_size;
    uint32_t in_cache = (uint32_t)(off - index * fs->cluster_size);
    if (bitmap_cache_load(fs, index) != 0 || in_cache >= fs->bitmap_cache_bytes) return -1;
    if (!write) { *byte = fs->bitmap_cache[in_cache]; return 0; }
    struct exfat_node bm;
    memset(&bm, 0, sizeof(bm));
    bm.fs = fs;
    bm.first_cluster = fs->bitmap_cluster;
    bm.data_length = fs->bitmap_length;
    bm.valid_length = fs->bitmap_length;
    uint32_t c;
    if (stream_cluster_at(&bm, index, &c) != 0) return -1;
    if (write_bytes(fs, cluster_off(fs, c) + in_cache, 1, byte) != 0) return -1;
    fs->bitmap_cache[in_cache] = *byte;
    return 0;
}

static int bitmap_get(struct exfat_fs *fs, uint32_t cluster, uint8_t *used) {
    uint8_t b;
    if (bitmap_rw(fs, cluster, &b, 0) != 0) return -1;
    *used = (uint8_t)((b >> ((cluster - EXFAT_CLUSTER_FIRST) & 7u)) & 1u);
    return 0;
}

static int bitmap_set(struct exfat_fs *fs, uint32_t cluster, uint8_t used) {
    uint8_t b;
    uint8_t bit = (uint8_t)(1u << ((cluster - EXFAT_CLUSTER_FIRST) & 7u));
    if (bitmap_rw(fs, cluster, &b, 0) != 0) return -1;
    uint8_t old = (uint8_t)((b & bit) != 0);
    if (old == used) return 0;
    if (used) b |= bit;
    else b &= (uint8_t)~bit;
    if (bitmap_rw(fs, cluster, &b, 1) != 0) return -1;
    if (fs->free_count_valid) {
        if (used && fs->free_clusters) fs->free_clusters--;
        else if (!used && fs->free_clusters < fs->cluster_count) fs->free_clusters++;
    }
    if (!used && (cluster < fs->next_free_cluster || !cluster_valid(fs, fs->next_free_cluster))) fs->next_free_cluster = cluster;
    return 0;
}

static int load_upcase(struct exfat_fs *fs) {
    struct exfat_upcase_ref ref;
    if (find_upcase_ref(fs, &ref) != 0 || !ref.data_length || ref.data_length > UINT32_MAX || (ref.data_length & 1u)) return -1;
    uint16_t *map = (uint16_t *)kmalloc(65536u * sizeof(uint16_t));
    uint8_t *raw = (uint8_t *)kmalloc((size_t)ref.data_length);
    if (!map || !raw) { if (map) kfree(map); if (raw) kfree(raw); return -1; }
    struct exfat_node s;
    memset(&s, 0, sizeof(s));
    s.fs = fs;
    s.first_cluster = ref.first_cluster;
    s.data_length = ref.data_length;
    s.valid_length = ref.data_length;
    if (read_stream(&s, 0, ref.data_length, raw) != 0) { kfree(raw); kfree(map); return -1; }
    uint32_t sum = 0;
    for (uint64_t i = 0; i < ref.data_length; i++) sum = checksum32_byte(sum, raw[i]);
    if (sum != ref.checksum) { kfree(raw); kfree(map); return -1; }
    uint32_t code = 0;
    for (uint64_t i = 0; i < ref.data_length;) {
        uint16_t v = rd16(raw + i);
        i += 2;
        if (v == 0xFFFFu) {
            if (i + 2 > ref.data_length) { kfree(raw); kfree(map); return -1; }
            uint16_t run = rd16(raw + i);
            i += 2;
            if ((uint32_t)run > 65536u - code) { kfree(raw); kfree(map); return -1; }
            for (uint32_t j = 0; j < run; j++) map[code] = (uint16_t)code, code++;
        } else {
            if (code >= 65536u) { kfree(raw); kfree(map); return -1; }
            map[code++] = v;
        }
    }
    kfree(raw);
    if (code != 65536u) { kfree(map); return -1; }
    fs->upcase = map;
    return 0;
}

static int validate_boot_checksum(struct vfs_node *blockdev, uint32_t sector_size, uint32_t expected_entries) {
    uint64_t bytes = (uint64_t)EXFAT_BOOT_REGION_SECTORS * sector_size;
    uint8_t *vbr = (uint8_t *)kmalloc((size_t)bytes);
    if (!vbr) return -1;
    if (block_read(blockdev, 0, bytes, vbr) != bytes) { kfree(vbr); return -1; }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < (EXFAT_BOOT_REGION_SECTORS - 1u) * sector_size; i++) {
        if (i == 106u || i == 107u || i == 112u) continue;
        sum = checksum32_byte(sum, vbr[i]);
    }
    const uint8_t *cs = vbr + (uint64_t)(EXFAT_BOOT_REGION_SECTORS - 1u) * sector_size;
    for (uint32_t i = 0; i < expected_entries; i++) {
        if (rd32(cs + i * 4u) != sum) { kfree(vbr); return -1; }
    }
    kfree(vbr);
    return 0;
}

static int parse_boot(struct exfat_fs *fs, struct vfs_node *blockdev, const uint8_t bs[512], int full_check) {
    if (rd16(bs + 510) != EXFAT_BOOT_SIG) return -1;
    if (bs[0] != 0xEBu || bs[1] != 0x76u || bs[2] != 0x90u || memcmp(bs + 3, "EXFAT   ", 8) != 0) return -1;
    for (uint32_t i = 11; i < 64; i++) if (bs[i]) return -1;
    uint8_t sector_shift = bs[108];
    uint8_t cluster_shift = bs[109];
    if (sector_shift < 9 || sector_shift > 12) return -1;
    if (cluster_shift > 25 || sector_shift + cluster_shift > 25) return -1;
    fs->sector_size = 1u << sector_shift;
    fs->sectors_per_cluster = 1u << cluster_shift;
    fs->cluster_size = 1u << (sector_shift + cluster_shift);
    fs->volume_length = rd64(bs + 72);
    fs->fat_offset = rd32(bs + 80);
    fs->fat_length = rd32(bs + 84);
    fs->cluster_heap_offset = rd32(bs + 88);
    fs->cluster_count = rd32(bs + 92);
    fs->root_cluster = rd32(bs + 96);
    uint16_t flags = rd16(bs + 106);
    uint8_t fats = bs[110];
    if (fats != 1 && fats != 2) return -1;
    if (fats == 1 && (flags & 1u)) return -1;
    uint32_t active = flags & 1u;
    fs->fat_count = fats;
    fs->active_fat = (uint8_t)active;
    fs->active_fat_offset = fs->fat_offset + active * fs->fat_length;
    if (!fs->volume_length || !fs->fat_offset || !fs->fat_length || !fs->cluster_heap_offset || !fs->cluster_count) return -1;
    if (!is_pow2_u32(fs->sector_size) || !is_pow2_u32(fs->sectors_per_cluster)) return -1;
    if (!cluster_valid(fs, fs->root_cluster)) return -1;
    uint64_t image_bytes;
    if (mul_overflow_u64(fs->volume_length, fs->sector_size, &image_bytes)) return -1;
    fs->image_bytes = image_bytes;
    uint64_t fat_end = (uint64_t)fs->active_fat_offset + fs->fat_length;
    uint64_t heap_end = (uint64_t)fs->cluster_heap_offset + (uint64_t)fs->cluster_count * fs->sectors_per_cluster;
    if ((uint64_t)fs->fat_offset < EXFAT_BOOT_REGION_SECTORS || fat_end > fs->cluster_heap_offset || heap_end > fs->volume_length) return -1;
    if ((uint64_t)(fs->cluster_count + EXFAT_CLUSTER_FIRST) * 4u > (uint64_t)fs->fat_length * fs->sector_size) return -1;
    uint64_t dev_size = block_size(blockdev);
    if (dev_size && fs->image_bytes > dev_size) return -1;
    if (full_check && validate_boot_checksum(blockdev, fs->sector_size, fs->sector_size / 4u) != 0) return -1;
    return 0;
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

static void attach_ops(struct vfs_node *vnode, uint32_t type) {
    if (type == VFS_NODE_DIRECTORY) {
        vfs_set_finddir(vnode, exfat_finddir);
        vfs_set_readdir(vnode, exfat_readdir);
        vfs_set_create(vnode, exfat_create);
        vfs_set_unlink(vnode, exfat_unlink);
        vfs_set_rename(vnode, exfat_rename);
    } else {
        vfs_set_read(vnode, exfat_read);
        vfs_set_write(vnode, exfat_write);
        vfs_set_truncate(vnode, exfat_truncate);
    }
}

static struct vfs_node *make_vnode(struct exfat_node *parent, const struct exfat_dir_entry *de) {
    struct exfat_node *n = (struct exfat_node *)kmalloc(sizeof(*n));
    if (!n) return 0;
    memset(n, 0, sizeof(*n));
    n->fs = parent->fs;
    n->first_cluster = de->first_cluster;
    n->data_length = de->data_length;
    n->valid_length = de->valid_length;
    n->attr = de->attr;
    n->flags = de->flags;
    n->dir_index = de->dir_index;
    n->secondary_count = de->secondary_count;
    n->parent_first_cluster = parent->first_cluster;
    n->parent_data_length = parent->data_length;
    n->parent_flags = parent->flags;
    n->parent_is_root = parent->is_root;
    uint32_t type = (de->attr & EXFAT_ATTR_DIRECTORY) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    uint64_t size = type == VFS_NODE_DIRECTORY ? 0 : de->valid_length;
    struct vfs_node *v = vfs_create_fs_node(de->name, type, de->first_cluster ? de->first_cluster : EXFAT_ROOT_INO, size, n);
    if (!v) { kfree(n); return 0; }
    attach_ops(v, type);
    return v;
}

static struct vfs_node *exfat_finddir(struct vfs_node *parent, const char *name) {
    struct exfat_node *dir = (struct exfat_node *)vfs_get_fs_data(parent);
    if (!dir || !name || !(dir->attr & EXFAT_ATTR_DIRECTORY)) return 0;
    uint16_t want[EXFAT_MAX_NAME_UNITS];
    uint32_t want_len;
    if (utf8_to_utf16(name, want, &want_len) != 0) return 0;
    struct exfat_dir_entry de;
    if (scan_dir(dir, want, want_len, 0, &de) != 0) return 0;
    struct vfs_node *v = make_vnode(dir, &de);
    if (v) vfs_add_child(parent, v);
    return v;
}

static int exfat_readdir(struct vfs_node *parent, uint64_t index, struct vfs_dirent *out) {
    struct exfat_node *dir = (struct exfat_node *)vfs_get_fs_data(parent);
    if (!dir || !out || !(dir->attr & EXFAT_ATTR_DIRECTORY)) return -1;
    struct exfat_dir_entry de;
    if (scan_dir(dir, 0, 0, index, &de) != 0) return -1;
    char *dst = out->name;
    uint64_t cap = out->name_capacity;
    uint64_t len = strlen(de.name);
    memset(out, 0, sizeof(*out));
    out->inode = de.first_cluster ? de.first_cluster : EXFAT_ROOT_INO;
    out->type = (de.attr & EXFAT_ATTR_DIRECTORY) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    out->name_len = len;
    out->name = dst;
    out->name_capacity = cap;
    if (dst && cap > len) memcpy(dst, de.name, len + 1);
    return 0;
}

static uint64_t exfat_read(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf) {
    struct exfat_node *n = (struct exfat_node *)vfs_get_fs_data(vnode);
    if (!n || !buf || (n->attr & EXFAT_ATTR_DIRECTORY)) return 0;
    if (off >= n->valid_length) return 0;
    if (size > n->valid_length - off) size = n->valid_length - off;
    if (!size || !n->first_cluster) return 0;
    uint32_t cluster;
    if (stream_cluster_at(n, off / n->fs->cluster_size, &cluster) != 0) return 0;
    uint64_t done = 0;
    uint64_t in_cluster = off % n->fs->cluster_size;
    while (done < size) {
        uint64_t take = n->fs->cluster_size - in_cluster;
        if (take > size - done) take = size - done;
        if (read_bytes(n->fs, cluster_off(n->fs, cluster) + in_cluster, take, buf + done) != 0) break;
        done += take;
        in_cluster = 0;
        if (done < size && stream_next_cluster(n, cluster, &cluster) != 0) break;
    }
    return done;
}

static uint32_t clusters_for(struct exfat_fs *fs, uint64_t size) {
    return size ? (uint32_t)((size + fs->cluster_size - 1u) / fs->cluster_size) : 0;
}

static int chain_count(struct exfat_node *n, uint32_t *out) {
    if (!out) return -1;
    if (!n->first_cluster || !n->data_length) { *out = 0; return 0; }
    if (n->flags & EXFAT_STREAM_NO_FAT_CHAIN) { *out = clusters_for(n->fs, n->data_length); return 0; }
    uint32_t c = n->first_cluster;
    uint32_t count = 0;
    while (cluster_valid(n->fs, c) && count <= n->fs->cluster_count) {
        uint32_t next;
        count++;
        if (fat_read_from(n->fs, n->fs->active_fat, c, &next) != 0) return -1;
        if (fat_eoc(next)) { *out = count; return 0; }
        c = next;
    }
    return -1;
}

static int free_chain(struct exfat_fs *fs, uint32_t first, uint8_t flags, uint32_t count_hint) {
    if (!first) return 0;
    if (flags & EXFAT_STREAM_NO_FAT_CHAIN) {
        for (uint32_t i = 0; i < count_hint; i++) {
            uint32_t c = first + i;
            if (!cluster_valid(fs, c) || bitmap_set(fs, c, 0) != 0 || fat_set(fs, c, EXFAT_FAT_FREE) != 0) return -1;
        }
        return 0;
    }
    uint32_t c = first;
    for (uint32_t guard = 0; guard < fs->cluster_count && cluster_valid(fs, c); guard++) {
        uint32_t next;
        if (fat_read_from(fs, fs->active_fat, c, &next) != 0) return -1;
        if (bitmap_set(fs, c, 0) != 0 || fat_set(fs, c, EXFAT_FAT_FREE) != 0) return -1;
        if (fat_eoc(next)) return 0;
        c = next;
    }
    return -1;
}

static int convert_no_fat_chain(struct exfat_node *n) {
    if (!(n->flags & EXFAT_STREAM_NO_FAT_CHAIN)) return 0;
    uint32_t count = clusters_for(n->fs, n->data_length);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t c = n->first_cluster + i;
        if (!cluster_valid(n->fs, c)) return -1;
        if (fat_set(n->fs, c, i + 1u == count ? EXFAT_FAT_EOC : c + 1u) != 0) return -1;
    }
    n->flags &= (uint8_t)~EXFAT_STREAM_NO_FAT_CHAIN;
    return 0;
}

static int alloc_scan_range(struct exfat_fs *fs, uint32_t first, uint32_t last, uint32_t *out) {
    for (uint32_t c = first; c < last; c++) {
        uint8_t used;
        if (bitmap_get(fs, c, &used) != 0) return -1;
        if (!used) { *out = c; return 0; }
    }
    return 1;
}

static int alloc_cluster(struct exfat_fs *fs, uint32_t *out) {
    if (!out || fs->readonly) return -1;
    if (fs->free_count_valid && !fs->free_clusters) return -1;
    uint32_t end = fs->cluster_count + EXFAT_CLUSTER_FIRST;
    uint32_t start = cluster_valid(fs, fs->next_free_cluster) ? fs->next_free_cluster : EXFAT_CLUSTER_FIRST;
    uint32_t c = 0;
    int r = alloc_scan_range(fs, start, end, &c);
    if (r == 1 && start > EXFAT_CLUSTER_FIRST) r = alloc_scan_range(fs, EXFAT_CLUSTER_FIRST, start, &c);
    if (r != 0) return -1;
    if (bitmap_set(fs, c, 1) != 0) return -1;
    if (fat_set(fs, c, EXFAT_FAT_EOC) != 0) { bitmap_set(fs, c, 0); return -1; }
    if (zero_bytes(fs, cluster_off(fs, c), fs->cluster_size) != 0) { bitmap_set(fs, c, 0); fat_set(fs, c, EXFAT_FAT_FREE); return -1; }
    fs->next_free_cluster = (c + 1u < end) ? c + 1u : EXFAT_CLUSTER_FIRST;
    *out = c;
    return 0;
}

static int ensure_clusters(struct exfat_node *n, uint32_t need) {
    if (!need) return 0;
    if (convert_no_fat_chain(n) != 0) return -1;
    uint32_t have;
    if (chain_count(n, &have) != 0) return -1;
    uint32_t last = 0;
    if (have && stream_cluster_at(n, have - 1u, &last) != 0) return -1;
    while (have < need) {
        uint32_t c;
        if (alloc_cluster(n->fs, &c) != 0) return -1;
        if (!n->first_cluster) n->first_cluster = c;
        else if (fat_set(n->fs, last, c) != 0) { free_chain(n->fs, c, 0, 1); return -1; }
        last = c;
        have++;
    }
    return last ? fat_set(n->fs, last, EXFAT_FAT_EOC) : 0;
}

static int trim_clusters(struct exfat_node *n, uint32_t keep) {
    uint32_t have;
    if (chain_count(n, &have) != 0) return -1;
    if (keep >= have) return 0;
    if (keep == 0) {
        int r = free_chain(n->fs, n->first_cluster, n->flags, have);
        n->first_cluster = 0;
        n->flags &= (uint8_t)~EXFAT_STREAM_NO_FAT_CHAIN;
        return r;
    }
    if (convert_no_fat_chain(n) != 0) return -1;
    uint32_t last;
    if (stream_cluster_at(n, keep - 1u, &last) != 0) return -1;
    uint32_t next;
    if (fat_read_from(n->fs, n->fs->active_fat, last, &next) != 0) return -1;
    if (fat_set(n->fs, last, EXFAT_FAT_EOC) != 0) return -1;
    return fat_eoc(next) ? 0 : free_chain(n->fs, next, 0, have - keep);
}

static void node_parent_dir(struct exfat_node *n, struct exfat_node *out) {
    memset(out, 0, sizeof(*out));
    out->fs = n->fs;
    out->first_cluster = n->parent_first_cluster;
    out->data_length = n->parent_data_length;
    out->valid_length = n->parent_data_length;
    out->attr = EXFAT_ATTR_DIRECTORY;
    out->flags = n->parent_flags;
    out->is_root = n->parent_is_root;
}

static int update_node_entry(struct exfat_node *n) {
    if (!n || n->is_root) return -1;
    struct exfat_node dir;
    node_parent_dir(n, &dir);
    uint8_t set[EXFAT_MAX_SET_ENTRIES][EXFAT_ENTRY_SIZE];
    if (n->secondary_count > EXFAT_MAX_SECONDARIES) return -1;
    for (uint32_t i = 0; i <= n->secondary_count; i++) if (dir_read_entry(&dir, n->dir_index + i, set[i]) != 0) return -1;
    uint32_t stream = UINT32_MAX;
    for (uint32_t i = 1; i <= n->secondary_count; i++) if (set[i][0] == EXFAT_ENTRY_STREAM) stream = i;
    if (stream == UINT32_MAX) return -1;
    set[stream][1] = n->flags;
    wr64(set[stream] + 8, n->valid_length);
    wr32(set[stream] + 20, n->first_cluster);
    wr64(set[stream] + 24, n->data_length);
    wr16(set[0] + 2, file_set_checksum((const uint8_t (*)[EXFAT_ENTRY_SIZE])set, n->secondary_count + 1u));
    for (uint32_t i = 0; i <= n->secondary_count; i++) if (dir_write_entry(&dir, n->dir_index + i, set[i]) != 0) return -1;
    return 0;
}

static int zero_stream_range(struct exfat_node *n, uint64_t off, uint64_t size) {
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    if (!size) return 0;
    uint32_t c;
    if (stream_cluster_at(n, off / n->fs->cluster_size, &c) != 0) return -1;
    uint64_t in_cluster = off % n->fs->cluster_size;
    while (size) {
        uint64_t take = n->fs->cluster_size - in_cluster;
        if (take > sizeof(zero)) take = sizeof(zero);
        if (take > size) take = size;
        if (write_bytes(n->fs, cluster_off(n->fs, c) + in_cluster, take, zero) != 0) return -1;
        off += take;
        size -= take;
        in_cluster += take;
        if (in_cluster == n->fs->cluster_size) {
            in_cluster = 0;
            if (size && stream_next_cluster(n, c, &c) != 0) return -1;
        }
    }
    return 0;
}

static uint64_t exfat_write(struct vfs_node *vnode, uint64_t off, uint64_t size, uint8_t *buf) {
    struct exfat_node *n = (struct exfat_node *)vfs_get_fs_data(vnode);
    if (!n || !buf || (n->attr & EXFAT_ATTR_DIRECTORY) || n->fs->readonly) return 0;
    uint64_t end;
    if (add_overflow_u64(off, size, &end)) return 0;
    if (!size) return 0;
    if (ensure_clusters(n, clusters_for(n->fs, end)) != 0) return 0;
    if (off > n->valid_length && zero_stream_range(n, n->valid_length, off - n->valid_length) != 0) return 0;
    uint32_t cluster;
    if (stream_cluster_at(n, off / n->fs->cluster_size, &cluster) != 0) return 0;
    uint64_t done = 0;
    uint64_t in_cluster = off % n->fs->cluster_size;
    while (done < size) {
        uint64_t take = n->fs->cluster_size - in_cluster;
        if (take > size - done) take = size - done;
        if (write_bytes(n->fs, cluster_off(n->fs, cluster) + in_cluster, take, buf + done) != 0) break;
        done += take;
        in_cluster = 0;
        if (done < size && stream_next_cluster(n, cluster, &cluster) != 0) break;
    }
    if (!done) return 0;
    uint64_t new_end = off + done;
    if (new_end > n->data_length) n->data_length = new_end;
    if (new_end > n->valid_length) n->valid_length = new_end;
    vfs_set_size(vnode, n->valid_length);
    if (update_node_entry(n) != 0) return 0;
    return done;
}

static int valid_new_name(const uint16_t *name, uint32_t len) {
    if (!name || !len || len > EXFAT_MAX_NAME_UNITS) return 0;
    for (uint32_t i = 0; i < len; i++) if (name[i] < 0x20u || name[i] == '/' || name[i] == '\\') return 0;
    return 1;
}

static int dir_find_free(struct exfat_node *dir, uint32_t need, uint64_t *out) {
    uint64_t limit = dir->is_root ? (uint64_t)dir->fs->cluster_count * (dir->fs->cluster_size / EXFAT_ENTRY_SIZE) : dir->data_length / EXFAT_ENTRY_SIZE;
    uint32_t run = 0;
    uint64_t start = 0;
    for (uint64_t i = 0; i < limit; i++) {
        uint8_t e[EXFAT_ENTRY_SIZE];
        if (dir_read_entry(dir, i, e) != 0) return -1;
        if (e[0] == EXFAT_ENTRY_EOD || !(e[0] & 0x80u)) {
            if (!run) start = i;
            run++;
            if (run >= need) { *out = start; return 0; }
        } else run = 0;
        if (e[0] == EXFAT_ENTRY_EOD && run < need) break;
    }
    if (dir->is_root) return -1;
    uint64_t old = dir->data_length;
    if (ensure_clusters(dir, clusters_for(dir->fs, old + dir->fs->cluster_size)) != 0) return -1;
    dir->data_length = old + dir->fs->cluster_size;
    dir->valid_length = dir->data_length;
    if (zero_stream_range(dir, old, dir->fs->cluster_size) != 0) return -1;
    *out = old / EXFAT_ENTRY_SIZE;
    return 0;
}

static int build_file_set(struct exfat_fs *fs, const uint16_t *name, uint32_t name_len, uint16_t attr, uint8_t flags, uint32_t first, uint64_t valid, uint64_t data, uint8_t set[EXFAT_MAX_SET_ENTRIES][EXFAT_ENTRY_SIZE], uint32_t *entries) {
    if (!valid_new_name(name, name_len)) return -1;
    uint32_t names = (name_len + 14u) / 15u;
    uint32_t total = 2u + names;
    if (total > EXFAT_MAX_SET_ENTRIES) return -1;
    memset(set, 0, EXFAT_MAX_SET_ENTRIES * EXFAT_ENTRY_SIZE);
    set[0][0] = EXFAT_ENTRY_FILE;
    set[0][1] = (uint8_t)(total - 1u);
    wr16(set[0] + 4, attr);
    set[1][0] = EXFAT_ENTRY_STREAM;
    set[1][1] = flags;
    set[1][3] = (uint8_t)name_len;
    wr16(set[1] + 4, name_hash(fs, name, name_len));
    wr64(set[1] + 8, valid);
    wr32(set[1] + 20, first);
    wr64(set[1] + 24, data);
    uint32_t pos = 0;
    for (uint32_t e = 0; e < names; e++) {
        set[2u + e][0] = EXFAT_ENTRY_NAME;
        for (uint32_t j = 0; j < 15u; j++) {
            wr16(set[2u + e] + 2u + j * 2u, pos < name_len ? name[pos++] : 0);
        }
    }
    wr16(set[0] + 2, file_set_checksum((const uint8_t (*)[EXFAT_ENTRY_SIZE])set, total));
    *entries = total;
    return 0;
}

static int write_new_set(struct exfat_node *dir, const uint8_t set[EXFAT_MAX_SET_ENTRIES][EXFAT_ENTRY_SIZE], uint32_t entries, uint64_t *index_out) {
    uint64_t index;
    if (dir_find_free(dir, entries, &index) != 0) return -1;
    for (uint32_t i = 0; i < entries; i++) if (dir_write_entry(dir, index + i, set[i]) != 0) return -1;
    *index_out = index;
    return 0;
}

static int exfat_create(struct vfs_node *parent, const char *name, uint32_t type, struct vfs_node **out) {
    struct exfat_node *dir = (struct exfat_node *)vfs_get_fs_data(parent);
    if (!dir || !(dir->attr & EXFAT_ATTR_DIRECTORY) || dir->fs->readonly || type != VFS_NODE_FILE) return -1;
    uint16_t name16[EXFAT_MAX_NAME_UNITS];
    uint32_t len;
    if (utf8_to_utf16(name, name16, &len) != 0 || !valid_new_name(name16, len)) return -1;
    struct exfat_dir_entry exists;
    if (scan_dir(dir, name16, len, 0, &exists) == 0) return -1;
    uint8_t set[EXFAT_MAX_SET_ENTRIES][EXFAT_ENTRY_SIZE];
    uint32_t entries;
    if (build_file_set(dir->fs, name16, len, EXFAT_ATTR_ARCHIVE, 0, 0, 0, 0, set, &entries) != 0) return -1;
    uint64_t index;
    if (write_new_set(dir, (const uint8_t (*)[EXFAT_ENTRY_SIZE])set, entries, &index) != 0) return -1;
    struct exfat_dir_entry de;
    if (scan_dir(dir, name16, len, 0, &de) != 0) return -1;
    if (out) *out = make_vnode(dir, &de);
    return 0;
}

static int mark_set_inactive(struct exfat_node *dir, uint64_t index, uint32_t entries) {
    for (uint32_t i = 0; i < entries; i++) {
        uint8_t e[EXFAT_ENTRY_SIZE];
        if (dir_read_entry(dir, index + i, e) != 0) return -1;
        e[0] &= 0x7Fu;
        if (dir_write_entry(dir, index + i, e) != 0) return -1;
    }
    return 0;
}

static int exfat_unlink(struct vfs_node *parent, const char *name) {
    struct exfat_node *dir = (struct exfat_node *)vfs_get_fs_data(parent);
    if (!dir || !(dir->attr & EXFAT_ATTR_DIRECTORY) || dir->fs->readonly) return -1;
    uint16_t name16[EXFAT_MAX_NAME_UNITS];
    uint32_t len;
    if (utf8_to_utf16(name, name16, &len) != 0) return -1;
    struct exfat_dir_entry de;
    if (scan_dir(dir, name16, len, 0, &de) != 0 || (de.attr & EXFAT_ATTR_DIRECTORY)) return -1;
    if (free_chain(dir->fs, de.first_cluster, de.flags, clusters_for(dir->fs, de.data_length)) != 0) return -1;
    return mark_set_inactive(dir, de.dir_index, (uint32_t)de.secondary_count + 1u);
}

static int exfat_rename(struct vfs_node *old_parent, const char *old_name, struct vfs_node *new_parent, const char *new_name) {
    struct exfat_node *odir = (struct exfat_node *)vfs_get_fs_data(old_parent);
    struct exfat_node *ndir = (struct exfat_node *)vfs_get_fs_data(new_parent);
    if (!odir || !ndir || odir->fs != ndir->fs || odir->fs->readonly) return -1;
    uint16_t old16[EXFAT_MAX_NAME_UNITS], new16[EXFAT_MAX_NAME_UNITS];
    uint32_t old_len, new_len;
    if (utf8_to_utf16(old_name, old16, &old_len) != 0 || utf8_to_utf16(new_name, new16, &new_len) != 0 || !valid_new_name(new16, new_len)) return -1;
    struct exfat_dir_entry oldde, exists;
    if (scan_dir(odir, old16, old_len, 0, &oldde) != 0) return -1;
    if (scan_dir(ndir, new16, new_len, 0, &exists) == 0) return -1;
    uint8_t set[EXFAT_MAX_SET_ENTRIES][EXFAT_ENTRY_SIZE];
    uint32_t entries;
    if (build_file_set(ndir->fs, new16, new_len, oldde.attr, oldde.flags, oldde.first_cluster, oldde.valid_length, oldde.data_length, set, &entries) != 0) return -1;
    uint64_t index;
    if (write_new_set(ndir, (const uint8_t (*)[EXFAT_ENTRY_SIZE])set, entries, &index) != 0) return -1;
    return mark_set_inactive(odir, oldde.dir_index, (uint32_t)oldde.secondary_count + 1u);
}

static int exfat_truncate(struct vfs_node *vnode, uint64_t size) {
    struct exfat_node *n = (struct exfat_node *)vfs_get_fs_data(vnode);
    if (!n || (n->attr & EXFAT_ATTR_DIRECTORY) || n->fs->readonly) return -1;
    uint64_t old = n->valid_length;
    uint32_t need = clusters_for(n->fs, size);
    if (ensure_clusters(n, need) != 0) return -1;
    if (size > old && zero_stream_range(n, old, size - old) != 0) return -1;
    n->data_length = size;
    n->valid_length = size;
    if (trim_clusters(n, need) != 0) return -1;
    vfs_set_size(vnode, size);
    return update_node_entry(n);
}

static int exfat_probe_bpb(struct vfs_node *blockdev, const uint8_t bs[512]) {
    struct exfat_fs fs;
    memset(&fs, 0, sizeof(fs));
    fs.dev = blockdev;
    if (parse_boot(&fs, blockdev, bs, 0) != 0) return FS_PROBE_NO;
    return FS_PROBE_YES;
}

static int exfat_probe(struct vfs_node *blockdev) {
    uint8_t bs[512];
    if (block_read(blockdev, 0, sizeof(bs), bs) != sizeof(bs)) return FS_PROBE_ERR;
    return exfat_probe_bpb(blockdev, bs);
}

static int exfat_mount(struct vfs_node *blockdev, struct vfs_node *mountpoint, const char *flags) {
    uint8_t bs[512];
    if (block_read(blockdev, 0, sizeof(bs), bs) != sizeof(bs)) return -1;
    if (exfat_probe_bpb(blockdev, bs) != FS_PROBE_YES) return -2;
    struct exfat_fs *fs = (struct exfat_fs *)kmalloc(sizeof(*fs));
    if (!fs) return -3;
    memset(fs, 0, sizeof(*fs));
    fs->dev = blockdev;
    if (parse_boot(fs, blockdev, bs, 1) != 0) { kfree(fs); return -4; }
    fs->readonly = mount_flags_rw(flags) ? 0 : 1;
    fs->next_free_cluster = EXFAT_CLUSTER_FIRST;
    if (load_upcase(fs) != 0) { kfree(fs); return -5; }
    if (find_bitmap_ref(fs) != 0) { kfree(fs->upcase); kfree(fs); return -7; }
    fs->bitmap_cache = (uint8_t *)kmalloc(fs->cluster_size);
    if (!fs->bitmap_cache) { kfree(fs->upcase); kfree(fs); return -8; }
    struct exfat_node *root = (struct exfat_node *)kmalloc(sizeof(*root));
    if (!root) { kfree(fs->bitmap_cache); kfree(fs->upcase); kfree(fs); return -6; }
    memset(root, 0, sizeof(*root));
    root->fs = fs;
    root->first_cluster = fs->root_cluster;
    root->attr = EXFAT_ATTR_DIRECTORY;
    root->is_root = 1;
    vfs_set_fs_data(mountpoint, root);
    attach_ops(mountpoint, VFS_NODE_DIRECTORY);
    vfs_set_size(mountpoint, 0);
    klog(LOG_INFO, "exfat: mounted clusters=%d cluster_size=%d fat=%d %s\n", fs->cluster_count, fs->cluster_size, fs->active_fat_offset, fs->readonly ? "ro" : "rw");
    return 0;
}

static void exfat_copy_text(char *dst, uint64_t cap, const char *src) {
    if (!dst || !cap) return;
    uint64_t i = 0;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int exfat_info(struct vfs_node *mountpoint, struct fs_info *out) {
    if (!mountpoint || !out) return -1;
    struct exfat_node *root = (struct exfat_node *)vfs_get_fs_data(mountpoint);
    if (!root || !root->fs) return -1;
    struct exfat_fs *fs = root->fs;
    memset(out, 0, sizeof(*out));
    out->version = FS_INFO_VERSION;
    out->block_size = fs->cluster_size;
    out->total_bytes = (uint64_t)fs->cluster_count * fs->cluster_size;
    out->max_file_size = out->total_bytes;
    if (fs->readonly) out->flags |= FS_INFO_FLAG_READONLY;
    exfat_copy_text(out->driver, sizeof(out->driver), "exfat");
    exfat_copy_text(out->type, sizeof(out->type), "exFAT");

    if (!fs->free_count_valid) {
        uint32_t free_clusters = 0;
        for (uint32_t c = EXFAT_CLUSTER_FIRST; c < fs->cluster_count + EXFAT_CLUSTER_FIRST; c++) {
            uint8_t used = 0;
            if (bitmap_get(fs, c, &used) != 0) return -1;
            if (!used) free_clusters++;
        }
        fs->free_clusters = free_clusters;
        fs->free_count_valid = 1;
    }
    out->free_bytes = (uint64_t)fs->free_clusters * fs->cluster_size;
    out->used_bytes = out->total_bytes >= out->free_bytes ? out->total_bytes - out->free_bytes : 0;
    return 0;
}

static int exfat_unmount(struct vfs_node *mountpoint) {
    struct exfat_node *root = (struct exfat_node *)vfs_get_fs_data(mountpoint);
    if (!root) return -1;
    if (root->fs) {
        if (root->fs->bitmap_cache) kfree(root->fs->bitmap_cache);
        if (root->fs->upcase) kfree(root->fs->upcase);
        kfree(root->fs);
    }
    kfree(root);
    return 0;
}

static struct fs_driver exfat_driver = { .name = "exfat", .mount = exfat_mount, .next = 0, .probe = exfat_probe, .unmount = exfat_unmount, .info = exfat_info };
static int exfat_init(void) { return fs_register(&exfat_driver); }
MODULE_INFO("exfat", exfat_init, 0, 0, "fs");
