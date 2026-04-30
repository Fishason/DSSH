#!/usr/bin/env python3
"""
Build romfs/pinyin_dict.bin from rime-ice YAML dict files.

Reads data/pinyin_dict_src/*.yaml (downloaded by fetch_pinyin_dict.sh),
deduplicates by (word, joined-pinyin), serializes to a binary that the
on-device IME engine can mmap-style load and binary-search.

Binary format (little-endian, 28-byte header):

    char     magic[4] = "PYIN"
    uint32_t version = 1
    uint32_t n_entries
    uint32_t pinyin_pool_off  (absolute file offset)
    uint32_t pinyin_pool_len
    uint32_t word_pool_off    (absolute file offset)
    uint32_t word_pool_len

    Entry[n_entries] {
        uint32_t pinyin_off  // offset within pinyin_pool, NUL-term ASCII
        uint32_t word_off    // offset within word_pool, NUL-term UTF-8
        uint32_t freq        // higher = more common
    }
    pinyin_pool[pinyin_pool_len]   // packed null-terminated lowercase ASCII
    word_pool[word_pool_len]       // packed null-terminated UTF-8

Entries are sorted by pinyin string lexicographically, with ties broken
by descending freq (so the most-common entry for a given pinyin shows
up first in linear scans).
"""
import struct
import sys
from pathlib import Path

ROOT       = Path(__file__).parent.parent
SRC_DIR    = ROOT / "data" / "pinyin_dict_src"
ROMFS_DIR  = ROOT / "romfs"
OUT_PATH   = ROMFS_DIR / "pinyin_dict.bin"

# Order matters only for the "first occurrence wins on weight tie" path
# below — content-wise these all merge.
DICT_FILES = [
    "8105.dict.yaml",
    "41448.dict.yaml",
    "others.dict.yaml",
    "base.dict.yaml",
    "ext.dict.yaml",
    "tencent.dict.yaml",
]


def parse_dict_file(path):
    """Yield (word, joined_pinyin, weight) for each entry.

    rime-ice format after the YAML frontmatter `---` ... `...` block:
        <word>\\t<space-separated-pinyin>\\t<weight>
    Lines starting with `#` and blank lines are ignored.
    """
    after_doc_end = False
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not after_doc_end:
                if line == "...":
                    after_doc_end = True
                continue
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            word = parts[0].strip()
            pinyin_raw = parts[1].strip() if len(parts) > 1 else ""
            try:
                weight = int(parts[2].strip()) if len(parts) > 2 else 1
            except ValueError:
                weight = 1
            if not word or not pinyin_raw:
                continue
            # Join syllables and drop any apostrophe separators rime-ice
            # occasionally uses ("xi'an" → "xian").
            pinyin = pinyin_raw.replace(" ", "").replace("'", "")
            # Only ASCII a-z is valid; reject anything else (rare but
            # keeps the binary compact).
            if not pinyin or not all("a" <= c <= "z" for c in pinyin):
                continue
            yield (word, pinyin, weight)


def main():
    if not SRC_DIR.exists():
        sys.exit(f"FATAL: {SRC_DIR} missing — run tools/fetch_pinyin_dict.sh first")

    # Phase 1: collect, dedupe (word, pinyin), keep the max weight seen.
    entries = {}  # (word, pinyin) -> weight
    for fname in DICT_FILES:
        path = SRC_DIR / fname
        if not path.exists():
            print(f"  WARN: {path} missing, skipping")
            continue
        before = len(entries)
        for word, pinyin, weight in parse_dict_file(path):
            key = (word, pinyin)
            if key not in entries or weight > entries[key]:
                entries[key] = weight
        print(f"  {fname:<22} +{len(entries) - before:>7} unique  "
              f"(running total: {len(entries):>7})")

    if not entries:
        sys.exit("FATAL: no entries parsed")

    # Phase 2a: trim to the top-N by weight.  rime-ice has ~900k entries
    # combined; the long-tail is rare technical / regional / archaic
    # vocab that bloats the binary without helping everyday input.
    # 300k caps the .3dsx around 16 MB while keeping ~99% coverage of
    # daily typing (the user explicitly chose this size).
    MAX_ENTRIES = 300_000
    flat = [(p, w, weight) for (w, p), weight in entries.items()]
    if len(flat) > MAX_ENTRIES:
        flat.sort(key=lambda e: -e[2])              # by weight desc
        cut_weight = flat[MAX_ENTRIES - 1][2]
        flat = flat[:MAX_ENTRIES]
        print(f"  trimmed to top {MAX_ENTRIES:,} (cutoff weight = {cut_weight})")

    # Phase 2b: sort by pinyin asc, then weight desc, then word asc.
    # The pinyin sort is required for binary-search prefix lookup at
    # runtime; the secondary keys make the on-disk order deterministic.
    sorted_entries = sorted(flat, key=lambda e: (e[0], -e[2], e[1]))

    # Phase 3: build string pools, deduplicating.
    pinyin_offsets = {}      # pinyin -> offset in pool
    word_offsets   = {}      # word   -> offset in pool
    pinyin_pool    = bytearray()
    word_pool      = bytearray()

    for pinyin, word, _ in sorted_entries:
        if pinyin not in pinyin_offsets:
            pinyin_offsets[pinyin] = len(pinyin_pool)
            pinyin_pool.extend(pinyin.encode("ascii"))
            pinyin_pool.append(0)
        if word not in word_offsets:
            word_offsets[word] = len(word_pool)
            word_pool.extend(word.encode("utf-8"))
            word_pool.append(0)

    # Phase 4: build the entry table (12 bytes each).
    entry_table = bytearray()
    for pinyin, word, weight in sorted_entries:
        entry_table.extend(struct.pack(
            "<III",
            pinyin_offsets[pinyin],
            word_offsets[word],
            weight,
        ))

    # Phase 5: header + concat + write.
    HEADER_SIZE     = 28
    pinyin_pool_off = HEADER_SIZE + len(entry_table)
    word_pool_off   = pinyin_pool_off + len(pinyin_pool)

    header = struct.pack(
        "<4sIIIIII",
        b"PYIN",
        1,                       # version
        len(sorted_entries),
        pinyin_pool_off,
        len(pinyin_pool),
        word_pool_off,
        len(word_pool),
    )

    ROMFS_DIR.mkdir(exist_ok=True)
    with open(OUT_PATH, "wb") as f:
        f.write(header)
        f.write(entry_table)
        f.write(pinyin_pool)
        f.write(word_pool)

    total = OUT_PATH.stat().st_size
    mb = total / (1024 * 1024)
    print()
    print(f"✓ {OUT_PATH}")
    print(f"  entries     : {len(sorted_entries):>10,}")
    print(f"  unique pinyins: {len(pinyin_offsets):>8,}")
    print(f"  unique words  : {len(word_offsets):>8,}")
    print(f"  pinyin pool : {len(pinyin_pool):>10,} bytes")
    print(f"  word pool   : {len(word_pool):>10,} bytes")
    print(f"  total       : {total:>10,} bytes ({mb:.2f} MB)")


if __name__ == "__main__":
    main()
