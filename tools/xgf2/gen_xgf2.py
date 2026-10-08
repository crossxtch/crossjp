#!/usr/bin/env python3
"""Rasterize a TTF into an XGF2 2-bit em-box font (body + ruby).

Example:
    python3 tools/xgf2/gen_xgf2.py --ttf NotoSerifCJKjp-Regular.otf --device x3 -o reading.xgf2
    python3 tools/xgf2/gen_xgf2.py --synthetic --codepoints あいう漢字 --em 16 --ruby-em 8 -o test.xgf2

Copy the result to the SD card as /.crossjp/reading.xgf2 (or next to a .txt as name.xgf2).
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC = 0x32464758  # XGF2
VERSION = 1
HEADER_SIZE = 64
FLAG_2BPP = 1
FLAG_FIXED = 2
FLAG_FREQ = 4

PPI = {"x3": 259.0, "x4": 219.0}

# Frequency buckets: earlier = lower glyph id = earlier on disk.
BUCKET_ASCII = 0
BUCKET_KANA = 1
BUCKET_CJK = 2
BUCKET_OTHER = 3


def em_for(device: str, pt: float) -> int:
    return max(4, int(round(pt * PPI[device] / 72.0)))


def bucket(cp: int) -> int:
    if 0x20 <= cp <= 0x7E:
        return BUCKET_ASCII
    if 0x3000 <= cp <= 0x30FF or 0xFF00 <= cp <= 0xFFEF:
        return BUCKET_KANA
    if 0x4E00 <= cp <= 0x9FFF or 0x3400 <= cp <= 0x4DBF or 0xF900 <= cp <= 0xFAFF:
        return BUCKET_CJK
    return BUCKET_OTHER


def quantize(g: int) -> int:
    if g >= 192:
        return 3
    if g >= 128:
        return 2
    if g >= 64:
        return 1
    return 0


def pack_2bit(pixels: list[int], em: int) -> bytes:
    row_bytes = (em + 3) // 4
    out = bytearray(row_bytes * em)
    for y in range(em):
        for x in range(em):
            v = pixels[y * em + x] & 3
            out[y * row_bytes + x // 4] |= v << (6 - (x % 4) * 2)
    return bytes(out)


def merge_intervals(cps: list[int], id_of: dict[int, int]) -> list[tuple[int, int, int]]:
    if not cps:
        return []
    ordered = sorted(set(cps))
    out: list[tuple[int, int, int]] = []
    start = prev = ordered[0]
    start_id = id_of[start]
    expected = start_id
    for cp in ordered[1:]:
        gid = id_of[cp]
        if cp == prev + 1 and gid == expected + 1:
            prev = cp
            expected = gid
            continue
        out.append((start, prev, start_id))
        start = prev = cp
        start_id = expected = gid
    out.append((start, prev, start_id))
    return out


def raster_char(font, ch: str, em: int) -> bytes:
    img = __import__("PIL.Image", fromlist=["Image"]).Image.new("L", (em, em), 0)
    draw = __import__("PIL.ImageDraw", fromlist=["ImageDraw"]).ImageDraw.Draw(img)
    bbox = font.getbbox(ch)
    if bbox is None:
        return pack_2bit([0] * (em * em), em)
    l, t, r, b = bbox
    w, h = r - l, b - t
    if w <= 0 or h <= 0:
        return pack_2bit([0] * (em * em), em)
    ox = (em - w) // 2 - l
    oy = (em - h) // 2 - t
    draw.text((ox, oy), ch, font=font, fill=255)
    px = img.load()
    pixels = [quantize(px[x, y]) for y in range(em) for x in range(em)]
    return pack_2bit(pixels, em)


def synthetic_slot(cp: int, em: int) -> bytes:
    pixels = [0] * (em * em)
    # Unique-ish box so tests can tell glyphs apart.
    inset = 1 + (cp % 3)
    for y in range(inset, em - inset):
        for x in range(inset, em - inset):
            pixels[y * em + x] = 1 + (cp + x + y) % 3
    return pack_2bit(pixels, em)


def collect_font_cps(font_path: str, max_glyphs: int) -> list[int]:
    from PIL import ImageFont

    font = ImageFont.truetype(font_path, 16)
    ranges = [
        (0x0020, 0x007E),
        (0x3000, 0x30FF),
        (0x4E00, 0x9FFF),
        (0xFF00, 0xFFEF),
        (0x2010, 0x2026),
        (0xFFFD, 0xFFFD),
    ]
    cps: list[int] = []
    cmap = getattr(font, "getmask", None)
    for a, b in ranges:
        for cp in range(a, b + 1):
            ch = chr(cp)
            try:
                if font.getmask(ch).size[0] == 0:
                    continue
            except Exception:
                continue
            cps.append(cp)
            if len(cps) >= max_glyphs:
                return cps
    if cmap is None:
        pass
    return cps


def assign_ids(cps: list[int]) -> dict[int, int]:
    ordered = sorted(set(cps) | {0xFFFD}, key=lambda c: (bucket(c), c))
    return {cp: i for i, cp in enumerate(ordered)}


def ruby_candidates(cps: list[int]) -> list[int]:
    out = []
    for cp in cps:
        if 0x3040 <= cp <= 0x30FF or 0x3000 <= cp <= 0x303F or 0xFF00 <= cp <= 0xFFEF or 0x20 <= cp <= 0x7E:
            out.append(cp)
    return out


def write_xgf2(
    path: Path,
    id_of: dict[int, int],
    body_slots: dict[int, bytes],
    ruby_slots: dict[int, bytes],
    em: int,
    ruby_em: int,
) -> None:
    ordered_cps = [None] * len(id_of)
    for cp, gid in id_of.items():
        ordered_cps[gid] = cp
    body_count = len(ordered_cps)
    body_stride = ((em + 3) // 4) * em
    ruby_stride = ((ruby_em + 3) // 4) * ruby_em if ruby_em else 0

    intervals = merge_intervals([cp for cp in ordered_cps if cp is not None], id_of)

    ruby_map: list[int] = []
    ruby_blob = bytearray()
    if ruby_em:
        for gid, cp in enumerate(ordered_cps):
            if cp in ruby_slots:
                ruby_map.append(gid)
                slot = ruby_slots[cp]
                if len(slot) != ruby_stride:
                    raise SystemExit(f"ruby stride {len(slot)} != {ruby_stride}")
                ruby_blob += slot

    body_blob = bytearray()
    for gid, cp in enumerate(ordered_cps):
        slot = body_slots[cp]
        if len(slot) != body_stride:
            raise SystemExit(f"body stride {len(slot)} != {body_stride}")
        body_blob += slot

    intervals_off = HEADER_SIZE
    ruby_map_off = intervals_off + len(intervals) * 6
    # 512-byte align bitmap blobs
    def align(n: int) -> int:
        return (n + 511) & ~511

    body_off = align(ruby_map_off + len(ruby_map) * 2)
    ruby_off = align(body_off + len(body_blob)) if ruby_blob else 0

    header = struct.pack(
        "<IHHBBBBHHHHHHIIII4s20s",
        MAGIC,
        VERSION,
        FLAG_2BPP | FLAG_FIXED | FLAG_FREQ,
        em,
        ruby_em,
        2,
        0,
        body_stride,
        ruby_stride,
        body_count,
        len(ruby_map),
        len(intervals),
        0,
        intervals_off,
        ruby_map_off,
        body_off,
        ruby_off,
        bytes([0, 96, 160, 255]),
        bytes(20),
    )
    assert len(header) == HEADER_SIZE

    with path.open("wb") as f:
        f.write(header)
        for first, last, gid in intervals:
            f.write(struct.pack("<HHH", first, last, gid))
        for gid in ruby_map:
            f.write(struct.pack("<H", gid))
        pad = body_off - f.tell()
        if pad:
            f.write(bytes(pad))
        f.write(body_blob)
        if ruby_blob:
            pad = ruby_off - f.tell()
            if pad:
                f.write(bytes(pad))
            f.write(ruby_blob)

    print(
        f"Wrote {path}  {fsize(path)}  em={em} ruby={ruby_em} "
        f"glyphs={body_count} rubySlots={len(ruby_map)} intervals={len(intervals)}",
        file=sys.stderr,
    )


def fsize(path: Path) -> str:
    n = path.stat().st_size
    if n >= 1024 * 1024:
        return f" {n / 1024 / 1024:.2f} MB"
    return f" {n / 1024:.1f} KB"


def parse_codepoints(s: str) -> list[int]:
    return [ord(ch) for ch in s if not ch.isspace()]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ttf", help="source TTF/OTF")
    ap.add_argument("--synthetic", action="store_true", help="boxes instead of a TTF (tests)")
    ap.add_argument("--codepoints", default="あいうえおかきくけこ漢字日本語", help="used with --synthetic")
    ap.add_argument("--device", choices=("x3", "x4"), default="x3")
    ap.add_argument("--pt", type=float, default=13.0)
    ap.add_argument("--em", type=int, help="override body em px")
    ap.add_argument("--ruby-em", type=int, dest="ruby_em", help="override ruby em px")
    ap.add_argument("--max-glyphs", type=int, default=16000)
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    em = args.em if args.em else em_for(args.device, args.pt)
    ruby_em = args.ruby_em if args.ruby_em is not None else (em + 1) // 2

    if args.synthetic:
        cps = parse_codepoints(args.codepoints) + [0xFFFD]
        id_of = assign_ids(cps)
        body = {cp: synthetic_slot(cp, em) for cp in id_of}
        ruby = {cp: synthetic_slot(cp, ruby_em) for cp in ruby_candidates(list(id_of))}
        write_xgf2(Path(args.output), id_of, body, ruby, em, ruby_em)
        return

    if not args.ttf:
        ap.error("--ttf is required unless --synthetic")

    from PIL import ImageFont

    cps = collect_font_cps(args.ttf, args.max_glyphs)
    id_of = assign_ids(cps)
    body_font = ImageFont.truetype(args.ttf, em)
    ruby_font = ImageFont.truetype(args.ttf, ruby_em) if ruby_em else None
    body = {}
    ruby = {}
    total = len(id_of)
    for i, cp in enumerate(sorted(id_of, key=lambda c: id_of[c])):
        ch = chr(cp)
        body[cp] = raster_char(body_font, ch, em)
        if ruby_font and cp in set(ruby_candidates(list(id_of))):
            ruby[cp] = raster_char(ruby_font, ch, ruby_em)
        if (i + 1) % 500 == 0:
            print(f"  raster {i + 1}/{total}", file=sys.stderr)
    write_xgf2(Path(args.output), id_of, body, ruby, em, ruby_em)


if __name__ == "__main__":
    main()
