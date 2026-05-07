#include "font_atlas.h"

static int bsearch_u32(const uint32_t *arr, int len, uint32_t cp) {
  int lo = 0, hi = len - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (arr[mid] == cp)
      return mid;
    if (arr[mid] < cp)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return -1;
}

/* Map "char missing from font" to a visually similar char that does exist.
 * Returns 0 if no good substitute (caller falls back to '?').  This lets
 * us display ⏵/⏸/⏯ etc. even when neither Terminus nor DejaVu carries
 * those exact glyphs — by aliasing to ▶/‖/■ which are basic geometric
 * shapes both fonts cover.
 *
 * Extended in M11 polish to cover the colour-emoji codepoints that
 * Lazy.nvim, Mason, lspconfig, and various plugin status indicators
 * emit (✅❌⚠️🚀🟢🔴🔵 etc.).  Bitmap fonts can't carry those, so we
 * substitute the visually closest single-char Unicode dingbat that
 * Terminus / DejaVu actually have. */
static uint32_t fallback_alias(uint32_t cp) {
  switch (cp) {
  /* ── Misc Technical play/pause cluster ────────────────────────── */
  case 0x23F4:
    return 0x25C0; /* ⏴ → ◀ */
  case 0x23F5:
    return 0x25B6; /* ⏵ → ▶ */
  case 0x23F6:
    return 0x25B2; /* ⏶ → ▲ */
  case 0x23F7:
    return 0x25BC; /* ⏷ → ▼ */
  case 0x23F8:
    return 0x2016; /* ⏸ pause → ‖ */
  case 0x23F9:
    return 0x25A0; /* ⏹ stop  → ■ */
  case 0x23FA:
    return 0x25CF; /* ⏺ record→ ● */
  case 0x23EF:
    return 0x25B6; /* ⏯ play/pause → ▶ */
  case 0x23ED:
    return 0x25B6; /* ⏭ next → ▶ */
  case 0x23EE:
    return 0x25C0; /* ⏮ prev → ◀ */

  /* ── Colour-emoji status markers (Lazy.nvim, Mason, etc.) ─────── */
  case 0x2705:
    return 0x2714; /* ✅ heavy white check → ✔ heavy check mark */
  case 0x274C:
    return 0x2716; /* ❌ cross mark → ✖ heavy multiplication x */
  case 0x274E:
    return 0x2716; /* ❎ cross mark button → ✖ */
  case 0x26D4:
    return 0x2718; /* ⛔ no-entry → ✘ */
  case 0x26A0:
    return 0x25B3; /* ⚠ warning → △ */
  case 0x26A1:
    return 0x21AF; /* ⚡ high voltage → ↯ */
  case 0x2728:
    return 0x2605; /* ✨ sparkles → ★ */
  case 0x1F534:    /* 🔴 large red circle */
  case 0x1F535:    /* 🔵 large blue circle */
  case 0x1F7E0:    /* 🟠 orange circle */
  case 0x1F7E1:    /* 🟡 yellow circle */
  case 0x1F7E2:    /* 🟢 green circle */
  case 0x1F7E3:    /* 🟣 purple circle */
  case 0x1F7E4:    /* 🟤 brown circle */
    return 0x25CF; /* → ● black circle */
  case 0x26AA:
    return 0x25CB; /* ⚪ medium-white circle → ○ */
  case 0x26AB:
    return 0x25CF; /* ⚫ medium-black circle → ● */
  case 0x1F7E5:    /* 🟥 large red square */
  case 0x1F7E6:    /* 🟦 large blue square */
  case 0x1F7E7:    /* 🟧 orange square */
  case 0x1F7E8:    /* 🟨 yellow square */
  case 0x1F7E9:    /* 🟩 green square */
  case 0x1F7EA:    /* 🟪 purple square */
  case 0x1F7EB:    /* 🟫 brown square */
    return 0x25A0; /* → ■ black square */
  case 0x2B1B:
    return 0x25A0; /* ⬛ black large square → ■ */
  case 0x2B1C:
    return 0x25A1; /* ⬜ white large square → □ */
  case 0x1F4A1:
    return 0x263C; /* 💡 light bulb → ☼ white sun with rays */
  case 0x1F680:
    return 0x2197; /* 🚀 rocket → ↗ NE arrow */
  case 0x1F4E6:
    return 0x25A3; /* 📦 package → ▣ inset square */
  case 0x1F527:
    return 0x2699; /* 🔧 wrench → ⚙ gear */
  case 0x1F4DD:
    return 0x270D; /* 📝 memo → ✍ writing hand */
  case 0x1F511:
    return 0x21B3; /* 🔑 key → ↳ (placeholder) */
  case 0x1F4DA:
    return 0x2261; /* 📚 books → ≡ */
  case 0x1F50D:
    return 0x2315; /* 🔍 magnifying glass → ⌕ */
  case 0x1F50E:
    return 0x2315; /* 🔎 → ⌕ */
  case 0x1F4CC:
    return 0x2691; /* 📌 pushpin → ⚑ */
  case 0x1F4CD:
    return 0x2691; /* 📍 round pushpin → ⚑ */
  case 0x231B:
    return 0x29D6; /* ⌛ hourglass → ⧖ */
  case 0x23F3:
    return 0x29D6; /* ⏳ hourglass with sand → ⧖ */
  case 0x1F4C1:    /* 📁 file folder */
  case 0x1F4C2:    /* 📂 open file folder */
    return 0xF115; /* → NF folder-open-o (always present) */
  case 0x1F4C4:
    return 0xF15B; /* 📄 page facing up → NF file */
  case 0x1F389:
    return 0x2605; /* 🎉 party popper → ★ */

  default:
    return 0;
  }
}

int font_glyph_index(uint32_t cp) {
  if (cp >= 0x20 && cp < 0x7F)
    return (int)(cp - 0x20);
  /* Special whitespace -> plain space. */
  if (cp == 0x00A0 || cp == 0x202F || cp == 0x2007 || cp == 0x2009 ||
      cp == 0x200B || cp == 0xFEFF)
    return (int)(0x20 - 0x20);
  int i = bsearch_u32(font_unicode_cps, FONT_UNICODE_MAP_LEN, cp);
  if (i >= 0)
    return font_unicode_idx[i];

  /* Try a known visual alias before giving up. */
  uint32_t alias = fallback_alias(cp);
  if (alias && alias != cp) {
    int j = bsearch_u32(font_unicode_cps, FONT_UNICODE_MAP_LEN, alias);
    if (j >= 0)
      return font_unicode_idx[j];
  }

  /* Fallback by range. */
  if (cp >= 0x2500 && cp <= 0x257F)
    return font_glyph_index((cp & 1) ? 0x2502 : 0x2500);
  if (cp >= 0x2580 && cp <= 0x259F)
    return font_glyph_index(0x2588);
  return font_glyph_index('?');
}

int font_wide_glyph_index(uint32_t cp) {
  int i = bsearch_u32(font_wide_cps, FONT_WIDE_MAP_LEN, cp);
  if (i >= 0)
    return font_wide_idx[i];
  return -1;
}

int font_is_wide(uint32_t cp) {
  /* ひらがな・カタカナ・CJK統合漢字・全角 */
  if (cp >= 0x3000 && cp <= 0x9FFF)
    return 1;
  if (cp >= 0xAC00 && cp <= 0xD7FF)
    return 1; /* ハングル */
  if (cp >= 0xFF01 && cp <= 0xFF60)
    return 1; /* 全角ASCII */
  if (cp >= 0xFFE0 && cp <= 0xFFE6)
    return 1;
  return 0;
}
