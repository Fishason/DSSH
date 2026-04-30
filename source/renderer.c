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

static u32 rgba_to_c2d(uint32_t rgba) {
    return C2D_Color32((rgba >> 24) & 0xff,
                       (rgba >> 16) & 0xff,
                       (rgba >>  8) & 0xff,
                        rgba        & 0xff);
}

static void draw_glyph(float fx, float fy, float z,
                       int glyph_idx, u32 color) {
    if (glyph_idx < 0 || glyph_idx >= FONT_NGLYPHS) return;
    const uint8_t *rows = font_glyphs[glyph_idx];
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
                draw_bg = (c.bg != 0x1e1e2eff);
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

void renderer_draw_text(renderer_t *r, int x_cells, int y_cells,
                        const char *text, uint32_t rgba) {
    (void)r;
    if (!text) return;
    u32 color = rgba_to_c2d(rgba);
    float fx = x_cells * FONT_CELL_W;
    float fy = y_cells * FONT_CELL_H;
    for (const char *s = text; *s; s++) {
        unsigned char c = (unsigned char)*s;
        draw_glyph(fx, fy, 0.5f, font_glyph_index(c), color);
        fx += FONT_CELL_W;
    }
}

void renderer_draw_rect_cells(renderer_t *r, int x_cells, int y_cells,
                              int w_cells, int h_cells, uint32_t rgba) {
    (void)r;
    C2D_DrawRectSolid(x_cells * FONT_CELL_W, y_cells * FONT_CELL_H, 0.4f,
                      w_cells * FONT_CELL_W, h_cells * FONT_CELL_H,
                      rgba_to_c2d(rgba));
}
