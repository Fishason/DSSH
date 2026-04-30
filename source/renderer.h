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

/* Filled rect on bottom screen, cell-aligned. */
void renderer_draw_rect_cells(renderer_t *r, int x_cells, int y_cells,
                              int w_cells, int h_cells, uint32_t rgba);
