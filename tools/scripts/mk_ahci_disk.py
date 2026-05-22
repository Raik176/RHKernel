#!/usr/bin/env python3
import os
import struct
import sys
import uuid
import zlib

SECTOR_SIZE = 512
GPT_ENTRY_SIZE = 128
GPT_ENTRY_COUNT = 128
GPT_HEADER_SIZE = 92
LINUX_FS_GUID = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4')


def parse_size(s):
    s = s.strip().upper()
    mul = 1
    if s.endswith('K'):
        mul, s = 1024, s[:-1]
    elif s.endswith('M'):
        mul, s = 1024 * 1024, s[:-1]
    elif s.endswith('G'):
        mul, s = 1024 * 1024 * 1024, s[:-1]
    return int(s) * mul


def crc32(data):
    return zlib.crc32(data) & 0xffffffff


def mbr_part(ptype, start, count, boot=0):
    if boot not in (0, 0x80):
        raise ValueError('invalid MBR boot flag')
    if start <= 0 or count <= 0 or start + count > 0x100000000:
        raise ValueError('invalid MBR range')
    return bytes([boot, 0, 2, 0, ptype, 0xff, 0xff, 0xff]) + struct.pack('<II', start, count)


def write_mbr(img, sectors):
    first_count = min(63488, sectors - 2048)
    second_count = min(65536, sectors - 65536)
    if first_count <= 0 or second_count <= 0:
        raise SystemExit('MBR test disk size is too small')
    img[446:462] = mbr_part(0x83, 2048, first_count)
    img[462:478] = mbr_part(0x83, 65536, second_count)
    img[510:512] = b'\x55\xaa'


def gpt_guid_bytes(g):
    return g.bytes_le


def gpt_name(text):
    return text.encode('utf-16le')[:72].ljust(72, b'\0')


def write_gpt_header(img, lba, alternate_lba, first_usable, last_usable, disk_guid,
                     entries_lba, entries_crc, sectors):
    header = bytearray(SECTOR_SIZE)
    header[0:8] = b'EFI PART'
    struct.pack_into('<I', header, 8, 0x00010000)
    struct.pack_into('<I', header, 12, GPT_HEADER_SIZE)
    struct.pack_into('<Q', header, 24, lba)
    struct.pack_into('<Q', header, 32, alternate_lba)
    struct.pack_into('<Q', header, 40, first_usable)
    struct.pack_into('<Q', header, 48, last_usable)
    header[56:72] = gpt_guid_bytes(disk_guid)
    struct.pack_into('<Q', header, 72, entries_lba)
    struct.pack_into('<I', header, 80, GPT_ENTRY_COUNT)
    struct.pack_into('<I', header, 84, GPT_ENTRY_SIZE)
    struct.pack_into('<I', header, 88, entries_crc)
    struct.pack_into('<I', header, 16, crc32(header[:GPT_HEADER_SIZE]))
    img[lba * SECTOR_SIZE:(lba + 1) * SECTOR_SIZE] = header


def write_gpt(img, sectors):
    entries_sectors = (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE + SECTOR_SIZE - 1) // SECTOR_SIZE
    first_usable = 2 + entries_sectors
    backup_entries_lba = sectors - 1 - entries_sectors
    last_usable = backup_entries_lba - 1
    if sectors < 2 * entries_sectors + 34 or last_usable <= first_usable:
        raise SystemExit('GPT test disk size is too small')

    img[446:462] = mbr_part(0xEE, 1, min(sectors - 1, 0xffffffff))
    img[510:512] = b'\x55\xaa'

    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    parts = [
        (first_usable, min(first_usable + 65535, last_usable), 'gpt-test-1'),
        (min(first_usable + 65536, last_usable), min(first_usable + 131071, last_usable), 'gpt-test-2'),
    ]
    for idx, (first, last, name) in enumerate(parts):
        if last < first:
            continue
        off = idx * GPT_ENTRY_SIZE
        entries[off:off + 16] = gpt_guid_bytes(LINUX_FS_GUID)
        entries[off + 16:off + 32] = gpt_guid_bytes(uuid.uuid5(uuid.NAMESPACE_DNS, name))
        struct.pack_into('<QQQ', entries, off + 32, first, last, 0)
        entries[off + 56:off + 128] = gpt_name(name)

    entries_crc = crc32(entries)
    img[2 * SECTOR_SIZE:(2 + entries_sectors) * SECTOR_SIZE] = entries
    img[backup_entries_lba * SECTOR_SIZE:(backup_entries_lba + entries_sectors) * SECTOR_SIZE] = entries

    disk_guid = uuid.UUID('11111111-2222-3333-4444-555555555555')
    write_gpt_header(img, 1, sectors - 1, first_usable, last_usable, disk_guid, 2, entries_crc, sectors)
    write_gpt_header(img, sectors - 1, 1, first_usable, last_usable, disk_guid,
                     backup_entries_lba, entries_crc, sectors)


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ('mbr', 'gpt'):
        print('usage: mk_ahci_disk.py mbr|gpt OUT.img SIZE', file=sys.stderr)
        sys.exit(2)
    kind, out, size_s = sys.argv[1], sys.argv[2], sys.argv[3]
    size = parse_size(size_s)
    if size < 16 * 1024 * 1024 or size % SECTOR_SIZE != 0:
        raise SystemExit('disk size must be a sector-aligned value >= 16M')
    sectors = size // SECTOR_SIZE
    img = bytearray(size)
    if kind == 'mbr':
        write_mbr(img, sectors)
    else:
        write_gpt(img, sectors)
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'wb') as f:
        f.write(img)
    print(f'wrote {out}: {kind} {size} bytes')


if __name__ == '__main__':
    main()
