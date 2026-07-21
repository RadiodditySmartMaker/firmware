#!/usr/bin/env python3
"""
Generate an external Chinese font binary image from a C++ font table.

Output layout:
  uint32 magic
  uint32 version
  uint32 count
  uint32 reserved = 0
  uint8  key_table[count][key_size]
  uint8  bitmap_table[count][bitmap_size]

Examples:
  python3 bin/generate_chinese_font_bin.py
  python3 bin/generate_chinese_font_bin.py --output /tmp/chinese_font.bin
  python3 bin/generate_chinese_font_bin.py --input src/graphics/fonts/ChineseFontData.cpp --output bin/chinese_font.bin
  python3 bin/generate_chinese_font_bin.py --input src/graphics/fonts/CN_Font_10.cpp --font-type-name CN_Font_10 --font-array-name cnFont10 --glyph-width 10 --glyph-height 10 --bitmap-size 20 --output bin/cn_font_10.bin
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path


MAGIC = 0x43484631  # "CHF1"
VERSION = 1
KEY_SIZE = 4
BITMAP_SIZE = 32


ENTRY_RE = re.compile(
    r'\{\s*"((?:\\.|[^"\\])*)"\s*,\s*\{([^}]*)\}\s*\},?',
    re.MULTILINE,
)


def parse_int_auto(value: str) -> int:
    return int(value, 0)


def compute_bitmap_size(glyph_width: int | None, glyph_height: int | None, bitmap_size: int | None) -> int:
    if bitmap_size is not None:
        return bitmap_size

    if glyph_width is None or glyph_height is None:
        return BITMAP_SIZE

    return ((glyph_width + 7) // 8) * glyph_height


def extract_font_block(source_text: str, font_type_name: str, font_array_name: str) -> str:
    declaration_re = re.compile(
        rf"const\s+{re.escape(font_type_name)}\s+{re.escape(font_array_name)}\s*\[\]\s*=\s*\{{"
    )
    match = declaration_re.search(source_text)
    if not match:
        raise ValueError(
            f"Cannot find 'const {font_type_name} {font_array_name}[] = {{' in source file"
        )

    start = match.end() - 1
    if start < 0:
        raise ValueError(f"Cannot locate start of {font_array_name} array body")

    end = source_text.find("\n};", start)
    if end < 0:
        raise ValueError(f"Cannot locate end of {font_array_name} array body")

    return source_text[start + 1 : end]


def parse_entries(source_text: str, font_type_name: str, font_array_name: str, bitmap_size: int) -> list[tuple[str, bytes]]:
    block = extract_font_block(source_text, font_type_name, font_array_name)
    entries: list[tuple[str, bytes]] = []

    for match in ENTRY_RE.finditer(block):
        utf8_text = match.group(1)
        bitmap_text = match.group(2)
        byte_values = [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{1,2})", bitmap_text)]

        if len(byte_values) != bitmap_size:
            raise ValueError(
                f"Glyph '{utf8_text}' has {len(byte_values)} bytes, expected {bitmap_size}"
            )

        entries.append((utf8_text, bytes(byte_values)))

    if not entries:
        raise ValueError(f"No {font_array_name} entries were parsed")

    return entries


def make_utf8_key(text: str, key_size: int) -> bytes:
    raw = text.encode("utf-8")
    if len(raw) > key_size - 1:
        raw = raw[: key_size - 1]
    return raw.ljust(key_size, b"\x00")


def build_binary(entries: list[tuple[str, bytes]], magic: int, version: int, key_size: int) -> bytes:
    header = struct.pack("<IIII", magic, version, len(entries), 0)
    key_table = b"".join(make_utf8_key(text, key_size) for text, _ in entries)
    bitmap_table = b"".join(bitmap for _, bitmap in entries)
    return header + key_table + bitmap_table


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate an external font binary image from a C++ font table")
    parser.add_argument(
        "--input",
        default="src/graphics/fonts/ChineseFontData.cpp",
        help="Path to the source .cpp font table",
    )
    parser.add_argument(
        "--output",
        default="bin/chinese_font.bin",
        help="Output path for generated binary",
    )
    parser.add_argument(
        "--font-type-name",
        default="ChineseFont",
        help="C++ struct/type name used by the font table",
    )
    parser.add_argument(
        "--font-array-name",
        default="chineseFont",
        help="C++ array name that contains the font table",
    )
    parser.add_argument(
        "--key-size",
        type=int,
        default=KEY_SIZE,
        help="UTF-8 key table entry size in bytes",
    )
    parser.add_argument(
        "--glyph-width",
        type=int,
        default=None,
        help="Glyph width in pixels; used to derive bitmap size when --bitmap-size is omitted",
    )
    parser.add_argument(
        "--glyph-height",
        type=int,
        default=None,
        help="Glyph height in pixels; used to derive bitmap size when --bitmap-size is omitted",
    )
    parser.add_argument(
        "--bitmap-size",
        type=int,
        default=None,
        help="Bitmap size in bytes per glyph; defaults to 32 or is derived from glyph geometry",
    )
    parser.add_argument(
        "--magic",
        type=parse_int_auto,
        default=MAGIC,
        help="Image magic value, accepts decimal or hex (for example 0x43484631)",
    )
    parser.add_argument(
        "--version",
        type=int,
        default=VERSION,
        help="Image version number",
    )
    args = parser.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output)
    bitmap_size = compute_bitmap_size(args.glyph_width, args.glyph_height, args.bitmap_size)

    source_text = input_path.read_text(encoding="utf-8")
    entries = parse_entries(source_text, args.font_type_name, args.font_array_name, bitmap_size)
    blob = build_binary(entries, args.magic, args.version, args.key_size)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(blob)

    print(f"Generated {output_path}")
    print(f"Source: {input_path}")
    print(f"Table: {args.font_type_name} {args.font_array_name}[]")
    print(f"Glyphs: {len(entries)}")
    print(f"Bytes: {len(blob)}")
    print(f"Key size: {args.key_size}")
    print(f"Bitmap size: {bitmap_size}")
    if args.glyph_width is not None and args.glyph_height is not None:
        print(f"Glyph geometry: {args.glyph_width}x{args.glyph_height}")
    print(f"Header: magic=0x{args.magic:08x} version={args.version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
