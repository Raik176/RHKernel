#!/usr/bin/env python3
from __future__ import annotations

import argparse
import math
import uuid
import zlib
from pathlib import Path

BS = 1024
DISK_SECTOR = 512
GPT_ENTRY_SIZE = 128
GPT_ENTRY_COUNT = 128
GPT_HEADER_SIZE = 92
LINUX_FS_GUID = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")
DISK_GUID = uuid.UUID("11112222-3333-4444-5555-666677778888")

S_IFDIR = 0o040000
S_IFCHR = 0o020000
S_IFBLK = 0o060000
S_IFREG = 0o100000
S_IFLNK = 0o120000

M_V1_14 = 0x137F
M_V1_30 = 0x138F
M_V2_14 = 0x2468
M_V2_30 = 0x2478
M_V3 = 0x4D5A


def le16(v): return v.to_bytes(2, "little")
def le32(v): return v.to_bytes(4, "little")
def be16(v): return v.to_bytes(2, "big")
def be32(v): return v.to_bytes(4, "big")
def e16(v, swapped): return be16(v) if swapped else le16(v)
def e32(v, swapped): return be32(v) if swapped else le32(v)
def align_up(v, a): return (v + a - 1) // a * a

def crc32(data): return zlib.crc32(data) & 0xffffffff

def gpt_name(s: str) -> bytes:
    return s.encode("utf-16-le")[:72].ljust(72, b"\0")

