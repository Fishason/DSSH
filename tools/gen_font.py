#!/usr/bin/env python3
"""
3DS terminal bitmap font atlas generator (M3 simplified, Linux paths).

Renders glyphs from system TTF fonts via Pillow, threshold-quantizes them
into a 6x12 (narrow) / 12x12 (wide) bitmap, and emits source/font_data.c
in the format expected by font_atlas.{c,h}.

This M3 version covers ASCII, box-drawing, math, arrows, and Powerline /
Nerd-Font Private Use Area ranges that tmux + claude code rely on. CJK
Unified Ideographs (the ~20K hanzi range) are intentionally OMITTED here
to keep font_data.c small (~80KB). M6 will regenerate this file with a
full Chinese bitmap font (Zpix or similar).

Fonts used (auto-detected, falls back gracefully):
  - narrow: /usr/share/fonts/truetype/terminus/TerminusTTF-*.ttf
            -> apt install fonts-terminus
  - symbols: /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
  - wide   : /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf (no CJK in M3)

Usage:
  python3 tools/gen_font.py
  -> source/font_data.c
"""
import os
import glob
from PIL import Image, ImageDraw, ImageFont

CELL_W = 6
CELL_H = 12
CELL_W2 = CELL_W * 2  # wide-cell width 12px
THRESHOLD = 80


# ── narrow glyph map ─────────────────────────────────────────
CHAR_MAP = {}
for cp in range(0x20, 0x7F):
    CHAR_MAP[cp] = cp - 0x20  # 0..94

special_narrow = [
    # Box drawing (single + double lines)
    *range(0x2500, 0x2580),
    # Block elements
    *range(0x2580, 0x25A0),
    # Geometric shapes
    *range(0x25A0, 0x2600),
    # Arrows
    *range(0x2190, 0x21FF),
    *range(0x27F0, 0x2800),
    *range(0x2900, 0x2980),
    # Misc Symbols
    *range(0x2600, 0x2700),
    # Dingbats
    *range(0x2700, 0x27C0),
    # Math operators
    *range(0x2200, 0x2300),
    # Misc Technical
    *range(0x2300, 0x2400),
    # Enclosed Alphanumerics ①②③ ...
    *range(0x2460, 0x2500),
    # Letterlike Symbols (™ © etc.) and Number Forms
    *range(0x2100, 0x2200),
    # Currency
    *range(0x20A0, 0x20D0),
    # Superscript / Subscript
    *range(0x2070, 0x20A0),
    # Latin-1 Supplement printable
    *range(0x00A0, 0x0100),
    # Latin Extended-A
    *range(0x0100, 0x0180),
    # Greek (occasional in math output)
    *range(0x0370, 0x0400),
    # Cyrillic basic
    *range(0x0400, 0x0500),
    # Braille patterns (claude code spinner uses these)
    *range(0x2800, 0x2900),
    # Powerline + Nerd-Font PUA (icons in modern shell prompts)
    *range(0xE000, 0xE100),
    *range(0xE0A0, 0xE0D5),
    *range(0xE200, 0xE2A9),
    *range(0xE700, 0xE800),
    *range(0xF000, 0xF400),  # Font Awesome
    *range(0xF400, 0xF500),  # Octicons
    *range(0xF500, 0xF700),  # Material Design
    *range(0xF700, 0xF900),  # More Nerd
    # Misc Symbols and Pictographs (sub-range)
    *range(0x2B00, 0x2C00),
]
idx = 95
for cp in special_narrow:
    if cp not in CHAR_MAP:
        CHAR_MAP[cp] = idx
        idx += 1


