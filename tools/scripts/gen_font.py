#!/usr/bin/env python3
import os
import sys

FIRST_GLYPH = 0
GLYPH_COUNT = 256
REQUIRED_MIN = 32
REQUIRED_MAX = 126
MAX_WIDTH = 32
MAX_HEIGHT = 64


class BdfError(Exception):
    pass


def die(msg):
    print(f"gen_font.py: error: {msg}", file=sys.stderr)
    sys.exit(1)


def parse_int(value, line_no):
    try:
        return int(value, 10)
    except ValueError as exc:
        raise BdfError(f"line {line_no}: invalid integer: {value}") from exc


def parse_bdf(path):
    if not os.path.exists(path):
        die(f"font file not found: {path}")

    with open(path, "r", encoding="latin-1", errors="strict", newline=None) as f:
        lines = [(i + 1, line.rstrip("\n")) for i, line in enumerate(f)]

    if not lines:
        raise BdfError("empty BDF file")

    started = False
    ended = False
    declared_chars = None
    font_bbox = None
    glyphs = {}
    parsed_chars = 0
    i = 0

    while i < len(lines):
        line_no, line = lines[i]
        parts = line.split()
        if not parts:
            i += 1
            continue

        key = parts[0]
        if key == "STARTFONT":
            if started:
                raise BdfError(f"line {line_no}: duplicate STARTFONT")
            if len(parts) != 2 or parts[1] != "2.1":
                raise BdfError(f"line {line_no}: unsupported STARTFONT version")
            started = True
        elif key == "FONTBOUNDINGBOX":
            if not started:
                raise BdfError(f"line {line_no}: FONTBOUNDINGBOX before STARTFONT")
            if font_bbox is not None:
                raise BdfError(f"line {line_no}: duplicate FONTBOUNDINGBOX")
            if len(parts) != 5:
                raise BdfError(f"line {line_no}: malformed FONTBOUNDINGBOX")
            w = parse_int(parts[1], line_no)
            h = parse_int(parts[2], line_no)
            xoff = parse_int(parts[3], line_no)
            yoff = parse_int(parts[4], line_no)
            if w < 1 or w > MAX_WIDTH or h < 1 or h > MAX_HEIGHT:
                raise BdfError(f"line {line_no}: unsupported FONTBOUNDINGBOX size")
            font_bbox = (w, h, xoff, yoff)
        elif key == "CHARS":
            if declared_chars is not None:
                raise BdfError(f"line {line_no}: duplicate CHARS")
            if len(parts) != 2:
                raise BdfError(f"line {line_no}: malformed CHARS")
            declared_chars = parse_int(parts[1], line_no)
            if declared_chars < 0:
                raise BdfError(f"line {line_no}: negative CHARS count")
        elif key == "STARTCHAR":
            if font_bbox is None:
                raise BdfError(f"line {line_no}: STARTCHAR before FONTBOUNDINGBOX")
            char, next_i = parse_bdf_char(lines, i)
            parsed_chars += 1
            encoding = char["encoding"]
            if FIRST_GLYPH <= encoding < FIRST_GLYPH + GLYPH_COUNT:
                if encoding in glyphs:
                    raise BdfError(f"line {line_no}: duplicate ENCODING {encoding}")
                glyphs[encoding] = char
            i = next_i
            continue
        elif key == "ENDFONT":
            ended = True
            if any(rest.strip() for rest in (line for _, line in lines[i + 1:])):
                raise BdfError(f"line {line_no}: data after ENDFONT")
            break

        i += 1

    if not started:
        raise BdfError("missing STARTFONT")
    if not ended:
        raise BdfError("missing ENDFONT")
    if font_bbox is None:
        raise BdfError("missing FONTBOUNDINGBOX")
    if declared_chars is None:
        raise BdfError("missing CHARS")
    if parsed_chars != declared_chars:
        raise BdfError(f"CHARS count mismatch: declared {declared_chars}, parsed {parsed_chars}")

    for code in range(REQUIRED_MIN, REQUIRED_MAX + 1):
        if code not in glyphs:
            raise BdfError(f"missing required glyph U+{code:04X}")

    return font_bbox, glyphs


