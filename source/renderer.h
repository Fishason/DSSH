#pragma once
#include <citro2d.h>
#include "terminal.h"
#include "font_atlas.h"

/* Cell size lives in font_atlas.h (FA_CELL_W / FA_CELL_H) since it's a
 * compile-time invariant tied to the bitmap atlas.  Re-export the names
 * the existing renderer code uses. */
#define FONT_CELL_W FA_CELL_W
#define FONT_CELL_H FA_CELL_H

#define R_TOP_COLS  (400 / FONT_CELL_W)   /* 66 at 6×10 */
#define R_TOP_ROWS  (240 / FONT_CELL_H)   /* 24 at 6×10 */
#define R_BOT_COLS  (320 / FONT_CELL_W)
#define R_BOT_ROWS  (240 / FONT_CELL_H)

typedef struct renderer_t {
    C3D_RenderTarget *top;
    C3D_RenderTarget *bot;
    int top_cols, top_rows;
} renderer_t;

renderer_t *renderer_init(C3D_RenderTarget *top, C3D_RenderTarget *bot);
void        renderer_free(renderer_t *r);
void        renderer_draw_terminal(renderer_t *r, terminal_t *term);

/* Bottom-screen status panel (M3 helper). Draws a single line of text at
 * (x_cells, y_cells) on the bottom render target. Must be called inside
 * C2D_SceneBegin(bot). Color is RGBA 0xRRGGBBAA. */
void renderer_draw_text(renderer_t *r, int x_cells, int y_cells,
                        const char *text, uint32_t rgba);

/* Pixel-precise version — draws each glyph at exact pixel coords.  Use
 * this when cell-grid rounding would mis-center labels (e.g. a 32 px
 * key cannot be exactly centered on a 6 px cell grid). */
void renderer_draw_text_px(int px, int py, const char *text, uint32_t rgba);

/* Same as renderer_draw_text_px but each glyph pixel is rendered as a
 * scale×scale block.  Used for the iOS-style key-press popup bubble
 * which shows the tapped character at 2× normal size. */
void renderer_draw_text_px_scaled(int px, int py, const char *text,
                                  uint32_t rgba, int scale);

/* Pixel width that renderer_draw_text_px would consume for the given
 * UTF-8 string.  Each ASCII codepoint contributes FONT_CELL_W pixels;
 * CJK / fullwidth codepoints contribute 2*FONT_CELL_W.  Used by the
 * IME candidate strip layout to fit-test before drawing. */
int  renderer_utf8_text_width_px(const char *text);

/* Filled rect on bottom screen, cell-aligned. */
void renderer_draw_rect_cells(renderer_t *r, int x_cells, int y_cells,
                              int w_cells, int h_cells, uint32_t rgba);
