#!/usr/bin/env python3
import argparse
import struct

SECTOR = 512
SPC = 8
CLUSTER = SECTOR * SPC
FAT_OFFSET = 24
FAT_LENGTH = 32
HEAP_OFFSET = 64
ROOT_CLUSTER = 2
BITMAP_CLUSTER = 3
UPCASE_CLUSTER = 4


def le16(v): return struct.pack('<H', v)
def le32(v): return struct.pack('<I', v)
def le64(v): return struct.pack('<Q', v)


def csum32(data):
    c = 0
    for b in data:
        c = ((0x80000000 if c & 1 else 0) + (c >> 1) + b) & 0xffffffff
    return c


def csum16(data, skip_primary_checksum=False):
    c = 0
    for i, b in enumerate(data):
        if skip_primary_checksum and (i == 2 or i == 3):
            continue
        c = ((0x8000 if c & 1 else 0) + (c >> 1) + b) & 0xffff
    return c


def upcase_blob():
    mapping = list(range(65536))
    for c in range(ord('a'), ord('z') + 1):
        mapping[c] = c - 32
    out = bytearray()
    i = 0
    while i < 65536:
        if mapping[i] == i:
            j = i
            while j < 65536 and mapping[j] == j and j - i < 0xffff:
                j += 1
            out += le16(0xffff) + le16(j - i)
            i = j
        else:
            out += le16(mapping[i])
            i += 1
    return bytes(out)


def name_hash(name):
    h = 0
    for ch in name:
        code = ord(ch)
        if ord('a') <= code <= ord('z'):
            code -= 32
        for b in le16(code):
            h = ((0x8000 if h & 1 else 0) + (h >> 1) + b) & 0xffff
    return h


def file_set(name, first_cluster, content_len, attr=0x20):
    units = list(name.encode('utf-16le'))
    chars = [units[i] | (units[i + 1] << 8) for i in range(0, len(units), 2)]
    name_entries = (len(chars) + 14) // 15
    primary = bytearray(32)
    primary[0] = 0x85
    primary[1] = 1 + name_entries
    primary[4:6] = le16(attr)
    stream = bytearray(32)
    stream[0] = 0xc0
    stream[1] = 0x03
    stream[3] = len(chars)
    stream[4:6] = le16(name_hash(name))
    stream[8:16] = le64(content_len)
    stream[20:24] = le32(first_cluster if content_len else 0)
    stream[24:32] = le64(content_len)
    entries = [primary, stream]
    for i in range(name_entries):
        ent = bytearray(32)
        ent[0] = 0xc1
        for j, ch in enumerate(chars[i * 15:(i + 1) * 15]):
            ent[2 + j * 2:4 + j * 2] = le16(ch)
        entries.append(ent)
    joined = bytearray().join(entries)
    primary[2:4] = le16(csum16(joined, True))
    return bytearray().join(entries)


def bitmap_entry(first_cluster, byte_len):
    e = bytearray(32)
    e[0] = 0x81
    e[1] = 0
    e[20:24] = le32(first_cluster)
    e[24:32] = le64(byte_len)
    return e


def upcase_entry(first_cluster, blob):
    e = bytearray(32)
    e[0] = 0x82
    e[4:8] = le32(csum32(blob))
    e[20:24] = le32(first_cluster)
    e[24:32] = le64(len(blob))
    return e