def parse_bdf_char(lines, start_index):
    start_line_no, _ = lines[start_index]
    encoding = None
    bbx = None
    bitmap = None
    swidth_seen = False
    dwidth_seen = False
    i = start_index + 1

    while i < len(lines):
        line_no, line = lines[i]
        parts = line.split()
        if not parts:
            raise BdfError(f"line {line_no}: blank line inside character")

        key = parts[0]
        if key == "ENCODING":
            if encoding is not None:
                raise BdfError(f"line {line_no}: duplicate ENCODING")
            if len(parts) not in (2, 3):
                raise BdfError(f"line {line_no}: malformed ENCODING")
            encoding = parse_int(parts[1], line_no)
        elif key == "SWIDTH":
            if len(parts) != 3:
                raise BdfError(f"line {line_no}: malformed SWIDTH")
            swidth_seen = True
        elif key == "DWIDTH":
            if len(parts) != 3:
                raise BdfError(f"line {line_no}: malformed DWIDTH")
            dwidth_seen = True
        elif key == "BBX":
            if bbx is not None:
                raise BdfError(f"line {line_no}: duplicate BBX")
            if len(parts) != 5:
                raise BdfError(f"line {line_no}: malformed BBX")
            w = parse_int(parts[1], line_no)
            h = parse_int(parts[2], line_no)
            xoff = parse_int(parts[3], line_no)
            yoff = parse_int(parts[4], line_no)
            if w < 0 or w > MAX_WIDTH or h < 0 or h > MAX_HEIGHT:
                raise BdfError(f"line {line_no}: unsupported BBX size")
            bbx = (w, h, xoff, yoff)
        elif key == "BITMAP":
            if bitmap is not None:
                raise BdfError(f"line {line_no}: duplicate BITMAP")
            if encoding is None:
                raise BdfError(f"line {line_no}: BITMAP before ENCODING")
            if bbx is None:
                raise BdfError(f"line {line_no}: BITMAP before BBX")
            bitmap, i = parse_bitmap(lines, i + 1, bbx)
            continue
        elif key == "ENDCHAR":
            if encoding is None:
                raise BdfError(f"line {line_no}: ENDCHAR before ENCODING")
            if not swidth_seen:
                raise BdfError(f"line {line_no}: missing SWIDTH")
            if not dwidth_seen:
                raise BdfError(f"line {line_no}: missing DWIDTH")
            if bbx is None:
                raise BdfError(f"line {line_no}: missing BBX")
            if bitmap is None:
                raise BdfError(f"line {line_no}: missing BITMAP")
            return {"encoding": encoding, "bbx": bbx, "bitmap": bitmap}, i + 1
        elif key == "STARTCHAR":
            raise BdfError(f"line {line_no}: nested STARTCHAR")

        i += 1

    raise BdfError(f"line {start_line_no}: unterminated STARTCHAR")


def parse_bitmap(lines, start_index, bbx):
    w, h, _, _ = bbx
    row_bytes = (w + 7) // 8
    bitmap = []
    i = start_index

    for row in range(h):
        if i >= len(lines):
            raise BdfError("unexpected EOF in BITMAP")
        line_no, line = lines[i]
        if len(line) != row_bytes * 2:
            raise BdfError(f"line {line_no}: bitmap row has wrong hex width")
        try:
            value = int(line, 16)
        except ValueError as exc:
            raise BdfError(f"line {line_no}: bitmap row is not hex") from exc
        if value >= (1 << (row_bytes * 8)):
            raise BdfError(f"line {line_no}: bitmap row overflows")
        bitmap.append(value)
        i += 1

    if i >= len(lines):
        raise BdfError("unexpected EOF after BITMAP")
    next_key = lines[i][1].split()[0] if lines[i][1].split() else ""
    if next_key != "ENDCHAR":
        raise BdfError(f"line {lines[i][0]}: expected ENDCHAR after BITMAP")

    return bitmap, i


def pack_row(bits):
    out = []
    for i in range(0, len(bits), 8):
        byte = 0
        for bit in range(8):
            if i + bit < len(bits) and bits[i + bit]:
                byte |= 0x80 >> bit
        out.append(byte)
    return bytes(out)


