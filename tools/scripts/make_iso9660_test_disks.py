#!/usr/bin/env python3
from __future__ import annotations

import argparse
import uuid
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

SECTOR = 2048
DISK_SECTOR = 512
VD_START = 16
GPT_ENTRY_SIZE = 128
GPT_ENTRY_COUNT = 128
GPT_HEADER_SIZE = 92
LINUX_FS_GUID = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")
DISK_GUID = uuid.UUID("9660aa55-1111-2222-3333-444455556666")


def le16(v: int) -> bytes:
    return v.to_bytes(2, "little")


def be16(v: int) -> bytes:
    return v.to_bytes(2, "big")


def both16(v: int) -> bytes:
    return le16(v) + be16(v)


def le32(v: int) -> bytes:
    return v.to_bytes(4, "little")


def be32(v: int) -> bytes:
    return v.to_bytes(4, "big")


def both32(v: int) -> bytes:
    return le32(v) + be32(v)


def crc32(data: bytes | bytearray) -> int:
    return zlib.crc32(data) & 0xffffffff


def align_up(v: int, a: int) -> int:
    return (v + a - 1) & ~(a - 1)


def pad_sector(data: bytes) -> bytes:
    return data + bytes((-len(data)) % SECTOR)


def a_chars(s: str, n: int) -> bytes:
    return s.encode("ascii", "replace")[:n].ljust(n, b" ")


def joliet_name(s: str) -> bytes:
    return s.encode("utf-16-be")


@dataclass
class Node:
    iso_name: str
    data: bytes = b""
    rr_name: Optional[str] = None
    joliet: Optional[str] = None
    children: list["Node"] = field(default_factory=list)
    file_lba: int = 0
    p_lba: int = 0
    j_lba: int = 0
    ce_lba: int = 0
    ce_data: bytes = b""

    @property
    def is_dir(self) -> bool:
        return bool(self.children)

    @property
    def size(self) -> int:
        return SECTOR if self.is_dir else len(self.data)


@dataclass
class IsoSpec:
    file_name: str
    part_name: str
    tree: Node
    joliet: bool
    rockridge: bool


def rr_nm(name: str) -> bytes:
    raw = name.encode("utf-8")
    if len(raw) > 250:
        raise ValueError("RR NM name too large for one entry")
    return b"NM" + bytes([5 + len(raw), 1, 0]) + raw


def rr_sp() -> bytes:
    return b"SP\x07\x01\xbe\xef\x00"


def rr_ce(lba: int, off: int, size: int) -> bytes:
    return b"CE\x1c\x01" + both32(lba) + both32(off) + both32(size)


def dir_record(lba: int, size: int, flags: int, ident: bytes, su: bytes = b"") -> bytes:
    pad = b"\0" if len(ident) % 2 == 0 else b""
    rec_len = 33 + len(ident) + len(pad) + len(su)
    if rec_len > 255:
        raise ValueError(f"directory record too large: {rec_len}")
    rec = bytearray(rec_len)
    rec[0] = rec_len
    rec[2:10] = both32(lba)
    rec[10:18] = both32(size)
    rec[18:25] = b"\x7d\x01\x01\x00\x00\x00\x00"
    rec[25] = flags
    rec[28:32] = both16(1)
    rec[32] = len(ident)
    rec[33:33 + len(ident)] = ident
    start = 33 + len(ident) + len(pad)
    rec[start:start + len(su)] = su
    return bytes(rec)


def dir_data(node: Node, joliet: bool, rockridge: bool, parent: Optional[Node]) -> bytes:
    out = bytearray()
    own_lba = node.j_lba if joliet else node.p_lba
    par = parent if parent is not None else node
    par_lba = par.j_lba if joliet else par.p_lba
    dot_su = rr_sp() if rockridge and parent is None and not joliet else b""
    out += dir_record(own_lba, SECTOR, 2, b"\0", dot_su)
    out += dir_record(par_lba, SECTOR, 2, b"\1")
    for child in node.children:
        flags = 2 if child.is_dir else 0
        clba = child.j_lba if joliet and child.is_dir else child.p_lba if child.is_dir else child.file_lba
        ident = joliet_name(child.joliet or child.rr_name or child.iso_name) if joliet else child.iso_name.encode("ascii")
        su = b""
        if rockridge and not joliet and (child.rr_name or child.ce_data):
            su = rr_ce(child.ce_lba, 0, len(child.ce_data)) if child.ce_lba else rr_nm(child.rr_name or "")
        out += dir_record(clba, child.size, flags, ident, su)
    if len(out) > SECTOR:
        raise ValueError(f"directory too large: {node.iso_name}")
    return bytes(out).ljust(SECTOR, b"\0")


