#!/usr/bin/env python3
import argparse
import math
import os
import struct
import sys

BPS = 512
MEDIA_FIXED = 0xF8

DEFAULT_SIZES = {"fat12": 1440 * 1024, "fat16": 16 * 1024 * 1024, "fat32": 64 * 1024 * 1024}


def parse_size(s):
    if s is None:
        return None
    u = s[-1].upper()
    n = int(s[:-1] if u in "KMG" else s)
    if u == "K":
        return n * 1024
    if u == "M":
        return n * 1024 * 1024
    if u == "G":
        return n * 1024 * 1024 * 1024
    return n


def le16(v): return struct.pack("<H", v)
def le32(v): return struct.pack("<I", v)


def checksum(sfn):
    s = 0
    for b in sfn:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def sfn(name11):
    b = name11.encode("ascii")
    if len(b) != 11:
        raise ValueError(name11)
    return b


def short_entry(name11, attr, cluster, size):
    e = bytearray(32)
    e[0:11] = sfn(name11)
    e[11] = attr
    e[20:22] = le16((cluster >> 16) & 0xFFFF)
    e[26:28] = le16(cluster & 0xFFFF)
    e[28:32] = le32(size)
    return bytes(e)


def lfn_entries(long_name, name11):
    units = [ord(c) for c in long_name]
    units.append(0)
    while len(units) % 13:
        units.append(0xFFFF)
    slots = len(units) // 13
    if slots < 1 or slots > 20:
        raise ValueError("LFN too long")
    out = []
    csum = checksum(sfn(name11))
    positions = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    for ordv in range(slots, 0, -1):
        e = bytearray(32)
        e[0] = ordv | (0x40 if ordv == slots else 0)
        e[11] = 0x0F
        e[12] = 0
        e[13] = csum
        e[26:28] = b"\0\0"
        chunk = units[(ordv - 1) * 13:ordv * 13]
        for pos, ch in zip(positions, chunk):
            e[pos:pos + 2] = le16(ch)
        out.append(bytes(e))
    return out


class FatBuilder:
    def __init__(self, kind, size):
        self.kind = kind
        self.size = size
        self.total_sectors = size // BPS
        if size % BPS or self.total_sectors < 128:
            raise ValueError("size must be a sane multiple of 512")
        if kind == "fat12":
            self.spc, self.reserved, self.root_entries = 1, 1, 224
            self.bits = 12
        elif kind == "fat16":
            self.spc, self.reserved, self.root_entries = 4, 1, 512
            self.bits = 16
        else:
            self.spc, self.reserved, self.root_entries = 1, 32, 0
            self.bits = 32
        self.fats = 2
        self.root_dir_sectors = (self.root_entries * 32 + BPS - 1) // BPS
        self.fat_sectors, self.cluster_count = self._layout()
        if kind == "fat12" and not self.cluster_count < 4085:
            raise ValueError("FAT12 cluster count out of range")
        if kind == "fat16" and not (4085 <= self.cluster_count < 65525):
            raise ValueError("FAT16 cluster count out of range")
        if kind == "fat32" and not self.cluster_count >= 65525:
            raise ValueError("FAT32 cluster count out of range")
        self.first_data_sector = self.reserved + self.fats * self.fat_sectors + self.root_dir_sectors
        self.root_dir_sector = self.reserved + self.fats * self.fat_sectors
        self.image = bytearray(size)
        self.fat = [0] * (self.cluster_count + 2)
        self.fat[0] = (0xFFFFFF00 if self.bits == 32 else 0xFF00) | MEDIA_FIXED
        self.fat[1] = self.eoc()
        self.next_cluster = 2
        self.root_cluster = 0
        if self.bits == 32:
            self.root_cluster = self.alloc_clusters(1)[0]

    def _layout(self):
        fat_sectors = 1
        while True:
            data = self.total_sectors - self.reserved - self.fats * fat_sectors - self.root_dir_sectors
            if data <= 0:
                raise ValueError("image too small")
            clusters = data // self.spc
            fat_bytes = (clusters + 2) * self.bits
            fat_sectors_new = (fat_bytes + 7) // 8
            fat_sectors_new = (fat_sectors_new + BPS - 1) // BPS
            if fat_sectors_new <= fat_sectors:
                return fat_sectors, clusters
            fat_sectors = fat_sectors_new

    def eoc(self):
        if self.bits == 12:
            return 0xFFF
        if self.bits == 16:
            return 0xFFFF
        return 0x0FFFFFFF

    def alloc_clusters(self, count):
        if count <= 0:
            return []
        start = self.next_cluster
        end = start + count
        if end > len(self.fat):
            raise ValueError("out of clusters")
        clusters = list(range(start, end))
        for i, c in enumerate(clusters):
            self.fat[c] = clusters[i + 1] if i + 1 < len(clusters) else self.eoc()
        self.next_cluster = end
        return clusters

    def cluster_offset(self, c):
        sector = self.first_data_sector + (c - 2) * self.spc
        return sector * BPS

    def write_cluster_chain(self, data):
        clusters = self.alloc_clusters(max(1, math.ceil(len(data) / (self.spc * BPS))))
        for i, c in enumerate(clusters):
            off = self.cluster_offset(c)
            chunk = data[i * self.spc * BPS:(i + 1) * self.spc * BPS]
            self.image[off:off + len(chunk)] = chunk
        return clusters[0]

    def write_dir_cluster(self, entries):
        data = b"".join(entries)
        if len(data) > self.spc * BPS:
            raise ValueError("test directory too large")
        c = self.write_cluster_chain(data + b"\0" * (self.spc * BPS - len(data)))
        return c

    def set_fat_bytes(self):
        fat_bytes = bytearray(self.fat_sectors * BPS)
        if self.bits == 12:
            for i in range(0, len(self.fat), 2):
                a = self.fat[i] & 0xFFF
                b = self.fat[i + 1] & 0xFFF if i + 1 < len(self.fat) else 0
                j = i + i // 2
                if j + 2 < len(fat_bytes):
                    fat_bytes[j] = a & 0xFF
                    fat_bytes[j + 1] = ((a >> 8) & 0x0F) | ((b & 0x0F) << 4)
                    fat_bytes[j + 2] = (b >> 4) & 0xFF
        elif self.bits == 16:
            for i, v in enumerate(self.fat):
                j = i * 2
                if j + 1 < len(fat_bytes):
                    fat_bytes[j:j + 2] = le16(v & 0xFFFF)
        else:
            for i, v in enumerate(self.fat):
                j = i * 4
                if j + 3 < len(fat_bytes):
                    fat_bytes[j:j + 4] = le32(v & 0x0FFFFFFF)
        for n in range(self.fats):
            off = (self.reserved + n * self.fat_sectors) * BPS
            self.image[off:off + len(fat_bytes)] = fat_bytes

    def boot_sector(self):
        bs = bytearray(512)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"SYM VFAT"
        bs[11:13] = le16(BPS)
        bs[13] = self.spc
        bs[14:16] = le16(self.reserved)
        bs[16] = self.fats
        bs[17:19] = le16(self.root_entries)
        if self.total_sectors < 65536:
            bs[19:21] = le16(self.total_sectors)
        bs[21] = MEDIA_FIXED
        if self.bits != 32:
            bs[22:24] = le16(self.fat_sectors)
        bs[24:26] = le16(63)
        bs[26:28] = le16(255)
        if self.total_sectors >= 65536:
            bs[32:36] = le32(self.total_sectors)
        if self.bits == 32:
            bs[36:40] = le32(self.fat_sectors)
            bs[40:42] = le16(0)
            bs[42:44] = le16(0)
            bs[44:48] = le32(self.root_cluster)
            bs[48:50] = le16(1)
            bs[50:52] = le16(6)
            bs[64] = 0x80
            bs[66] = 0x29
            bs[67:71] = le32(0x53465432)
            bs[71:82] = b"NO NAME    "
            bs[82:90] = b"FAT32   "
        else:
            bs[36] = 0x80
            bs[38] = 0x29
            bs[39:43] = le32(0x53465412 if self.bits == 12 else 0x53465416)
            bs[43:54] = b"NO NAME    "
            bs[54:62] = b"FAT12   " if self.bits == 12 else b"FAT16   "
        bs[510:512] = b"\x55\xAA"
        self.image[0:512] = bs
        if self.bits == 32:
            fsinfo = bytearray(512)
            fsinfo[0:4] = le32(0x41615252)
            fsinfo[484:488] = le32(0x61417272)
            fsinfo[488:492] = le32(0xFFFFFFFF)
            fsinfo[492:496] = le32(0xFFFFFFFF)
            fsinfo[508:512] = le32(0xAA550000)
            self.image[BPS:2 * BPS] = fsinfo
            self.image[6 * BPS:7 * BPS] = bs

    def finish(self, root_entries):
        self.boot_sector()
        if self.bits == 32:
            root = b"".join(root_entries)
            off = self.cluster_offset(self.root_cluster)
            self.image[off:off + len(root)] = root
        else:
            root = b"".join(root_entries)
            max_bytes = self.root_dir_sectors * BPS
            if len(root) > max_bytes:
                raise ValueError("root too large")
            off = self.root_dir_sector * BPS
            self.image[off:off + len(root)] = root
        self.set_fat_bytes()


