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
CELL_H = 12   # native design size of both Terminus and Zpix.  See font_atlas.h.
CELL_W2 = CELL_W * 2
THRESHOLD = 80

WIDE_FONT_NATIVE = 12  # match CELL_H — Zpix is pixel-perfect at exactly 12px.


# ── narrow glyph map ─────────────────────────────────────────
CHAR_MAP = {}
for cp in range(0x20, 0x7F):
    CHAR_MAP[cp] = cp - 0x20  # 0..94

special_narrow = [
    # General Punctuation: em dash —, en dash –, ellipsis …, curly quotes,
    # bullets, prime, etc.  Heavy use in Chinese text and Markdown output.
    *range(0x2000, 0x2070),
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
    # Misc Technical (incl. ⏵⏸⏯ play/pause)
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
# Yazi-specific icon codepoints (extracted from yazi's default theme).
# Includes Devicons + Codicons + Material Design icons in both the BMP
# PUA (0xE000-0xF900) and the Nerd Font v3 supplementary plane
# (U+F0001-U+F1FFF).  Loaded from data/yazi_icons.txt so the list can
# be regenerated when yazi releases new themes.
yazi_icons = []
yazi_icons_path = os.path.join(os.path.dirname(__file__),
                               "yazi_icons.txt")
if os.path.exists(yazi_icons_path):
    with open(yazi_icons_path) as fyz:
        for line in fyz:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                yazi_icons.append(int(line, 16))
            except ValueError:
                pass

idx = 95
for cp in special_narrow:
    if cp not in CHAR_MAP:
        CHAR_MAP[cp] = idx
        idx += 1
for cp in yazi_icons:
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
def find_font(size, *patterns):
    for pat in patterns:
        for p in glob.glob(pat):
            try:
                return ImageFont.truetype(p, size), p
            except Exception:
                continue
    return None, None

font_narrow, narrow_path = find_font(CELL_H,
    "/usr/share/fonts/truetype/terminus/TerminusTTF-*.ttf",
    "/usr/share/fonts/**/Terminus*.ttf",
    "/usr/share/fonts/**/DejaVuSansMono.ttf",
)
font_symbols, symbols_path = find_font(CELL_H,
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/**/Hack-Regular.ttf",
)
# Nerd Font (Symbols Only) — covers Devicons, Codicons, Font Awesome,
# Octicons, Material Design icons (incl. v3 supplementary plane).  Used
# as the priority source for any cp the yazi icon set requests, and as
# a fallback for any cp that Terminus/DejaVu can't render.
font_nerd, nerd_path = find_font(CELL_H,
    os.path.join(os.path.dirname(__file__), "..", "data", "fonts",
                 "SymbolsNerdFontMono-Regular.ttf"),
)
# Wide font: Zpix (Chinese 12px pixel font).  Always loaded at its native
# WIDE_FONT_NATIVE size; render_rows will clip top/bottom into the cell.
font_wide, wide_path = find_font(WIDE_FONT_NATIVE,
    os.path.join(os.path.dirname(__file__), "..", "data", "fonts", "zpix.ttf"),
    "/home/ubuntu/my/3dssh/data/fonts/zpix.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
)

if font_narrow is None:
    raise SystemExit("FATAL: no narrow font found. apt install fonts-terminus")
print(f"narrow   : {narrow_path}")
print(f"symbols  : {symbols_path or '(none)'}")
print(f"nerd     : {nerd_path or '(none — yazi icons will fall through)'}")
print(f"wide     : {wide_path or '(none)'}")
print(f"narrow glyphs target: {NGLYPHS}  (incl. {len(yazi_icons)} yazi cps)")
print(f"wide   glyphs target: {NGLYPHS_WIDE}")


# ── render one glyph to a binary row list ──────────────────────
#
# Two strategies:
#   render_rows_baseline (used for ASCII / monospace narrow text):
#     draw at (x_center, baseline_oy) using the FONT's ascent metric so
#     every glyph in the same font shares one baseline.  This is what
#     real text renderers do; centering by ink-bbox would make
#     descender chars (g/p/y) and tall caps (H) drift independently and
#     a row of text looks "wonky" (用户反馈：歪七扭八).
#
#   render_rows_centered (for symbol / icon fonts where bbox-centering
#     looks better than baseline alignment, e.g. ◻ / arrows / Powerline):
#     centers the ink box in the cell.

def render_rows_pixels_to_bits(img, w, h):
    px = list(img.getdata())
    rows = []
    for r in range(h):
        bits = 0
        for c in range(w):
            if px[r * w + c] >= THRESHOLD:
                bits |= 1 << (w - 1 - c)
        rows.append(bits)
    return rows


def render_rows_baseline(cp, fnt, w, h):
    """Baseline-anchored render. Best for ASCII/monospace continuous text.

    Pillow's draw.text((x,y)) places the text top at y and the baseline at
    y + ascent.  To put the baseline at cell row (h - descent) (leaving
    enough room for a full descender), solve for y:
        y + ascent = h - descent
        y = h - descent - ascent
    """
    img = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(img)
    try:
        ascent, descent = fnt.getmetrics()
        # Leave at least 1 row for descender (if font has any), at most descent.
        descent_room = max(1, descent) if descent > 0 else 0
        oy = h - descent_room - ascent
        # Monospace fonts have uniform advance — no horizontal centering needed.
        draw.text((0, oy), chr(cp), font=fnt, fill=255)
    except Exception:
        pass
    return render_rows_pixels_to_bits(img, w, h)


def render_rows_centered(cp, fnt, w, h):
    """Ink-bbox centered render. Best for isolated symbols / icons.

    For glyphs whose ink box is WIDER than the cell (Powerline caps
    E0B4/E0B6, some MDI icons), the naive centered draw places ink
    outside the cell.  Detect that case and fall through to a
    proportional resize so the ink at least shows up — even if
    horizontally squished, a rounded-cap or icon silhouette is more
    useful than a `?`. """
    img = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(img)
    try:
        bbox = fnt.getbbox(chr(cp))
        if not (bbox and bbox[2] > bbox[0]):
            return render_rows_pixels_to_bits(img, w, h)
        cw  = bbox[2] - bbox[0]
        ch2 = bbox[3] - bbox[1]

        if cw <= w and ch2 <= h:
            # Fits — center directly in the cell.
            ox = max(0, (w - cw) // 2) - bbox[0]
            oy = max(0, (h - ch2) // 2) - bbox[1]
            draw.text((ox, oy), chr(cp), font=fnt, fill=255)
            return render_rows_pixels_to_bits(img, w, h)

        # Too big for the cell — render to a canvas matching the bbox,
        # then resize down to (w, h).  Lossy but preserves silhouette.
        bw = max(1, cw)
        bh = max(1, ch2)
        big = Image.new("L", (bw, bh), 0)
        ImageDraw.Draw(big).text((-bbox[0], -bbox[1]),
                                 chr(cp), font=fnt, fill=255)
        # Threshold + resize so we don't get gray-squashed lines that
        # vanish below THRESHOLD.  L→1-bit→L round-trip keeps edges crisp.
        big = big.point(lambda v: 255 if v >= THRESHOLD else 0)
        img = big.resize((w, h), Image.LANCZOS)
    except Exception:
        pass
    return render_rows_pixels_to_bits(img, w, h)


# Default for narrow path: baseline-anchored when the source is the
# Terminus monospace bitmap; ink-centered when the source is the symbol
# fallback (DejaVu Sans Mono / Zpix).  Helps mixed-font rows look uniform.
def render_rows(cp, fnt, w, h):
    return render_rows_baseline(cp, fnt, w, h)


# Each font has its own ".notdef" placeholder glyph (the rectangle frame
# Pillow renders for chars the font doesn't actually have).  Terminus's
# notdef is 5px wide, DejaVu's is 6px — different bitmaps but both mean
# "this font doesn't have this glyph".  Render 0xFFFF (which no font has)
# through each font's actual rendering path to capture its specific
# notdef pattern, then treat any glyph that exactly matches as missing.
notdef_terminus = render_rows_baseline(0xFFFF, font_narrow, CELL_W, CELL_H)
notdef_dejavu   = render_rows_centered(0xFFFF, font_symbols, CELL_W, CELL_H) if font_symbols else None
notdef_nerd     = render_rows_centered(0xFFFF, font_nerd,    CELL_W, CELL_H) if font_nerd    else None
notdef_zpix     = render_rows_centered(0xFFFF, font_wide,    CELL_W, CELL_H) if font_wide    else None
yazi_icons_set  = set(yazi_icons)


def is_empty_or_notdef(rows, font_notdef):
    """Empty bitmap, or exactly equal to this font's own notdef glyph.
    No heuristic — heuristics misfired and dropped legit chars like H, O,
    [, ] whose silhouette can vaguely resemble a rectangle frame."""
    if all(b == 0 for b in rows):
        return True
    if font_notdef is not None and rows == font_notdef:
        return True
    return False


# ── narrow glyph generation ────────────────────────────────────
# Fallback chain (each step rejects per-font notdef + empty):
#
# Nerd Font cps in the yazi set get NF first (most Devicons / MDI v3
# only exist there); everything else starts with Terminus.  Both paths
# end in NF / DejaVu / Zpix as further fallbacks, so an icon that NF
# happens to lack still gets a chance to render through DejaVu's
# ▢/▶/▮/⛶ glyphs.
#
# If everything returns notdef, drop from CHAR_MAP so the C-side
# fallback_alias() in font_atlas.c can substitute a similar glyph.
def render_with_chain(cp, fonts):
    """Try each (font, render_fn, notdef) in order; first non-notdef wins.
    `fonts` is a list of (font_obj, render_fn, notdef_rows) tuples."""
    for fnt, rfn, nd in fonts:
        if fnt is None:
            continue
        rows = rfn(cp, fnt, CELL_W, CELL_H)
        if not is_empty_or_notdef(rows, nd):
            return rows
    return [0] * CELL_H

# Two chains.  The "icon-first" chain is used for cps yazi explicitly
# pulls from Nerd Font (Devicons / Codicons / MDI / Powerline caps).
# The "text-first" chain is the default for everything else.
icon_first = [
    (font_nerd,    render_rows_centered, notdef_nerd),
    (font_narrow,  render_rows_baseline, notdef_terminus),
    (font_symbols, render_rows_centered, notdef_dejavu),
    (font_wide,    render_rows_centered, notdef_zpix),
]
text_first = [
    (font_narrow,  render_rows_baseline, notdef_terminus),
    (font_symbols, render_rows_centered, notdef_dejavu),
    (font_nerd,    render_rows_centered, notdef_nerd),
    (font_wide,    render_rows_centered, notdef_zpix),
]

glyph_bitmaps = [[0] * CELL_H for _ in range(NGLYPHS)]
empty_cps = set()
for cp, atlas_idx in CHAR_MAP.items():
    chain = icon_first if cp in yazi_icons_set else text_first
    # ASCII (cp <= 0x7E) sticks to baseline through Terminus regardless.
    if cp <= 0x7E:
        rows = render_rows_baseline(cp, font_narrow, CELL_W, CELL_H)
    else:
        rows = render_with_chain(cp, chain)
    if all(b == 0 for b in rows) and cp > 0x7E:
        empty_cps.add(cp)
    else:
        glyph_bitmaps[atlas_idx] = rows

for cp in empty_cps:
    del CHAR_MAP[cp]
print(f"  narrow filled: {len(CHAR_MAP)}  empty dropped: {len(empty_cps)}")


# ── wide glyph generation (CJK via Zpix at native 12pt) ────────
# Use baseline-anchored render so the visual baseline matches the narrow row.
wide_bitmaps = [[0] * CELL_H for _ in range(NGLYPHS_WIDE)]
wide_empty = set()
for cp, atlas_idx in WIDE_CHAR_MAP.items():
    rows = render_rows_baseline(cp, font_wide or font_narrow, CELL_W2, CELL_H)
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
    f.write(f"const int FONT_WIDE_NGLYPHS = {NGLYPHS_WIDE};\n\n")
    # FA_CELL_W / FA_CELL_H are now #defines in font_atlas.h, do NOT emit
    # them as runtime constants here (would conflict with the macros).

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