def walk_dirs(root: Node) -> list[tuple[Optional[Node], Node]]:
    items: list[tuple[Optional[Node], Node]] = []

    def rec(parent: Optional[Node], node: Node) -> None:
        if node.is_dir:
            items.append((parent, node))
            for child in node.children:
                rec(node, child)

    rec(None, root)
    return items


def walk_files(root: Node) -> list[Node]:
    files: list[Node] = []

    def rec(node: Node) -> None:
        if node.is_dir:
            for child in node.children:
                rec(child)
        else:
            files.append(node)

    rec(root)
    return files


def all_nodes(root: Node) -> list[Node]:
    nodes = [root]
    for child in root.children:
        nodes += all_nodes(child)
    return nodes


def assign_lbas(root: Node, joliet: bool) -> int:
    next_lba = VD_START + 4
    dirs = walk_dirs(root)
    for _, d in dirs:
        d.p_lba = next_lba
        next_lba += 1
    if joliet:
        for _, d in dirs:
            d.j_lba = next_lba
            next_lba += 1
    for f in walk_files(root):
        f.file_lba = next_lba
        next_lba += max(1, (len(f.data) + SECTOR - 1) // SECTOR)
    for n in all_nodes(root):
        if n.ce_data:
            n.ce_lba = next_lba
            next_lba += max(1, (len(n.ce_data) + SECTOR - 1) // SECTOR)
    return next_lba


def path_table(root: Node, joliet: bool) -> bytes:
    lba = root.j_lba if joliet else root.p_lba
    rec = bytearray()
    rec += b"\x01\x00" + le32(lba) + le16(1) + b"\0"
    rec += b"\0" if len(rec) % 2 else b""
    return bytes(rec)


def volume_descriptor(root: Node, volume_id: str, joliet: bool, total_sectors: int, path_lba: int) -> bytes:
    sec = bytearray(SECTOR)
    sec[0] = 2 if joliet else 1
    sec[1:6] = b"CD001"
    sec[6] = 1
    sec[8:40] = a_chars("SYMKERNEL", 32)
    sec[40:72] = a_chars(volume_id, 32)
    sec[80:88] = both32(total_sectors)
    if joliet:
        sec[88:91] = b"%/E"
    sec[120:124] = both16(1)
    sec[124:128] = both16(1)
    sec[128:132] = both16(SECTOR)
    pt = path_table(root, joliet)
    sec[132:140] = both32(len(pt))
    sec[140:144] = le32(path_lba)
    sec[148:152] = be32(path_lba)
    root_lba = root.j_lba if joliet else root.p_lba
    sec[156:156 + 34] = dir_record(root_lba, SECTOR, 2, b"\0")
    sec[190:318] = a_chars(volume_id, 128)
    sec[318:446] = a_chars("TEST", 128)
    sec[446:574] = a_chars("SYM", 128)
    sec[813:830] = b"2026010100000000\0"
    sec[830:847] = b"2026010100000000\0"
    sec[847:864] = b"0000000000000000\0"
    sec[864:881] = b"0000000000000000\0"
    sec[881] = 1
    return bytes(sec)


def terminator() -> bytes:
    sec = bytearray(SECTOR)
    sec[0] = 255
    sec[1:6] = b"CD001"
    sec[6] = 1
    return bytes(sec)


def build_iso_image(root: Node, volume_id: str, joliet: bool, rockridge: bool) -> bytes:
    total = assign_lbas(root, joliet)
    image = bytearray(total * SECTOR)
    p_path_lba = VD_START + 3
    image[VD_START * SECTOR:(VD_START + 1) * SECTOR] = volume_descriptor(root, volume_id, False, total, p_path_lba)
    if joliet:
        image[(VD_START + 1) * SECTOR:(VD_START + 2) * SECTOR] = volume_descriptor(root, volume_id, True, total, p_path_lba)
    term_lba = VD_START + (2 if joliet else 1)
    image[term_lba * SECTOR:(term_lba + 1) * SECTOR] = terminator()
    pt = path_table(root, False)
    image[p_path_lba * SECTOR:p_path_lba * SECTOR + len(pt)] = pt
    for parent, d in walk_dirs(root):
        image[d.p_lba * SECTOR:(d.p_lba + 1) * SECTOR] = dir_data(d, False, rockridge, parent)
    if joliet:
        for parent, d in walk_dirs(root):
            image[d.j_lba * SECTOR:(d.j_lba + 1) * SECTOR] = dir_data(d, True, False, parent)
    for f in walk_files(root):
        data = pad_sector(f.data)
        image[f.file_lba * SECTOR:f.file_lba * SECTOR + len(data)] = data
    for n in all_nodes(root):
        if n.ce_data:
            data = pad_sector(n.ce_data)
            image[n.ce_lba * SECTOR:n.ce_lba * SECTOR + len(data)] = data
    return bytes(image)


def gpt_guid_bytes(g: uuid.UUID) -> bytes:
    return g.bytes_le


def gpt_name(text: str) -> bytes:
    return text.encode("utf-16le")[:72].ljust(72, b"\0")


def write_gpt_header(img: bytearray, lba: int, alternate_lba: int, first_usable: int,
                     last_usable: int, entries_lba: int, entries_crc: int) -> None:
    header = bytearray(DISK_SECTOR)
    header[0:8] = b"EFI PART"
    header[8:12] = le32(0x00010000)
    header[12:16] = le32(GPT_HEADER_SIZE)
    header[24:32] = lba.to_bytes(8, "little")
    header[32:40] = alternate_lba.to_bytes(8, "little")
    header[40:48] = first_usable.to_bytes(8, "little")
    header[48:56] = last_usable.to_bytes(8, "little")
    header[56:72] = gpt_guid_bytes(DISK_GUID)
    header[72:80] = entries_lba.to_bytes(8, "little")
    header[80:84] = le32(GPT_ENTRY_COUNT)
    header[84:88] = le32(GPT_ENTRY_SIZE)
    header[88:92] = le32(entries_crc)
    header[16:20] = le32(crc32(header[:GPT_HEADER_SIZE]))
    img[lba * DISK_SECTOR:(lba + 1) * DISK_SECTOR] = header


def build_partition_disk(parts: list[tuple[str, bytes]]) -> bytes:
    entries_sectors = (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE + DISK_SECTOR - 1) // DISK_SECTOR
    first_usable = 2 + entries_sectors
    cursor = align_up(first_usable, 2048)
    layout: list[tuple[str, int, int, bytes]] = []
    for name, data in parts:
        start = cursor
        count = align_up(len(data), DISK_SECTOR) // DISK_SECTOR
        layout.append((name, start, count, data))
        cursor = align_up(start + count, 2048)

    backup_entries_lba = cursor
    last_usable = backup_entries_lba - 1
    sectors = backup_entries_lba + entries_sectors + 1
    img = bytearray(sectors * DISK_SECTOR)
    img[446:462] = bytes([0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff]) + le32(1) + le32(min(sectors - 1, 0xffffffff))
    img[510:512] = b"\x55\xaa"

    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    for idx, (name, start, count, data) in enumerate(layout):
        off = idx * GPT_ENTRY_SIZE
        entries[off:off + 16] = gpt_guid_bytes(LINUX_FS_GUID)
        entries[off + 16:off + 32] = gpt_guid_bytes(uuid.uuid5(uuid.NAMESPACE_DNS, f"symkernel-iso9660-{name}"))
        entries[off + 32:off + 40] = start.to_bytes(8, "little")
        entries[off + 40:off + 48] = (start + count - 1).to_bytes(8, "little")
        entries[off + 56:off + 128] = gpt_name(name)
        begin = start * DISK_SECTOR
        img[begin:begin + len(data)] = data

    entries_crc = crc32(entries)
    img[2 * DISK_SECTOR:(2 + entries_sectors) * DISK_SECTOR] = entries
    img[backup_entries_lba * DISK_SECTOR:(backup_entries_lba + entries_sectors) * DISK_SECTOR] = entries
    write_gpt_header(img, 1, sectors - 1, first_usable, last_usable, 2, entries_crc)
    write_gpt_header(img, sectors - 1, 1, first_usable, last_usable, backup_entries_lba, entries_crc)
    return bytes(img)


def plain_tree() -> Node:
    return Node("", children=[
        Node("HELLO.TXT;1", b"plain iso9660 hello\n"),
        Node("DIR", children=[Node("NESTED.TXT;1", b"plain nested file\n")]),
    ])


def joliet_tree() -> Node:
    return Node("", children=[
        Node("JOLIET.TXT;1", b"joliet unicode name\n", joliet="über-joliet.txt"),
        Node("SUBDIR", children=[Node("CHILD.TXT;1", b"joliet child\n", joliet="child-lower.txt")], joliet="lower-dir"),
    ])


def rockridge_tree() -> Node:
    return Node("", children=[
        Node("RRNAME.TXT;1", b"rock ridge nm name\n", rr_name="rr-name.txt"),
        Node("RRDIR", children=[Node("CHILD.TXT;1", b"rock ridge child\n", rr_name="child.txt")], rr_name="rr-dir"),
    ])


def both_tree() -> Node:
    return Node("", children=[
        Node("BOTH.TXT;1", b"rock ridge must win over joliet\n", rr_name="rock-wins.txt", joliet="joliet-loses.txt"),
        Node("BOTH_DIR", children=[Node("INNER.TXT;1", b"nested both\n", rr_name="inner-rock.txt", joliet="inner-joliet.txt")], rr_name="rock-dir", joliet="joliet-dir"),
    ])


def ce_tree() -> Node:
    long_name = "rr-ce-" + "x" * 120 + ".txt"
    n = Node("LONGNM.TXT;1", b"rock ridge continuation area name\n")
    n.ce_data = rr_nm(long_name)
    return Node("", children=[n])


def iso_specs() -> list[IsoSpec]:
    return [
        IsoSpec("iso9660_plain.iso", "iso-plain", plain_tree(), False, False),
        IsoSpec("iso9660_joliet.iso", "iso-joliet", joliet_tree(), True, False),
        IsoSpec("iso9660_rockridge.iso", "iso-rockridge", rockridge_tree(), False, True),
        IsoSpec("iso9660_rr_preferred.iso", "iso-rr-preferred", both_tree(), True, True),
        IsoSpec("iso9660_rockridge_ce.iso", "iso-rockridge-ce", ce_tree(), False, True),
    ]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir", nargs="?", default="build/testdisks/iso9660")
    args = ap.parse_args()
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    images: list[tuple[str, bytes]] = []
    for spec in iso_specs():
        volume_id = spec.file_name.removesuffix(".iso")[:32].upper()
        image = build_iso_image(spec.tree, volume_id, spec.joliet, spec.rockridge)
        (out / spec.file_name).write_bytes(image)
        images.append((spec.part_name, image))

    (out / "iso9660-partitions.img").write_bytes(build_partition_disk(images))
    manifest = """iso9660-partitions.img:
  GPT partition 1: iso9660_plain.iso
  GPT partition 2: iso9660_joliet.iso
  GPT partition 3: iso9660_rockridge.iso
  GPT partition 4: iso9660_rr_preferred.iso
  GPT partition 5: iso9660_rockridge_ce.iso

iso9660_plain.iso:
  /HELLO.TXT -> plain iso9660 hello
  /DIR/NESTED.TXT -> plain nested file

iso9660_joliet.iso:
  /über-joliet.txt -> joliet unicode name
  /lower-dir/child-lower.txt -> joliet child

iso9660_rockridge.iso:
  /rr-name.txt -> rock ridge nm name
  /rr-dir/child.txt -> rock ridge child

iso9660_rr_preferred.iso:
  /rock-wins.txt -> rock ridge must win over joliet
  /rock-dir/inner-rock.txt -> nested both
  Joliet aliases should not be visible when Rock Ridge is detected.

iso9660_rockridge_ce.iso:
  /rr-ce-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.txt -> rock ridge continuation area name
"""
    (out / "README.txt").write_text(manifest, encoding="utf-8")


if __name__ == "__main__":
    main()
