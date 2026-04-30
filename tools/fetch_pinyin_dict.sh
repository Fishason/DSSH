#!/usr/bin/env bash
# Download rime-ice pinyin dictionary YAML sources.
# Idempotent: skips files that already exist with the right size.
#
# We pin a specific commit hash so the binary the IME loads doesn't
# silently change every time we rebuild — upstream rime-ice updates
# weights frequently and the dict format has historically changed once.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DICT_SRC="$PROJECT_ROOT/data/pinyin_dict_src"
mkdir -p "$DICT_SRC"

# Pinned at iDvel/rime-ice main as of 2026-04-27.
RIME_ICE_COMMIT="3f57a6f69b3393f9fccaae216d3439325786a62f"
RAW="https://raw.githubusercontent.com/iDvel/rime-ice/${RIME_ICE_COMMIT}/cn_dicts"

# (filename, expected_size_bytes) tuples — sizes from GitHub API at
# this commit, verified before pinning.
DICT_FILES=(
    "8105.dict.yaml:114111"
    "41448.dict.yaml:387281"
    "others.dict.yaml:16862"
    "base.dict.yaml:16620104"
    "ext.dict.yaml:11927296"
    "tencent.dict.yaml:17362395"
)

for entry in "${DICT_FILES[@]}"; do
    name="${entry%%:*}"
    want="${entry##*:}"
    out="$DICT_SRC/$name"

    if [ -f "$out" ]; then
        actual=$(stat -c%s "$out")
        if [ "$actual" = "$want" ]; then
            echo "✓ $name ($actual bytes)"
            continue
        fi
        echo "✗ $name size mismatch (got $actual, want $want) — redownloading"
        rm -f "$out"
    fi

    echo "↓ $name ($(numfmt --to=iec --suffix=B "$want" 2>/dev/null || echo "$want B"))"
    curl -fLsS --retry 3 -o "$out" "$RAW/$name"
    actual=$(stat -c%s "$out")
    if [ "$actual" != "$want" ]; then
        echo "FATAL: $name downloaded $actual bytes, expected $want"
        rm -f "$out"
        exit 1
    fi
done

echo
echo "rime-ice dicts ready (pinned to ${RIME_ICE_COMMIT:0:10}):"
ls -la "$DICT_SRC"