# ── wide glyph map (M3 includes full CJK via Zpix bitmap font) ────
# Range covers everything Zpix supports at 12px and what tmux + claude-code
# may emit. Per cp 24 bytes of bitmap → ~500KB worth of CJK glyphs.
WIDE_CHAR_MAP = {}
wide_chars = []
wide_chars += list(range(0x3000, 0x3040))   # CJK punctuation
wide_chars += list(range(0x3040, 0x30FF))   # Hiragana + Katakana
wide_chars += list(range(0x4E00, 0x9FFF))   # CJK Unified Ideographs (~20,991 cp)
wide_chars += list(range(0xFF00, 0xFF60))   # Fullwidth ASCII
wide_chars += list(range(0xFFE0, 0xFFE7))   # Fullwidth currency/symbols
wide_idx_counter = 0
for cp in wide_chars:
    if cp not in WIDE_CHAR_MAP:
        WIDE_CHAR_MAP[cp] = wide_idx_counter
        wide_idx_counter += 1

NGLYPHS = idx
NGLYPHS_WIDE = max(wide_idx_counter, 1)  # never zero — keep at least 1 for table validity


# ── font discovery ─────────────────────────────────────────────
def find_font(*patterns):
    for pat in patterns:
        for p in glob.glob(pat):
            try:
                return ImageFont.truetype(p, CELL_H), p
            except Exception:
                continue
    return None, None

font_narrow, narrow_path = find_font(
    "/usr/share/fonts/truetype/terminus/TerminusTTF-*.ttf",
    "/usr/share/fonts/**/Terminus*.ttf",
    "/usr/share/fonts/**/DejaVuSansMono.ttf",
)
font_symbols, symbols_path = find_font(
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/**/Hack-Regular.ttf",
)
# Wide font: Zpix (Chinese 12px pixel font, covers Hiragana/Katakana/CJK).
font_wide, wide_path = find_font(
    os.path.join(os.path.dirname(__file__), "..", "data", "fonts", "zpix.ttf"),
    "/home/ubuntu/my/3dssh/data/fonts/zpix.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
)

if font_narrow is None:
    raise SystemExit("FATAL: no narrow font found. apt install fonts-terminus")
print(f"narrow   : {narrow_path}")
print(f"symbols  : {symbols_path or '(none)'}")
print(f"wide     : {wide_path or '(none)'}")
print(f"narrow glyphs target: {NGLYPHS}")
print(f"wide   glyphs target: {NGLYPHS_WIDE}")


