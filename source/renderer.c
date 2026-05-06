#include "renderer.h"
#include "font_atlas.h"
#include <stdlib.h>
#include <string.h>

/*
 * citro2d-based terminal renderer (forked from skmtrd/3dssh, with the touch
 * keyboard portions removed — those will live in M4's softkb.c).
 *
 * Two-pass terminal draw:
 *   pass 1  background rects per cell + cursor
 *   pass 2  glyph bitmaps blitted as 1-pixel-tall horizontal runs of
 *           C2D_DrawRectSolid; no texture atlas, just stamping pixels.
 *
 * Why pixel runs and not a texture: avoids the cost/complexity of uploading
 * texture atlases at startup; ARM11 GPU can handle thousands of solid rects
 * per frame easily and we're drawing ~1320 cells × ~10 lit pixels avg per
 * glyph at 60fps. Plenty of headroom.
 */

/* Raw passthrough — neovim sends exact 24-bit RGB via SGR-truecolor;
 * trust those colours and let the LCD render them as-is.  An earlier
 * "punch" gain was found to wash dark blues into greys and lift bright
 * blues toward white, defeating the user's colourscheme. */
static u32 rgba_to_c2d(uint32_t rgba) {
    return C2D_Color32((rgba >> 24) & 0xff,
                       (rgba >> 16) & 0xff,
                       (rgba >>  8) & 0xff,
                        rgba        & 0xff);
}

/* Walk a 1bpp layer (FA_CELL_H bytes) and paint contiguous lit runs as
 * solid 1×1-row rects in the given C2D-format colour.  Factored out so
 * draw_glyph and draw_glyph_scaled can share the inner loop and the AA
 * variant can call it three times with different alpha. */
static inline void draw_layer_runs(float fx, float fy, float z,
                                   const uint8_t *rows, u32 color) {
    for (int row = 0; row < FA_CELL_H; row++) {
        uint8_t bits = rows[row];
        if (!bits) continue;
        int col = 0;
        while (col < FA_CELL_W) {
            if (bits & (1 << (FA_CELL_W - 1 - col))) {
                int start = col;
                while (col < FA_CELL_W && (bits & (1 << (FA_CELL_W - 1 - col))))
                    col++;
                C2D_DrawRectSolid(fx + start, fy + row, z, col - start, 1, color);
            } else col++;
        }
    }
}

/* Three-pass anti-aliased glyph rendering.  Layer 0 is full-alpha
 * (matches the `color` argument); layer 1 is half alpha; layer 2 is
 * quarter alpha.  For pure bitmap fonts (Terminus, Zpix) only layer 0
 * has set bits, so this is identical-cost to the old 1bpp path.  For
 * TTF text fonts the edge pixels populate layers 1 and 2, smoothing
 * stroke edges via citro2d's natural alpha blending against whatever
 * background colour is below. */
static void draw_glyph(float fx, float fy, float z,
                       int glyph_idx, u32 color) {
    if (glyph_idx < 0 || glyph_idx >= FONT_NGLYPHS) return;
    const uint8_t *base = font_glyphs[glyph_idx];

    draw_layer_runs(fx, fy, z, base, color);

    /* Decompose the C2D colour back to channels so we can scale the
     * alpha for the half/quarter layers.  C2D_Color32 packs as
     * (a<<24)|(b<<16)|(g<<8)|r — the byte order is fixed by the macro
     * and matches the GPU's expected format. */
    u8 a = (color >> 24) & 0xff;
    if (a < 2) return;  /* fully transparent already; AA layers add nothing */
    u8 b = (color >> 16) & 0xff;
    u8 g = (color >>  8) & 0xff;
    u8 r =  color        & 0xff;

    u32 c_half    = C2D_Color32(r, g, b, a / 2);
    u32 c_quarter = C2D_Color32(r, g, b, a / 4);
    draw_layer_runs(fx, fy, z, base + FA_CELL_H,         c_half);
    draw_layer_runs(fx, fy, z, base + (FA_CELL_H * 2),   c_quarter);
}

static void draw_wide_glyph(float fx, float fy, float z,
                            int glyph_idx, u32 color) {
    if (glyph_idx < 0 || glyph_idx >= FONT_WIDE_NGLYPHS) return;
    const uint16_t *rows = font_wide_glyphs[glyph_idx];
    int wide = FA_CELL_W * 2;
    for (int row = 0; row < FA_CELL_H; row++) {
        uint16_t bits = rows[row];
        if (!bits) continue;
        int col = 0;
        while (col < wide) {
            if (bits & (1 << (wide - 1 - col))) {
                int start = col;
                while (col < wide && (bits & (1 << (wide - 1 - col))))
                    col++;
                C2D_DrawRectSolid(fx + start, fy + row, z, col - start, 1, color);
            } else col++;
        }
    }
}

