#!/usr/bin/env bash
# Idempotently install bannertool + makerom into ~/bin so `make cia`
# can produce DSSH.cia from a fresh checkout.
#
#   - bannertool: built from carstene1ns/3ds-bannertool source (the
#     official prebuilts target glibc 2.38 and don't run on Ubuntu 22.04).
#   - makerom: prebuilt v0.18.4 from 3DSGuy/Project_CTR (last release
#     that links against glibc 2.35).
#
# Both binaries land in $HOME/bin which is not on PATH by default.
# The Makefile cia target prepends $HOME/bin so a fresh shell doesn't
# matter.

set -euo pipefail

BIN="$HOME/bin"
mkdir -p "$BIN"

install_bannertool() {
    if [ -x "$BIN/bannertool" ]; then
        echo "  ✓ bannertool already at $BIN/bannertool"
        return
    fi
    echo "↓ building bannertool from source..."
    local tmp; tmp="$(mktemp -d)"
    git clone --depth 1 https://github.com/carstene1ns/3ds-bannertool.git "$tmp/bt" 2>&1 | tail -1
    # 3ds-bannertool requires CMake 3.28; on 22.04 we have 3.22 — same
    # build sequence works after relaxing the version pin.
    sed -i 's/cmake_minimum_required.*/cmake_minimum_required(VERSION 3.22)/' "$tmp/bt/CMakeLists.txt"
    cmake -S "$tmp/bt" -B "$tmp/bt/build" >/dev/null
    cmake --build "$tmp/bt/build" --parallel 2>&1 | tail -1
    cp "$tmp/bt/build/bannertool" "$BIN/bannertool"
    chmod +x "$BIN/bannertool"
    rm -rf "$tmp"
    echo "  ✓ $BIN/bannertool"
}

install_makerom() {
    if [ -x "$BIN/makerom" ]; then
        echo "  ✓ makerom already at $BIN/makerom"
        return
    fi
    echo "↓ downloading makerom v0.18.4..."
    local zip="/tmp/makerom-v0.18.4.zip"
    curl -fLsS -o "$zip" \
        "https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.18.4/makerom-v0.18.4-ubuntu_x86_64.zip"
    python3 -c "
import zipfile, shutil
with zipfile.ZipFile('$zip') as z: z.extractall('/tmp/mk_dssh/')
shutil.move('/tmp/mk_dssh/makerom', '$BIN/makerom')
"
    chmod +x "$BIN/makerom"
    rm -f "$zip"
    rm -rf /tmp/mk_dssh
    echo "  ✓ $BIN/makerom"
}

install_bannertool
install_makerom
"$BIN/bannertool" 2>&1 | head -1
"$BIN/makerom" 2>&1 | head -1