# ── render one glyph to a binary row list ──────────────────────
def render_rows(cp, fnt, w, h):
    img = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(img)
    try:
        bbox = fnt.getbbox(chr(cp))
        if bbox and bbox[2] > bbox[0]:
            cw = bbox[2] - bbox[0]
            ch2 = bbox[3] - bbox[1]
            ox = max(0, (w - cw) // 2) - bbox[0]
            oy = max(0, (h - ch2) // 2) - bbox[1]
            draw.text((ox, oy), chr(cp), font=fnt, fill=255)
        else:
            draw.text((0, 0), chr(cp), font=fnt, fill=255)
    except Exception:
        pass
    px = list(img.getdata())
    rows = []
    for r in range(h):
        bits = 0
        for c in range(w):
            if px[r * w + c] >= THRESHOLD:
                bits |= 1 << (w - 1 - c)
        rows.append(bits)
    return rows


# Detect Terminus' "not-defined" placeholder so we treat it as empty and fall
# back to a different font.
notdef_rows = render_rows(0xFFFF, font_narrow, CELL_W, CELL_H)


def is_empty_or_notdef(rows):
    return all(b == 0 for b in rows) or rows == notdef_rows


# ── narrow glyph generation ────────────────────────────────────
glyph_bitmaps = [[0] * CELL_H for _ in range(NGLYPHS)]
empty_cps = set()
for cp, atlas_idx in CHAR_MAP.items():
    rows = render_rows(cp, font_narrow, CELL_W, CELL_H)
    if cp > 0x7E and is_empty_or_notdef(rows) and font_symbols is not None:
        rows = render_rows(cp, font_symbols, CELL_W, CELL_H)
    if all(b == 0 for b in rows) and cp > 0x7E:
        empty_cps.add(cp)
    else:
        glyph_bitmaps[atlas_idx] = rows

for cp in empty_cps:
    del CHAR_MAP[cp]
print(f"  narrow filled: {len(CHAR_MAP)}  empty dropped: {len(empty_cps)}")


# ── wide glyph generation (M3: punctuation only) ───────────────
wide_bitmaps = [[0] * CELL_H for _ in range(NGLYPHS_WIDE)]
wide_empty = set()
for cp, atlas_idx in WIDE_CHAR_MAP.items():
    rows = render_rows(cp, font_wide or font_narrow, CELL_W2, CELL_H)
    if all(b == 0 for b in rows):
        wide_empty.add(cp)
    else:
        wide_bitmaps[atlas_idx] = rows
for cp in wide_empty:
    del WIDE_CHAR_MAP[cp]
print(f"  wide   filled: {len(WIDE_CHAR_MAP)}  empty dropped: {len(wide_empty)}")


# ── emit source/font_data.c ────────────────────────────────────
out_path = os.path.join(os.path.dirname(__file__), "..", "source", "font_data.c")
out_path = os.path.normpath(out_path)
with open(out_path, "w") as f:
    f.write("/* Auto-generated by tools/gen_font.py — do not edit by hand */\n")
    f.write('#include "font_atlas.h"\n\n')
    f.write(f"const int FONT_NGLYPHS      = {NGLYPHS};\n")
    f.write(f"const int FONT_WIDE_NGLYPHS = {NGLYPHS_WIDE};\n")
    f.write(f"const int FA_CELL_W         = {CELL_W};\n")
    f.write(f"const int FA_CELL_H         = {CELL_H};\n\n")

    # narrow glyphs
    f.write(f"const uint8_t font_glyphs[{NGLYPHS}][{CELL_H}] = {{\n")
    for gi in range(NGLYPHS):
        rs = ", ".join(f"0x{b:02x}" for b in glyph_bitmaps[gi])
        f.write(f"    {{ {rs} }},\n")
    f.write("};\n\n")

    # wide glyphs (always emit at least one entry to avoid zero-size array)
    f.write(f"const uint16_t font_wide_glyphs[{NGLYPHS_WIDE}][{CELL_H}] = {{\n")
    for gi in range(NGLYPHS_WIDE):
        rs = ", ".join(f"0x{b:04x}" for b in wide_bitmaps[gi])
        f.write(f"    {{ {rs} }},\n")
    f.write("};\n\n")

    # narrow lookup table
    pairs = sorted(CHAR_MAP.items())
    f.write(f"const int FONT_UNICODE_MAP_LEN = {len(pairs)};\n")
    f.write(f"const uint32_t font_unicode_cps[{len(pairs)}] = {{\n    ")
    f.write(", ".join(str(cp) for cp, _ in pairs))
    f.write("\n};\n")
    f.write(f"const uint16_t font_unicode_idx[{len(pairs)}] = {{\n    ")
    f.write(", ".join(str(i) for _, i in pairs))
    f.write("\n};\n\n")

    # wide lookup table
    wpairs = sorted(WIDE_CHAR_MAP.items())
    wlen = max(len(wpairs), 1)
    f.write(f"const int FONT_WIDE_MAP_LEN = {len(wpairs)};\n")
    if wpairs:
        f.write(f"const uint32_t font_wide_cps[{wlen}] = {{\n    ")
        f.write(", ".join(str(cp) for cp, _ in wpairs))
        f.write("\n};\n")
        f.write(f"const uint16_t font_wide_idx[{wlen}] = {{\n    ")
        f.write(", ".join(str(i) for _, i in wpairs))
        f.write("\n};\n")
    else:
        f.write("const uint32_t font_wide_cps[1] = { 0 };\n")
        f.write("const uint16_t font_wide_idx[1] = { 0 };\n")

print(f"written: {out_path}")