renderer_t *renderer_init(C3D_RenderTarget *top, C3D_RenderTarget *bot) {
    renderer_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->top = top;
    r->bot = bot;
    r->top_cols = R_TOP_COLS;
    r->top_rows = R_TOP_ROWS;
    return r;
}

void renderer_free(renderer_t *r) { free(r); }

/* Cursor style: invert fg/bg of the cell at term->cur_x/cur_y.  This is
 * the xterm/iTerm/alacritty default — fully opaque, never leaves
 * a "ghost" trail when cursor moves (since the next frame rebuilds
 * the entire screen from cell state, the inversion only happens at
 * the new cursor position).  Far more reliable than alpha-blended
 * overlay blocks at 60 fps with double-buffering. */
static inline int cell_is_cursor(const terminal_t *t, int x, int y) {
    return t->cursor_visible && x == t->cur_x && y == t->cur_y;
}

void renderer_draw_terminal(renderer_t *r, terminal_t *term) {
    if (!r || !term) return;

    int cols = (term->cols < r->top_cols) ? term->cols : r->top_cols;
    int rows = (term->rows < r->top_rows) ? term->rows : r->top_rows;
    float cw = FONT_CELL_W, ch = FONT_CELL_H;

    /* pass 1: backgrounds (with cursor cell painted using fg color) */
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            term_cell_t c = terminal_get_cell(term, x, y);
            float fx = x * cw, fy = y * ch;
            uint32_t bg_rgba;
            int draw_bg;
            if (cell_is_cursor(term, x, y)) {
                /* Inverted cursor: cell bg becomes fg color. */
                bg_rgba = c.fg;
                draw_bg = 1;
            } else {
                bg_rgba = c.bg;
                /* Skip drawing the default bg cell — the canvas clear
                 * paints that colour for "free".  Hex literal must
                 * track DEFAULT_BG in terminal.c. */
                draw_bg = (c.bg != 0x1a1b26ff);
            }
            if (draw_bg)
                C2D_DrawRectSolid(fx, fy, 0.1f, cw, ch, rgba_to_c2d(bg_rgba));
        }
    }

    /* pass 2: glyphs (cursor cell glyph drawn in bg color) */
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            term_cell_t c = terminal_get_cell(term, x, y);
            if (c.flags & CELL_FLAG_WIDE_CONT) continue;
            if (c.codepoint <= 0x20) continue;
            u32 fg = cell_is_cursor(term, x, y)
                   ? rgba_to_c2d(c.bg)   /* invert: glyph uses cell's bg */
                   : rgba_to_c2d(c.fg);
            if (c.flags & CELL_FLAG_WIDE) {
                int gi = font_wide_glyph_index(c.codepoint);
                if (gi >= 0)
                    draw_wide_glyph(x * cw, y * ch, 0.3f, gi, fg);
                else
                    draw_glyph(x * cw, y * ch, 0.3f,
                               font_glyph_index(c.codepoint), fg);
            } else {
                draw_glyph(x * cw, y * ch, 0.3f,
                           font_glyph_index(c.codepoint), fg);
            }
        }
    }
}

/* Pull one UTF-8 codepoint from *s, advance *s past it.  Bytes that
 * don't form a valid sequence yield U+FFFD and advance by 1 so we
 * never loop forever.  Used only by these label helpers — the SSH
 * terminal path has its own decoder in terminal.c. */
static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned char b0 = *p;
    uint32_t cp;
    int extra;
    if (b0 < 0x80)      { cp = b0;          extra = 0; }
    else if (b0 < 0xC0) { *s = (const char *)(p + 1); return 0xFFFD; }
    else if (b0 < 0xE0) { cp = b0 & 0x1F;   extra = 1; }
    else if (b0 < 0xF0) { cp = b0 & 0x0F;   extra = 2; }
    else if (b0 < 0xF8) { cp = b0 & 0x07;   extra = 3; }
    else                { *s = (const char *)(p + 1); return 0xFFFD; }
    p++;
    for (int i = 0; i < extra; i++) {
        if ((*p & 0xC0) != 0x80) {
            /* truncated/invalid — stop here, emit replacement. */
            *s = (const char *)p;
            return 0xFFFD;
        }
        cp = (cp << 6) | (*p & 0x3F);
        p++;
    }
    *s = (const char *)p;
    return cp;
}

