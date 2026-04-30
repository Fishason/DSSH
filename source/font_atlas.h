#pragma once
#include <stdint.h>

/* gen_font.py で生成された font_data.c の定数 */
extern const int      FONT_NGLYPHS;
extern const int      FONT_WIDE_NGLYPHS;
extern const int      FA_CELL_W;
extern const int      FA_CELL_H;

/* 半角グリフ (uint8_t: CELL_W=6bit/row) */
extern const uint8_t  font_glyphs[][12];
extern const int      FONT_UNICODE_MAP_LEN;
extern const uint32_t font_unicode_cps[];
extern const uint16_t font_unicode_idx[];

/* 全角グリフ (uint16_t: CELL_W*2=12bit/row) */
extern const uint16_t font_wide_glyphs[][12];
extern const int      FONT_WIDE_MAP_LEN;
extern const uint32_t font_wide_cps[];
extern const uint16_t font_wide_idx[];

/* codepoint → glyph index */
int font_glyph_index(uint32_t codepoint);       /* 半角: not-found → '?' */
int font_wide_glyph_index(uint32_t codepoint);  /* 全角: not-found → -1  */
int font_is_wide(uint32_t codepoint);           /* 全角文字か？ */
