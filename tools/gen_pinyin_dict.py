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

# Words removed from the output dict on user request.
BLOCKED_WORDS = {
    "江泽民",
    "江泽民同志",
    "三国群英传",
}

# Optional curated word list — 2024-2025 vocabulary that rime-ice's
# weight-trimmed top-300k may not cover well (LLM/AI terms, recent
# internet slang, modern tech).  Format mirrors rime-ice yaml entries
# after the `...` document end marker:  `<word>\t<pinyin>\t<weight>`.
MODERN_WORDS_FILE = "tools/modern_words.txt"

# Per-syllable abbreviation: the first letter of every syllable.  Adding
# entries with the abbreviated pinyin lets users type "nh" and still
# reach 你好 (ni hao) — common in Chinese IMEs as "shuangpin lite" or
# "shengmu input".  Abbrev entries get a reduced weight so the full-
# pinyin form (when typed completely) still ranks first.
ABBREV_WEIGHT_FACTOR = 0.3


def parse_dict_file(path):
    """Yield (word, joined_pinyin, weight, syllables_tuple) for each entry.

    rime-ice format after the YAML frontmatter `---` ... `...` block:
        <word>\\t<space-separated-pinyin>\\t<weight>
    The syllables tuple is needed downstream to derive abbreviation
    entries (one-letter-per-syllable shortcuts like "nh" → 你好).
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
            # rime occasionally uses an apostrophe to disambiguate
            # syllable boundaries ("xi'an"); strip it so the joined
            # form is just letters.
            cleaned = pinyin_raw.replace("'", "")
            syllables = tuple(s for s in cleaned.split() if s)
            joined = "".join(syllables)
            # Only ASCII a-z is valid; reject anything else.
            if not joined or not all("a" <= c <= "z" for c in joined):
                continue
            yield (word, joined, weight, syllables)


def main():
    if not SRC_DIR.exists():
        sys.exit(f"FATAL: {SRC_DIR} missing — run tools/fetch_pinyin_dict.sh first")

    # Phase 1: collect, dedupe (word, pinyin), keep the max weight seen
    # plus the syllable splitting (needed for abbrev entries).
    entries = {}  # (word, pinyin) -> (weight, syllables)
    skipped_blocked = 0
    for fname in DICT_FILES:
        path = SRC_DIR / fname
        if not path.exists():
            print(f"  WARN: {path} missing, skipping")
            continue
        before = len(entries)
        for word, pinyin, weight, syl in parse_dict_file(path):
            if word in BLOCKED_WORDS:
                skipped_blocked += 1
                continue
            key = (word, pinyin)
            if key not in entries or weight > entries[key][0]:
                entries[key] = (weight, syl)
        print(f"  {fname:<22} +{len(entries) - before:>7} unique  "
              f"(running total: {len(entries):>7})")
    if skipped_blocked:
        print(f"  blocked: {skipped_blocked} entries removed by BLOCKED_WORDS")

    # Phase 1b: overlay curated modern-vocabulary entries.  These
    # bypass the trim cut-off below by virtue of their high weight.
    modern_path = ROOT / MODERN_WORDS_FILE
    if modern_path.exists():
        added = 0
        for word, pinyin, weight, syl in parse_dict_file(modern_path):
            if word in BLOCKED_WORDS:
                continue
            key = (word, pinyin)
            if key not in entries or weight > entries[key][0]:
                entries[key] = (weight, syl)
                added += 1
        print(f"  {MODERN_WORDS_FILE:<22} +{added:>7} curated modern entries")

    if not entries:
        sys.exit("FATAL: no entries parsed")

    # Phase 2a: trim full-pinyin entries to the top-N by weight.
    # rime-ice has ~900k entries combined; the long-tail is rare
    # technical / regional / archaic vocab that bloats the binary
    # without helping everyday input.  300k caps the .3dsx around
    # 14 MB even with abbrev entries added in Phase 2c.
    MAX_ENTRIES = 300_000
    flat = [(pinyin, word, w, syl)
            for (word, pinyin), (w, syl) in entries.items()]
    if len(flat) > MAX_ENTRIES:
        flat.sort(key=lambda e: -e[2])              # by weight desc
        cut_weight = flat[MAX_ENTRIES - 1][2]
        flat = flat[:MAX_ENTRIES]
        print(f"  full-pinyin trimmed to top {MAX_ENTRIES:,} "
              f"(cutoff weight = {cut_weight})")

    # Phase 2b: derive abbreviation entries — one letter per syllable.
    # E.g. (你好, "nihao", 70000, ("ni","hao")) → +(你好, "nh", 21000).
    # Skip entries where the abbreviation equals the full pinyin (1
    # syllable) or where any syllable is empty.  Dedupe so the same
    # (word, abbrev) only appears once even if it would arise from
    # multiple full-pinyin variants.
    abbrev_map = {}  # (word, abbrev) -> weight
    for pinyin, word, weight, syllables in flat:
        if len(syllables) < 2:
            continue
        abbrev = "".join(s[0] for s in syllables)
        if abbrev == pinyin:
            continue
        if not all("a" <= c <= "z" for c in abbrev):
            continue
        key = (word, abbrev)
        adj  = max(1, int(weight * ABBREV_WEIGHT_FACTOR))
        if key not in abbrev_map or adj > abbrev_map[key]:
            abbrev_map[key] = adj
    abbrev_entries = [(abbrev, word, w, ()) for (word, abbrev), w in abbrev_map.items()]
    print(f"  abbrev entries added: {len(abbrev_entries):,}")

    # Phase 2c: combine + sort by pinyin asc, weight desc, word asc.
    # Re-dedupe via dict to defend against the rare case where an
    # abbreviation collides with a real full-pinyin spelling.
    combined = {}
    for pinyin, word, w, _ in (flat + abbrev_entries):
        key = (pinyin, word)
        if key not in combined or w > combined[key]:
            combined[key] = w
    sorted_entries = sorted(
        ((p, w, weight) for (p, w), weight in combined.items()),
        key=lambda e: (e[0], -e[2], e[1]),
    )

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