/* Dispatches one codepoint to either the narrow or wide blit path
 * and returns the pixel advance.  Used by every label helper so CJK
 * candidates (M7's IME bar) render with the right cell width. */
static int draw_codepoint_px(float fx, float fy, float z,
                             uint32_t cp, u32 color) {
    if (font_is_wide(cp)) {
        int gi = font_wide_glyph_index(cp);
        if (gi >= 0) {
            draw_wide_glyph(fx, fy, z, gi, color);
        } else {
            /* Wide font lookup miss — fall back to the narrow notdef
             * so the slot isn't visually empty. */
            draw_glyph(fx, fy, z, font_glyph_index(cp), color);
        }
        return FONT_CELL_W * 2;
    }
    draw_glyph(fx, fy, z, font_glyph_index(cp), color);
    return FONT_CELL_W;
}

void renderer_draw_text(renderer_t *r, int x_cells, int y_cells,
                        const char *text, uint32_t rgba) {
    (void)r;
    if (!text) return;
    u32 color = rgba_to_c2d(rgba);
    float fx = x_cells * FONT_CELL_W;
    float fy = y_cells * FONT_CELL_H;
    const char *s = text;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        fx += (float)draw_codepoint_px(fx, fy, 0.5f, cp, color);
    }
}

void renderer_draw_text_px(int px, int py, const char *text, uint32_t rgba) {
    if (!text) return;
    u32 color = rgba_to_c2d(rgba);
    float fx = (float)px;
    float fy = (float)py;
    const char *s = text;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        fx += (float)draw_codepoint_px(fx, fy, 0.5f, cp, color);
    }
}

static inline void draw_layer_runs_scaled(float fx, float fy, float z,
                                          const uint8_t *rows, u32 color,
                                          int scale) {
    for (int row = 0; row < FA_CELL_H; row++) {
        uint8_t bits = rows[row];
        if (!bits) continue;
        int col = 0;
        while (col < FA_CELL_W) {
            if (bits & (1 << (FA_CELL_W - 1 - col))) {
                int start = col;
                while (col < FA_CELL_W && (bits & (1 << (FA_CELL_W - 1 - col))))
                    col++;
                C2D_DrawRectSolid(fx + start * scale, fy + row * scale, z,
                                  (col - start) * scale, scale, color);
            } else col++;
        }
    }
}

/* Scaled glyph blit: same 3-layer AA as draw_glyph but each lit run
 * becomes a (run × scale) × scale rect.  Drawn at z=0.85 so the popup
 * bubble's label sits above the key but below any later overlay. */
static void draw_glyph_scaled(float fx, float fy, float z,
                              int glyph_idx, u32 color, int scale) {
    if (glyph_idx < 0 || glyph_idx >= FONT_NGLYPHS) return;
    const uint8_t *base = font_glyphs[glyph_idx];
    draw_layer_runs_scaled(fx, fy, z, base, color, scale);

    u8 a = (color >> 24) & 0xff;
    if (a < 2) return;
    u8 b = (color >> 16) & 0xff;
    u8 g = (color >>  8) & 0xff;
    u8 r =  color        & 0xff;
    u32 c_half    = C2D_Color32(r, g, b, a / 2);
    u32 c_quarter = C2D_Color32(r, g, b, a / 4);
    draw_layer_runs_scaled(fx, fy, z, base + FA_CELL_H,       c_half,    scale);
    draw_layer_runs_scaled(fx, fy, z, base + FA_CELL_H * 2,   c_quarter, scale);
}

int renderer_utf8_text_width_px(const char *text) {
    if (!text) return 0;
    int w = 0;
    const char *s = text;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        w += font_is_wide(cp) ? FONT_CELL_W * 2 : FONT_CELL_W;
    }
    return w;
}

void renderer_draw_text_px_scaled(int px, int py, const char *text,
                                  uint32_t rgba, int scale) {
    if (!text || scale <= 0) return;
    u32 color = rgba_to_c2d(rgba);
    float fx = (float)px;
    float fy = (float)py;
    int step = FONT_CELL_W * scale;
    const char *s = text;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        draw_glyph_scaled(fx, fy, 0.85f, font_glyph_index(cp), color, scale);
        fx += step;
    }
}

void renderer_draw_rect_cells(renderer_t *r, int x_cells, int y_cells,
                              int w_cells, int h_cells, uint32_t rgba) {
    (void)r;
    C2D_DrawRectSolid(x_cells * FONT_CELL_W, y_cells * FONT_CELL_H, 0.4f,
                      w_cells * FONT_CELL_W, h_cells * FONT_CELL_H,
                      rgba_to_c2d(rgba));
}
