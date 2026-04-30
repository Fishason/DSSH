#pragma once
#include <citro2d.h>
#include "terminal.h"
#include "font_atlas.h"

/* Bitmap font cell size — must match gen_font.py */
#define FONT_CELL_W  6
#define FONT_CELL_H  12

/* Top screen is 400x240, 50 cols default; we use 66x20 for tighter packing */
#define R_TOP_COLS  (400 / FONT_CELL_W)   /* 66 */
#define R_TOP_ROWS  (240 / FONT_CELL_H)   /* 20 */
#define R_BOT_COLS  (320 / FONT_CELL_W)   /* 53 */
#define R_BOT_ROWS  (240 / FONT_CELL_H)   /* 20 */

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