def set_bitmap_bit(bitmap, cluster):
    bit = cluster - 2
    bitmap[bit // 8] |= 1 << (bit % 8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--size-mib', type=int, default=16)
    args = ap.parse_args()

    total_sectors = args.size_mib * 1024 * 1024 // SECTOR
    cluster_count = (total_sectors - HEAP_OFFSET) // SPC
    upcase = upcase_blob()
    upcase_clusters = (len(upcase) + CLUSTER - 1) // CLUSTER
    readme = b'exfat driver smoke test\n'
    sub = b'nested file\n'
    readme_cluster = UPCASE_CLUSTER + upcase_clusters
    dir_cluster = readme_cluster + 1
    nested_cluster = dir_cluster + 1
    bitmap_bytes = (cluster_count + 7) // 8

    img = bytearray(total_sectors * SECTOR)
    bs = bytearray(SECTOR)
    bs[0:3] = b'\xeb\x76\x90'
    bs[3:11] = b'EXFAT   '
    bs[64:72] = le64(0)
    bs[72:80] = le64(total_sectors)
    bs[80:84] = le32(FAT_OFFSET)
    bs[84:88] = le32(FAT_LENGTH)
    bs[88:92] = le32(HEAP_OFFSET)
    bs[92:96] = le32(cluster_count)
    bs[96:100] = le32(ROOT_CLUSTER)
    bs[100:104] = le32(0x12345678)
    bs[104:106] = le16(0x0100)
    bs[106:108] = le16(0)
    bs[108] = 9
    bs[109] = 3
    bs[110] = 1
    bs[111] = 0x80
    bs[112] = 0
    bs[510:512] = b'\x55\xaa'
    img[0:SECTOR] = bs
    img[12 * SECTOR:13 * SECTOR] = bs

    fat = bytearray(FAT_LENGTH * SECTOR)
    def fat_set(c, v):
        fat[c * 4:c * 4 + 4] = le32(v)
    fat_set(0, 0xfffffff8)
    fat_set(1, 0xffffffff)
    fat_set(ROOT_CLUSTER, 0xffffffff)
    fat_set(BITMAP_CLUSTER, 0xffffffff)
    for i in range(upcase_clusters):
        c = UPCASE_CLUSTER + i
        fat_set(c, 0xffffffff if i + 1 == upcase_clusters else c + 1)
    fat_set(dir_cluster, 0xffffffff)
    img[FAT_OFFSET * SECTOR:(FAT_OFFSET + FAT_LENGTH) * SECTOR] = fat

    bitmap = bytearray(bitmap_bytes)
    for c in [ROOT_CLUSTER, BITMAP_CLUSTER, readme_cluster, dir_cluster, nested_cluster]:
        set_bitmap_bit(bitmap, c)
    for c in range(UPCASE_CLUSTER, UPCASE_CLUSTER + upcase_clusters):
        set_bitmap_bit(bitmap, c)

    def cluster_off(c):
        return (HEAP_OFFSET + (c - 2) * SPC) * SECTOR
    img[cluster_off(BITMAP_CLUSTER):cluster_off(BITMAP_CLUSTER) + len(bitmap)] = bitmap
    img[cluster_off(UPCASE_CLUSTER):cluster_off(UPCASE_CLUSTER) + len(upcase)] = upcase
    img[cluster_off(readme_cluster):cluster_off(readme_cluster) + len(readme)] = readme
    img[cluster_off(nested_cluster):cluster_off(nested_cluster) + len(sub)] = sub

    root = bytearray(CLUSTER)
    p = 0
    for e in [bitmap_entry(BITMAP_CLUSTER, len(bitmap)), upcase_entry(UPCASE_CLUSTER, upcase),
              file_set('README.TXT', readme_cluster, len(readme)),
              file_set('MixedCase.bin', nested_cluster, len(sub)),
              file_set('DIR', dir_cluster, CLUSTER, 0x10)]:
        root[p:p + len(e)] = e
        p += len(e)
    img[cluster_off(ROOT_CLUSTER):cluster_off(ROOT_CLUSTER) + CLUSTER] = root

    subdir = bytearray(CLUSTER)
    subdir[0:len(file_set('CHILD.TXT', nested_cluster, len(sub)))] = file_set('CHILD.TXT', nested_cluster, len(sub))
    img[cluster_off(dir_cluster):cluster_off(dir_cluster) + CLUSTER] = subdir

    vbr = img[:11 * SECTOR]
    s = 0
    for i, b in enumerate(vbr):
        if i in (106, 107, 112):
            continue
        s = ((0x80000000 if s & 1 else 0) + (s >> 1) + b) & 0xffffffff
    checksum_sector = le32(s) * (SECTOR // 4)
    img[11 * SECTOR:12 * SECTOR] = checksum_sector
    img[23 * SECTOR:24 * SECTOR] = checksum_sector

    with open(args.image, 'wb') as f:
        f.write(img)

if __name__ == '__main__':
    main()
