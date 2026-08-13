#!/usr/bin/env bash
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"
mkdir -p build
gcc -O2 -Wall -Wextra -std=c11 \
    -o build/test_terminal \
    tools/test_terminal.c source/terminal.c
./build/test_terminal
