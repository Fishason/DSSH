#!/usr/bin/env bash
# Compile + run the host-side IME smoke test.  Uses system gcc, no
# 3DS toolchain needed — much faster iteration than `make` + 3dslink.
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"
mkdir -p build
gcc -O2 -Wall -Wextra -std=c11 \
    -o build/test_ime \
    tools/test_ime.c source/ime_pinyin.c
./build/test_ime "$@"
