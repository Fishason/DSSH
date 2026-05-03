#!/usr/bin/env python3
"""
Front-facing chibi crab with banana-cat sway gait — body rocks left
and right around the anchored feet.  Both eyes visible, simple 2×2
black squares.  Walk = pendulum tilt, NOT step gait.

Char palette:
    @ = body main (salmon pink)
    F = foot (slightly darker than body)
    # = eye (black square)
    . = transparent

Outputs:
    docs/media/mascot_preview.png   8-frame strip
    docs/media/mascot_preview.gif   loop
"""
from pathlib import Path
from PIL import Image

ROOT     = Path(__file__).parent.parent
OUT_DIR  = ROOT / "docs" / "media"
OUT_DIR.mkdir(parents=True, exist_ok=True)

BG    = (24, 24, 37, 255)
BODY  = (232, 155, 137, 255)
FOOT  = (216, 130, 113, 255)
EYE   = (0, 0, 0, 255)

PALETTE = {
    '@': BODY,
    'F': FOOT,
    '#': EYE,
    '.': None,
}

CRAB_W, CRAB_H = 18, 11

# Front-facing sprite — rounded oval body, 2 black-square eyes, 2 feet.
base_rows = [
    "......@@@@@@......",   # 0  top arc
    "....@@@@@@@@@@....",   # 1
    "..@@@@@@@@@@@@@@..",   # 2  wide
    ".@@@@##@@@@##@@@@.",   # 3  eyes top
    ".@@@@##@@@@##@@@@.",   # 4  eyes bottom
    "@@@@@@@@@@@@@@@@@@",   # 5  widest
    "@@@@@@@@@@@@@@@@@@",   # 6
    ".@@@@@@@@@@@@@@@@.",   # 7  narrowing
    "..@@@@@@@@@@@@@@..",   # 8
    "...@@@@@@@@@@@@...",   # 9  bottom curve
    "...FFF......FFF...",   # 10 two feet
]

# 8-frame sway cycle: 0 → -1 → -2 (left peak) → -1 → 0 → +1 → +2 (right peak) → +1
TILTS = [0, -1, -2, -1, 0, +1, +2, +1]


def shear(amount: int):
    """Row dx for a tilt of `amount` px at the top.  Bottom row 0,
    rows interpolate linearly upward.  Foot row stays at dx=0."""
    dx = []
    for r in range(10):
        dx.append(round(amount * (9 - r) / 9))
    dx.append(0)
    return dx


def frame_metadata(i: int):
    tilt = TILTS[i]
    row_dx = shear(tilt)
    body_dy = -1 if abs(tilt) == 2 else 0
    # At peak left tilt the RIGHT foot is unweighted; at peak right
    # tilt the LEFT foot is unweighted.  The lifted foot nudges up 1 px.
    foot_l_dy = -1 if tilt == +2 else 0
    foot_r_dy = -1 if tilt == -2 else 0
    return row_dx, body_dy, foot_l_dy, foot_r_dy


def render_frame(i: int, scale: int = 16, padding: int = 32) -> Image.Image:
    row_dx, body_dy, fl_dy, fr_dy = frame_metadata(i)
    img_w = CRAB_W * scale + padding * 2
    img_h = CRAB_H * scale + padding * 2
    img = Image.new("RGBA", (img_w, img_h), BG)

    def put(col: int, row: int, color):
        if color is None: return
        x0 = padding + col * scale
        y0 = padding + row * scale
        for yy in range(scale):
            for xx in range(scale):
                px_col, px_row = x0 + xx, y0 + yy
                if 0 <= px_col < img_w and 0 <= px_row < img_h:
                    img.putpixel((px_col, px_row), color)

    for r, src in enumerate(base_rows):
        if r == 10:
            for c, ch in enumerate(src):
                color = PALETTE.get(ch)
                if color is None: continue
                if 3 <= c <= 5:    foot_dy = fl_dy
                elif 12 <= c <= 14: foot_dy = fr_dy
                else: foot_dy = 0
                put(c, r + foot_dy, color)
        else:
            for c, ch in enumerate(src):
                color = PALETTE.get(ch)
                if color is None: continue
                put(c + row_dx[r], r + body_dy, color)

    return img


def main():
    n = len(TILTS)
    frames = [render_frame(i) for i in range(n)]

    strip_w = sum(f.width for f in frames) + (n - 1) * 12
    strip_h = max(f.height for f in frames)
    strip = Image.new("RGBA", (strip_w, strip_h), BG)
    x = 0
    for f in frames:
        strip.paste(f, (x, 0))
        x += f.width + 12
    strip.save(OUT_DIR / "mascot_preview.png")
    print(f"  ✓ {OUT_DIR / 'mascot_preview.png'}  ({strip_w}×{strip_h})")

    frames[0].save(
        OUT_DIR / "mascot_preview.gif",
        save_all=True,
        append_images=frames[1:],
        duration=120,
        loop=0,
        disposal=2,
    )
    print(f"  ✓ {OUT_DIR / 'mascot_preview.gif'}")


if __name__ == "__main__":
    main()
