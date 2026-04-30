#pragma once
#include <stdint.h>

#define TERM_MAX_COLS   80
#define TERM_MAX_ROWS   30
#define TERM_SCROLLBACK 500

typedef struct {
    uint32_t codepoint;
    uint32_t fg, bg;
    uint8_t  flags;
} term_cell_t;

#define CELL_FLAG_BOLD      (1<<0)
#define CELL_FLAG_UNDERLINE (1<<1)
#define CELL_FLAG_REVERSE   (1<<2)
#define CELL_FLAG_BLINK     (1<<3)
#define CELL_FLAG_WIDE      (1<<4)  /* 全角文字の先頭セル */
#define CELL_FLAG_WIDE_CONT (1<<5)  /* 全角文字の後続セル（描画スキップ） */

typedef struct terminal_t {
    int cols, rows;
    int cur_x, cur_y;
    int scroll_top, scroll_bottom;

    term_cell_t *cells;      // active screen
    term_cell_t *alt_cells;  // alternate screen (?1047/?1049)
    int use_alt;

    /* saved cursor (main screen) */
    int      saved_x, saved_y;
    uint32_t saved_fg, saved_bg;
    uint8_t  saved_flags;

    /* scrollback */
    term_cell_t *scrollback;
    int sb_head, sb_size, sb_offset;

    /* current attributes */
    uint32_t cur_fg, cur_bg;
    uint8_t  cur_flags;

    /* escape sequence parser */
    int  parse_state;
    char parse_buf[512];   /* 512 — Claude Code sends long OSC/SGR sequences */
    int  parse_len;

    int cursor_visible;
    int cursor_blink_count;
} terminal_t;

terminal_t *terminal_init(int cols, int rows);
void        terminal_free(terminal_t *term);
void        terminal_write(terminal_t *term, const char *data);
void        terminal_write_n(terminal_t *term, const char *data, int len);
void        terminal_reset(terminal_t *term);
void        terminal_scroll_view(terminal_t *term, int delta);
term_cell_t terminal_get_cell(terminal_t *term, int x, int y);
