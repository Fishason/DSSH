#include "font_atlas.h"

static int bsearch_u32(const uint32_t *arr, int len, uint32_t cp) {
    int lo = 0, hi = len - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (arr[mid] == cp) return mid;
        if (arr[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int font_glyph_index(uint32_t cp) {
    if (cp >= 0x20 && cp < 0x7F)
        return (int)(cp - 0x20);
    /* 特殊空白文字 → 通常スペース */
    if (cp == 0x00A0 || cp == 0x202F || cp == 0x2007 || cp == 0x2009 ||
        cp == 0x200B || cp == 0xFEFF)
        return (int)(0x20 - 0x20);
    int i = bsearch_u32(font_unicode_cps, FONT_UNICODE_MAP_LEN, cp);
    if (i >= 0) return font_unicode_idx[i];

    /* fallback */
    if (cp >= 0x2500 && cp <= 0x257F)
        return font_glyph_index((cp & 1) ? 0x2502 : 0x2500);
    if (cp >= 0x2580 && cp <= 0x259F)
        return font_glyph_index(0x2588);
    return font_glyph_index('?');
}

int font_wide_glyph_index(uint32_t cp) {
    int i = bsearch_u32(font_wide_cps, FONT_WIDE_MAP_LEN, cp);
    if (i >= 0) return font_wide_idx[i];
    return -1;
}

int font_is_wide(uint32_t cp) {
    /* ひらがな・カタカナ・CJK統合漢字・全角 */
    if (cp >= 0x3000 && cp <= 0x9FFF) return 1;
    if (cp >= 0xAC00 && cp <= 0xD7FF) return 1;  /* ハングル */
    if (cp >= 0xFF01 && cp <= 0xFF60) return 1;  /* 全角ASCII */
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return 1;
    return 0;
}
