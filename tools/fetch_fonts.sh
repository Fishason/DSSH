#!/usr/bin/env bash
# Download bitmap fonts needed by tools/gen_font.py.
# Idempotent: skips files that already exist with the right size.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FONT_DIR="$PROJECT_ROOT/data/fonts"
mkdir -p "$FONT_DIR"

# Zpix — 12px Chinese pixel font (OFL-licensed) by SolidZORO.
# Covers Simplified + Traditional Chinese, Hiragana, Katakana, ASCII.
ZPIX_VERSION="v3.1.11"
ZPIX_URL="https://github.com/SolidZORO/zpix-pixel-font/releases/download/${ZPIX_VERSION}/zpix.ttf"
ZPIX_PATH="$FONT_DIR/zpix.ttf"
ZPIX_EXPECTED_BYTES=7179288

if [ -f "$ZPIX_PATH" ]; then
    actual=$(stat -c%s "$ZPIX_PATH")
    if [ "$actual" = "$ZPIX_EXPECTED_BYTES" ]; then
        echo "zpix.ttf already present ($actual bytes)"
    else
        echo "zpix.ttf size mismatch (got $actual, want $ZPIX_EXPECTED_BYTES) — redownloading"
        rm -f "$ZPIX_PATH"
    fi
fi

if [ ! -f "$ZPIX_PATH" ]; then
    echo "Downloading $ZPIX_URL -> $ZPIX_PATH"
    if command -v gh >/dev/null 2>&1; then
        # GitHub release downloads via gh CLI bypass Cloudflare anti-bot on AWS IPs.
        (cd "$FONT_DIR" && gh release download "$ZPIX_VERSION" --repo SolidZORO/zpix-pixel-font --pattern 'zpix.ttf')
    else
        curl -fLo "$ZPIX_PATH" "$ZPIX_URL"
    fi
fi

# Terminus TTF — narrow ASCII/box-drawing pixel font.
# Distributed by Debian/Ubuntu in the fonts-terminus apt package.
if [ ! -d /usr/share/fonts/truetype/terminus ]; then
    echo "Terminus TTF not found at /usr/share/fonts/truetype/terminus/"
    echo "Install with: sudo apt install fonts-terminus"
    exit 1
fi

echo "Fonts ready:"
ls -la "$FONT_DIR" /usr/share/fonts/truetype/terminus/ 2>/dev/null