def rasterize_glyph(glyph, font_bbox, cell_width, cell_height):
    font_w, font_h, font_xoff, font_yoff = font_bbox
    _, _, _, _ = font_w, font_h, font_xoff, font_yoff
    bbx_w, bbx_h, bbx_xoff, bbx_yoff = glyph["bbx"]
    bitmap = glyph["bitmap"]
    rows = [[False] * cell_width for _ in range(cell_height)]
    baseline_y = cell_height + font_yoff
    bitmap_row_bytes = (bbx_w + 7) // 8

    if baseline_y < 0 or baseline_y > cell_height:
        raise BdfError("font baseline is outside the output cell")

    for y in range(bbx_h):
        row_value = bitmap[y]
        glyph_y = bbx_yoff + bbx_h - 1 - y
        dst_y = baseline_y - 1 - glyph_y
        if dst_y < 0 or dst_y >= cell_height:
            raise BdfError("glyph bitmap does not fit output cell vertically")

        for x in range(bbx_w):
            mask = 1 << (bitmap_row_bytes * 8 - 1 - x)
            if (row_value & mask) == 0:
                continue
            dst_x = bbx_xoff - font_xoff + x
            if dst_x < 0 or dst_x >= cell_width:
                raise BdfError("glyph bitmap does not fit output cell horizontally")
            rows[dst_y][dst_x] = True

    return rows


def generate_font_bin(bdf_path, output_path, cell_width=None, cell_height=None):
    try:
        font_bbox, glyphs = parse_bdf(bdf_path)
        font_w, font_h, _, _ = font_bbox
        if cell_width is None:
            cell_width = font_w
        if cell_height is None:
            cell_height = font_h
        if cell_width < 1 or cell_width > MAX_WIDTH:
            raise BdfError("glyph width must be in range 1..32")
        if cell_height < 1 or cell_height > MAX_HEIGHT:
            raise BdfError("glyph height must be in range 1..64")
        if font_w > cell_width or font_h > cell_height:
            raise BdfError("FONTBOUNDINGBOX is larger than requested output cell")

        row_bytes = (cell_width + 7) // 8
        expected_size = GLYPH_COUNT * cell_height * row_bytes
        os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)

        with open(output_path, "wb") as out:
            for code in range(FIRST_GLYPH, FIRST_GLYPH + GLYPH_COUNT):
                glyph = glyphs.get(code)
                if glyph is None:
                    blank = [False] * cell_width
                    for _ in range(cell_height):
                        out.write(pack_row(blank))
                    continue

                try:
                    rows = rasterize_glyph(glyph, font_bbox, cell_width, cell_height)
                except BdfError as exc:
                    raise BdfError(f"glyph U+{code:04X}: {exc}") from exc
                for row in rows:
                    out.write(pack_row(row))

        total = os.path.getsize(output_path)
        if total != expected_size:
            raise BdfError(f"internal size mismatch: got {total}, expected {expected_size}")

        print(f"generated {output_path}")
        print(f"glyphs: {GLYPH_COUNT}")
        print(f"dimensions: {cell_width}x{cell_height}")
        print(f"bytes: {total}")
    except BdfError as exc:
        die(str(exc))


def parse_cli(argv):
    if len(argv) == 3:
        return argv[1], argv[2], None, None
    if len(argv) == 5:
        return argv[1], argv[2], parse_int(argv[3], 0), parse_int(argv[4], 0)
    if len(argv) in (6, 7):
        return argv[1], argv[2], parse_int(argv[4], 0), parse_int(argv[5], 0)

    print(
        "Usage:\n"
        "  python3 gen_font.py <input.bdf> <output.bin>\n"
        "  python3 gen_font.py <input.bdf> <output.bin> <width> <height>\n"
        "  python3 gen_font.py <input.bdf> <output.bin> <ignored-font-size> <width> <height> [ignored-threshold]",
        file=sys.stderr,
    )
    sys.exit(1)


if __name__ == "__main__":
    bdf_path, output_path, width, height = parse_cli(sys.argv)
    generate_font_bin(bdf_path, output_path, width, height)
