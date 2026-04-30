#!/usr/bin/env python3
"""
Convert the source 162×102 PNG icon into the two raster assets the
3DS CIA pipeline needs:

  cia_assets/icon.png    48×48   — shows in the HOME menu grid
  cia_assets/banner.png  256×128 — shows when the icon is selected

Strategy:
  - SMDH icon must be square; we center-crop the source to 102×102
    (full height, central column) and downscale to 48×48 with nearest-
    neighbor so the pixel-art edges stay crisp.
  - Banner is 2:1.  The source is 1.59:1 — we paste the icon onto a
    256×128 canvas matching the icon's orange chassis color so the
    selected-app card looks like a single coherent illustration.

Run via tools/gen_cia_assets.py or `make cia` (auto-invoked).
"""
from pathlib import Path
from PIL import Image

ROOT     = Path(__file__).parent.parent
SRC      = ROOT / "69633.PNG"
OUT_DIR  = ROOT / "cia_assets"
# Icon goes at the project root as `icon.png` so devkitPro's Makefile
# picks it up for the .3dsx SMDH (the wildcard at Makefile-parse time
# wants it co-located with the Makefile).  The banner + silent audio
# are CIA-only and live under cia_assets/.
ICON_OUT   = ROOT / "icon.png"
BANNER_OUT = OUT_DIR / "banner.png"

# The console silhouette in the source is this orange.  Picked by
# eyedroppering the source — keeps the banner background blend
# seamless with the icon body.
BG = (242, 137, 33, 255)


def make_icon(src):
    w, h = src.size
    # Center-crop to a square of side = min(w, h).
    side = min(w, h)
    left = (w - side) // 2
    top  = (h - side) // 2
    sq   = src.crop((left, top, left + side, top + side))
    return sq.resize((48, 48), Image.LANCZOS)


def make_banner(src):
    canvas = Image.new("RGBA", (256, 128), BG)
    # Scale the source to fit the canvas, preserving aspect.
    sw, sh = src.size
    scale  = min(256 / sw, 128 / sh)
    nw, nh = int(round(sw * scale)), int(round(sh * scale))
    scaled = src.resize((nw, nh), Image.LANCZOS)
    canvas.paste(scaled, ((256 - nw) // 2, (128 - nh) // 2), scaled)
    return canvas


def main():
    if not SRC.exists():
        raise SystemExit(f"FATAL: source icon {SRC} missing")
    OUT_DIR.mkdir(exist_ok=True)
    src = Image.open(SRC).convert("RGBA")
    icon = make_icon(src)
    icon.save(ICON_OUT)
    banner = make_banner(src)
    banner.save(BANNER_OUT)
    print(f"  ✓ {ICON_OUT}    {icon.size}")
    print(f"  ✓ {BANNER_OUT}  {banner.size}")


if __name__ == "__main__":
    main()