class Image:
    def __init__(self, version: int, name_len: int, swapped: bool):
        self.version = version
        self.name_len = name_len
        self.swapped = swapped
        self.inode_size = 32 if version == 1 else 64
        self.dirent_size = 64 if version == 3 else name_len + 2
        if version == 3:
            self.name_len = 60
        self.ninodes = 64
        self.blocks = 2048
        self.imap_blocks = 1
        self.zmap_blocks = 1
        inode_blocks = align_up(self.ninodes * self.inode_size, BS) // BS
        self.firstdatazone = 2 + self.imap_blocks + self.zmap_blocks + inode_blocks
        self.img = bytearray(self.blocks * BS)
        self.next_ino = 1
        self.next_zone = self.firstdatazone
        self.used_inodes = set()
        self.used_zones = set()

    def alloc_ino(self):
        self.next_ino += 1
        ino = self.next_ino - 1
        self.used_inodes.add(ino)
        return ino

    def alloc_zone(self, data: bytes = b""):
        z = self.next_zone
        self.next_zone += 1
        self.used_zones.add(z)
        off = z * BS
        self.img[off:off + min(len(data), BS)] = data[:BS]
        return z

    def set_bitmap(self, off, bit):
        self.img[off + bit // 8] |= 1 << (bit & 7)

    def write_super(self):
        sb = bytearray(64)
        if self.version in (1, 2):
            magic = M_V1_14 if self.version == 1 and self.name_len == 14 else M_V1_30 if self.version == 1 else M_V2_14 if self.name_len == 14 else M_V2_30
            sb[0:2] = e16(self.ninodes, self.swapped)
            sb[2:4] = e16(self.blocks, self.swapped)
            sb[4:6] = e16(self.imap_blocks, self.swapped)
            sb[6:8] = e16(self.zmap_blocks, self.swapped)
            sb[8:10] = e16(self.firstdatazone, self.swapped)
            sb[10:12] = e16(0, self.swapped)
            sb[12:16] = e32(0xffffffff, self.swapped)
            sb[16:18] = e16(magic, self.swapped)
            sb[18:20] = e16(0, self.swapped)
            sb[20:24] = e32(self.blocks, self.swapped)
        else:
            sb[0:4] = e32(self.ninodes, self.swapped)
            sb[6:8] = e16(self.imap_blocks, self.swapped)
            sb[8:10] = e16(self.zmap_blocks, self.swapped)
            sb[10:12] = e16(self.firstdatazone, self.swapped)
            sb[12:14] = e16(0, self.swapped)
            sb[14:18] = e32(0xffffffff, self.swapped)
            sb[20:24] = e32(self.blocks, self.swapped)
            sb[24:26] = e16(M_V3, self.swapped)
            sb[28:30] = e16(BS, self.swapped)
            sb[30] = 3
        self.img[BS:BS + len(sb)] = sb

    def inode_off(self, ino):
        return (2 + self.imap_blocks + self.zmap_blocks) * BS + (ino - 1) * self.inode_size

    def write_inode(self, ino, mode, size, zones, links=1):
        raw = bytearray(self.inode_size)
        if self.version == 1:
            raw[0:2] = e16(mode, self.swapped)
            raw[2:4] = e16(0, self.swapped)
            raw[4:8] = e32(size, self.swapped)
            raw[12] = 0
            raw[13] = links
            for i, z in enumerate(zones[:9]):
                raw[14 + i * 2:16 + i * 2] = e16(z, self.swapped)
        else:
            raw[0:2] = e16(mode, self.swapped)
            raw[2:4] = e16(links, self.swapped)
            raw[4:6] = e16(0, self.swapped)
            raw[6:8] = e16(0, self.swapped)
            raw[8:12] = e32(size, self.swapped)
            for i, z in enumerate(zones[:10]):
                raw[24 + i * 4:28 + i * 4] = e32(z, self.swapped)
        self.img[self.inode_off(ino):self.inode_off(ino) + self.inode_size] = raw

    def dirent(self, ino, name):
        raw = bytearray(self.dirent_size)
        if self.version == 3:
            raw[0:4] = e32(ino, self.swapped)
            pos = 4
        else:
            raw[0:2] = e16(ino, self.swapped)
            pos = 2
        enc = name.encode("ascii")
        raw[pos:pos + min(len(enc), self.name_len)] = enc[:self.name_len]
        return bytes(raw)

    def write_bitmaps(self):
        self.set_bitmap(2 * BS, 0)
        for ino in self.used_inodes:
            self.set_bitmap(2 * BS, ino)
        zmap = (2 + self.imap_blocks) * BS
        for z in self.used_zones:
            self.set_bitmap(zmap, z - self.firstdatazone)

    def make(self):
        self.write_super()
        root = self.alloc_ino()
        hello = self.alloc_ino()
        subdir = self.alloc_ino()
        child = self.alloc_ino()
        link = self.alloc_ino()
        chrdev = self.alloc_ino()
        blkdev = self.alloc_ino()
        created = self.alloc_ino()
        hello_data = f"minix v{self.version} name{self.name_len} {'swapped' if self.swapped else 'native'} hello\n".encode()
        child_data = b"nested minix child\n"
        created_data = b"seed file before mutation\n"
        hello_z = self.alloc_zone(hello_data)
        child_z = self.alloc_zone(child_data)
        created_z = self.alloc_zone(created_data)
        link_data = b"hello.txt"
        link_z = self.alloc_zone(link_data)
        self.write_inode(hello, S_IFREG | 0o644, len(hello_data), [hello_z])
        self.write_inode(child, S_IFREG | 0o644, len(child_data), [child_z])
        self.write_inode(created, S_IFREG | 0o644, len(created_data), [created_z])
        self.write_inode(link, S_IFLNK | 0o777, len(link_data), [link_z])
        self.write_inode(chrdev, S_IFCHR | 0o600, 0, [])
        self.write_inode(blkdev, S_IFBLK | 0o600, 0, [])
        subents = [self.dirent(subdir, "."), self.dirent(root, ".."), self.dirent(child, "child.txt")]
        sub_z = self.alloc_zone(b"".join(subents))
        self.write_inode(subdir, S_IFDIR | 0o755, len(subents) * self.dirent_size, [sub_z], 2)
        entries = [
            self.dirent(root, "."), self.dirent(root, ".."), self.dirent(hello, "hello.txt"),
            self.dirent(subdir, "dir"), self.dirent(link, "hello.lnk"), self.dirent(chrdev, "char0"),
            self.dirent(blkdev, "block0"), self.dirent(created, "seed.txt"),
        ]
        root_z = self.alloc_zone(b"".join(entries))
        self.write_inode(root, S_IFDIR | 0o755, len(entries) * self.dirent_size, [root_z], 2)
        self.write_bitmaps()
        return bytes(self.img)


def partition_guid(i):
    return uuid.UUID(f"aaaaaaaa-bbbb-cccc-dddd-{i + 1:012x}")


def write_gpt_header(img: bytearray, lba: int, alternate_lba: int, first_usable: int,
                     last_usable: int, entries_lba: int, entries_crc: int) -> None:
    h = bytearray(DISK_SECTOR)
    h[0:8] = b"EFI PART"
    h[8:12] = le32(0x00010000)
    h[12:16] = le32(GPT_HEADER_SIZE)
    h[24:32] = lba.to_bytes(8, "little")
    h[32:40] = alternate_lba.to_bytes(8, "little")
    h[40:48] = first_usable.to_bytes(8, "little")
    h[48:56] = last_usable.to_bytes(8, "little")
    h[56:72] = DISK_GUID.bytes_le
    h[72:80] = entries_lba.to_bytes(8, "little")
    h[80:84] = le32(GPT_ENTRY_COUNT)
    h[84:88] = le32(GPT_ENTRY_SIZE)
    h[88:92] = le32(entries_crc)
    h[16:20] = le32(crc32(h[:GPT_HEADER_SIZE]))
    img[lba * DISK_SECTOR:(lba + 1) * DISK_SECTOR] = h


def make_gpt(parts: list[tuple[str, bytes]]) -> bytes:
    entries_sectors = align_up(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE, DISK_SECTOR) // DISK_SECTOR
    first_usable = 2 + entries_sectors
    cursor = align_up(first_usable, 2048)
    layout = []

    for name, data in parts:
        count = align_up(len(data), DISK_SECTOR) // DISK_SECTOR
        layout.append((name, cursor, count, data))
        cursor = align_up(cursor + count, 2048)

    backup_entries_lba = cursor
    last_usable = backup_entries_lba - 1
    total = backup_entries_lba + entries_sectors + 1
    disk = bytearray(total * DISK_SECTOR)

    disk[446:462] = bytes([0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF]) + le32(1) + le32(min(total - 1, 0xFFFFFFFF))
    disk[510:512] = b"\x55\xaa"

    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    for i, (name, start, count, data) in enumerate(layout):
        off = i * GPT_ENTRY_SIZE
        entries[off:off + 16] = LINUX_FS_GUID.bytes_le
        entries[off + 16:off + 32] = partition_guid(i).bytes_le
        entries[off + 32:off + 40] = start.to_bytes(8, "little")
        entries[off + 40:off + 48] = (start + count - 1).to_bytes(8, "little")
        entries[off + 56:off + 128] = gpt_name(name)
        begin = start * DISK_SECTOR
        disk[begin:begin + len(data)] = data

    entries_crc = crc32(entries)
    disk[2 * DISK_SECTOR:(2 + entries_sectors) * DISK_SECTOR] = entries
    disk[backup_entries_lba * DISK_SECTOR:(backup_entries_lba + entries_sectors) * DISK_SECTOR] = entries
    write_gpt_header(disk, 1, total - 1, first_usable, last_usable, 2, entries_crc)
    write_gpt_header(disk, total - 1, 1, first_usable, last_usable, backup_entries_lba, entries_crc)
    return bytes(disk)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir", type=Path)
    args = ap.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)
    specs = [
        ("minix-v1-14.img", "minix_v1_14", 1, 14, False),
        ("minix-v1-30-swapped.img", "minix_v1_30_swapped", 1, 30, True),
        ("minix-v2-14.img", "minix_v2_14", 2, 14, False),
        ("minix-v2-30.img", "minix_v2_30", 2, 30, False),
        ("minix-v3.img", "minix_v3", 3, 60, False),
        ("minix-v3-swapped.img", "minix_v3_swapped", 3, 60, True),
    ]
    parts = []
    lines = []
    for file_name, part_name, version, name_len, swapped in specs:
        data = Image(version, name_len, swapped).make()
        (args.outdir / file_name).write_bytes(data)
        parts.append((part_name, data))
        lines.append(f"{part_name}: {file_name}")
    (args.outdir / "minix-partitions.img").write_bytes(make_gpt(parts))
    (args.outdir / "README.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

if __name__ == "__main__":
    main()
