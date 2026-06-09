#!/usr/bin/env python3
"""
pack_jsc_map.py — convert JSC_map.cpp into a memory-mappable binary blob

JSC_map.cpp from JS8Call-improved (and gfsk8-modem-clean) contains a 262,144-
entry word dictionary for JS8's source codec:

    const Tuple JSC::map[262144] = {
        {"T", 1, 1},
        {"A", 1, 2},
        ...
        {"ROSIDS", 1, 262143},
    };

Compiling this into the app partition would consume ~5 MB of flash and force
a repartition. Instead we extract the data into a self-contained binary that
lives in its own ESP-IDF partition and is accessed via esp_partition_mmap().

Binary format (little-endian, all integers):

    [Header]                                       16 bytes
        uint32  magic              = 0x4D43534A ('JSCM')
        uint32  version            = 1
        uint32  entry_count        = 262144
        uint32  string_pool_size   = total bytes of NUL-terminated strings

    [Offset table]                  4 bytes × entry_count = 1.0 MB
        uint32  str_offset[i]      offset within string pool to NUL-term
                                    string for entry i. Resolved at runtime
                                    as: const char *word = pool + offsets[i];

    [String pool]                       variable, ~3 MB
        contiguous NUL-terminated strings, referenced by str_offset

Why this minimal format:
  - Audit of gfsk8-modem-clean's JSC.cpp shows JSC::map[i] is only ever
    accessed via .str during RX (decompress path called from Varicode.cpp).
    .size and .index are TX-only (compress / exists / lookup).
  - L7.4a is RX-first; storing only .str saves ~1 MB and trims the format
    surface area. If TX is added later we bump version and add a richer
    format, no breaking change.
  - 32-bit string offsets give 4 GB range — overkill for ~3 MB pool but
    simpler than 16-bit. Cache-friendly: offsets array is tight.

Usage:
    pack_jsc_map.py INPUT.cpp OUTPUT.bin
"""

from __future__ import annotations

import argparse
import codecs
import re
import struct
import sys
from pathlib import Path

# ── Format constants ───────────────────────────────────────────────────────────

MAGIC          = 0x4D43534A   # 'JSCM' little-endian
VERSION        = 1
HEADER_FORMAT  = "<IIII"      # 4 × uint32
HEADER_SIZE    = struct.calcsize(HEADER_FORMAT)
OFFSET_FORMAT  = "<I"         # uint32 str_offset per entry
OFFSET_SIZE    = struct.calcsize(OFFSET_FORMAT)
EXPECTED_COUNT = 262144

# Regex matches a single dictionary entry. Captures:
#   group 1: escaped string content (between the quotes)
#   group 2: declared size
#   group 3: map_index
#
# String content may contain escaped quotes ( \" ) and other C escapes.
# The character class [^"\\] excludes raw quote and backslash; \\. allows
# any escape sequence (\n, \t, \", \\, \xHH, ...).
ENTRY_RE = re.compile(
    r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}'
)


def parse_jsc_map(path: Path) -> list[bytes]:
    """Read the .cpp source and return the words in array order."""
    text = path.read_text(encoding="utf-8")
    words: list[bytes] = []

    # Locate the array definition to bound our parse — avoids accidentally
    # matching anything in the comments or includes.
    start = text.find("JSC::map[262144]")
    if start < 0:
        raise ValueError("Could not find JSC::map[262144] array in source")
    end = text.find("};", start)
    if end < 0:
        raise ValueError("Could not find closing '};' of JSC::map array")

    region = text[start:end]

    # Strip C-style /* ... */ block comments. The upstream source has
    # 32 inline annotations like:
    #     {"\xa1" /* ¡ - "BO60" */, 1, 10704},
    # which break a naive single-line entry regex. We verified upstream
    # that no string literal contains '/*' so a global strip is safe.
    region = re.sub(r'/\*.*?\*/', '', region, flags=re.DOTALL)

    for m in ENTRY_RE.finditer(region):
        raw_str = m.group(1)
        # We ignore the declared `size` (group 2) and `map_index` (group 3)
        # — RX-only audit (see file docstring) confirms only `.str` is read.

        # Decode C-style escapes to actual bytes. unicode_escape handles
        # \n \t \" \\ \xHH and standard sequences correctly.
        try:
            decoded = codecs.decode(raw_str, "unicode_escape").encode("latin-1")
        except UnicodeError as e:
            raise ValueError(
                f"Failed to decode word '{raw_str}': {e}"
            )

        # Defensive: catch any word that's pathologically long, signalling
        # a parser bug. Longest real entry should be ~20 chars.
        if len(decoded) > 255:
            raise ValueError(
                f"Suspiciously long word ({len(decoded)} B): "
                f"raw={raw_str!r} — parser likely broken"
            )

        words.append(decoded)

    return words


def pack(words: list[bytes], out_path: Path) -> None:
    """Write the binary blob: header + offset table + string pool."""
    if len(words) != EXPECTED_COUNT:
        raise ValueError(
            f"Expected exactly {EXPECTED_COUNT} entries, parsed {len(words)}"
        )

    # ── Build the string pool ──
    # Each entry's str_offset = current pool size before appending the word.
    pool = bytearray()
    offsets: list[int] = []
    for word in words:
        offsets.append(len(pool))
        pool.extend(word)
        pool.append(0)  # NUL terminator

    # ── Sanity: size limit check ──
    total = HEADER_SIZE + len(words) * OFFSET_SIZE + len(pool)
    partition_size = 6 * 1024 * 1024
    if total > partition_size:
        raise ValueError(
            f"Packed blob {total} B exceeds 6 MB partition limit "
            f"({partition_size} B)."
        )

    # ── Write ──
    with out_path.open("wb") as f:
        # Header
        f.write(struct.pack(HEADER_FORMAT, MAGIC, VERSION,
                              len(words), len(pool)))
        # Offset table
        for off in offsets:
            f.write(struct.pack(OFFSET_FORMAT, off))
        # String pool
        f.write(pool)

    print(f"  Parsed entries: {len(words):,}")
    print(f"  Offset table:   {len(words) * OFFSET_SIZE:,} B")
    print(f"  String pool:    {len(pool):,} B")
    print(f"  Header:         {HEADER_SIZE} B")
    print(f"  Total file:     {total:,} B  ({total / (1024*1024):.2f} MB)")
    print(f"  Partition:      {partition_size:,} B  "
          f"({(total / partition_size) * 100:.1f}% utilized)")
    print(f"  Output:         {out_path}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input",  type=Path, help="JSC_map.cpp source file")
    ap.add_argument("output", type=Path, help="output .bin file")
    args = ap.parse_args()

    if not args.input.is_file():
        print(f"error: {args.input} not found", file=sys.stderr)
        return 1

    try:
        words = parse_jsc_map(args.input)
        pack(words, args.output)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
