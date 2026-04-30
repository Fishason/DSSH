#pragma once
#include <stdint.h>

#define TERM_MAX_COLS   80
#define TERM_MAX_ROWS   30
/* Scrollback rows kept in memory.  Kept small (500) on purpose — for
 * a "native SSH" experience the real history lives on the server side
 * (tmux's `set -g history-limit` typically 2000–50000).  This client
 * scrollback only matters when the user is NOT in tmux and pipes
 * something long like `cat largefile`; for those cases 500 rows is
 * plenty.  At 80 cols × 12 bytes/cell that's ~480 KB. */
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

    /* xterm mouse-tracking state.  Set by parser when server emits
     *   ESC[?1000h / ?1002h / ?1003h    (any mouse tracking enabled)
     *   ESC[?1006h                       (SGR encoding, recommended by tmux)
     * and cleared by the corresponding `l` (low) variants.  When both
     * mouse_proto > 0 *and* mouse_sgr is set, the client should send
     *   ESC[<button;col;rowM
     * for every press (and "m" for release) instead of acting locally on
     * scrolls / clicks.  This is how PC terminals make tmux's mouse
     * support transparent. */
    int mouse_proto;   /* 0 = off; otherwise 1000/1002/1003 (last set wins) */
    int mouse_sgr;     /* ESC[?1006h: encode as SGR (\x1b[<...M) */
} terminal_t;

/* Convenience: true when the server has enabled any mouse tracking mode. */
static inline int terminal_mouse_enabled(const terminal_t *t) {
    return t && t->mouse_proto != 0;
}

terminal_t *terminal_init(int cols, int rows);
void        terminal_free(terminal_t *term);
void        terminal_write(terminal_t *term, const char *data);
void        terminal_write_n(terminal_t *term, const char *data, int len);
void        terminal_reset(terminal_t *term);
void        terminal_scroll_view(terminal_t *term, int delta);
term_cell_t terminal_get_cell(terminal_t *term, int x, int y);