def add_file(builder, long_name, name11, payload):
    first = builder.write_cluster_chain(payload)
    return lfn_entries(long_name, name11) + [short_entry(name11, 0x20, first, len(payload))]


def build(kind, size):
    b = FatBuilder(kind, size)
    tag = kind.upper().encode("ascii")
    readme = b"README on " + kind.encode("ascii") + b"\n"
    long_payload = b"This file is addressed through a VFAT long file name on " + tag + b".\n"
    child_payload = b"Nested VFAT LFN child on " + tag + b".\n"
    child_first = b.write_cluster_chain(child_payload)
    nested_cluster = b.alloc_clusters(1)[0]
    nested_entries = [
        short_entry(".          ", 0x10, nested_cluster, 0),
        short_entry("..         ", 0x10, 0 if b.bits != 32 else b.root_cluster, 0),
    ]
    nested_entries += lfn_entries("Child Long Name.txt", "CHILD~1 TXT")
    nested_entries.append(short_entry("CHILD~1 TXT", 0x20, child_first, len(child_payload)))
    nested_data = b"".join(nested_entries)
    b.image[b.cluster_offset(nested_cluster):b.cluster_offset(nested_cluster) + len(nested_data)] = nested_data

    root = []
    root += [short_entry("README  TXT", 0x20, b.write_cluster_chain(readme), len(readme))]
    root += add_file(b, f"Long File Name {kind.upper()}.txt", "LONGFI~1TXT", long_payload)
    root += lfn_entries("Nested Directory", "NESTED~1   ")
    root += [short_entry("NESTED~1   ", 0x10, nested_cluster, 0)]
    b.finish(root)
    return b.image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kind", choices=["fat12", "fat16", "fat32"])
    ap.add_argument("output")
    ap.add_argument("size", nargs="?")
    args = ap.parse_args()
    size = parse_size(args.size) or DEFAULT_SIZES[args.kind]
    img = build(args.kind, size)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(img)

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"mk_fat_disk.py: {e}", file=sys.stderr)
        sys.exit(1)
