#include "terminal.h"
#include "font_atlas.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DEFAULT_FG 0xcdd6f4ff
#define DEFAULT_BG 0x1e1e2eff

#define STATE_NORMAL 0
#define STATE_ESC    1
#define STATE_CSI    2
#define STATE_OSC    3

static void cell_reset(term_cell_t *c) {
    c->codepoint = ' ';
    c->fg = DEFAULT_FG;
    c->bg = DEFAULT_BG;
    c->flags = 0;
}

terminal_t *terminal_init(int cols, int rows) {
    terminal_t *t = calloc(1, sizeof(terminal_t));
    if (!t) return NULL;
    t->cols = cols; t->rows = rows;
    t->scroll_top = 0; t->scroll_bottom = rows - 1;
    t->cursor_visible = 1;
    t->cur_fg = DEFAULT_FG; t->cur_bg = DEFAULT_BG;
    t->parse_state = STATE_NORMAL;
    t->cells = calloc(cols * rows, sizeof(term_cell_t));
    if (!t->cells) { free(t); return NULL; }
    t->scrollback = calloc(cols * TERM_SCROLLBACK, sizeof(term_cell_t));
    if (!t->scrollback) { free(t->cells); free(t); return NULL; }
    for (int i = 0; i < cols * rows; i++) cell_reset(&t->cells[i]);
    return t;
}

void terminal_free(terminal_t *t) {
    if (!t) return;
    free(t->cells);
    free(t->alt_cells);
    free(t->scrollback);
    free(t);
}

void terminal_reset(terminal_t *t) {
    t->cur_x = 0; t->cur_y = 0;
    t->cur_fg = DEFAULT_FG; t->cur_bg = DEFAULT_BG; t->cur_flags = 0;
    t->scroll_top = 0; t->scroll_bottom = t->rows - 1;
    t->cursor_visible = 1;
    for (int i = 0; i < t->cols * t->rows; i++) cell_reset(&t->cells[i]);
}

/* ── スクロール ── */
static void scroll_up_one(terminal_t *t) {
    int top = t->scroll_top, bot = t->scroll_bottom;
    if (top == 0) {
        int dst = t->sb_head * t->cols;
        memcpy(&t->scrollback[dst], &t->cells[0], t->cols * sizeof(term_cell_t));
        t->sb_head = (t->sb_head + 1) % TERM_SCROLLBACK;
        if (t->sb_size < TERM_SCROLLBACK) t->sb_size++;
    }
    memmove(&t->cells[top * t->cols], &t->cells[(top+1) * t->cols],
            (bot - top) * t->cols * sizeof(term_cell_t));
    for (int x = 0; x < t->cols; x++) cell_reset(&t->cells[bot * t->cols + x]);
}

static void scroll_down_one(terminal_t *t) {
    int top = t->scroll_top, bot = t->scroll_bottom;
    memmove(&t->cells[(top+1) * t->cols], &t->cells[top * t->cols],
            (bot - top) * t->cols * sizeof(term_cell_t));
    for (int x = 0; x < t->cols; x++) cell_reset(&t->cells[top * t->cols + x]);
}

static void newline(terminal_t *t) {
    if (t->cur_y >= t->scroll_bottom) scroll_up_one(t);
    else t->cur_y++;
}

static void put_char(terminal_t *t, uint32_t cp) {
    int wide = font_is_wide(cp);
    /* 全角で残り1列しかない場合はスペースで埋めて折り返す */
    if (wide && t->cur_x >= t->cols - 1) { t->cur_x = 0; newline(t); }
    if (!wide && t->cur_x >= t->cols)    { t->cur_x = 0; newline(t); }

    uint32_t fg = (t->cur_flags & CELL_FLAG_REVERSE) ? t->cur_bg : t->cur_fg;
    uint32_t bg = (t->cur_flags & CELL_FLAG_REVERSE) ? t->cur_fg : t->cur_bg;

    term_cell_t *c = &t->cells[t->cur_y * t->cols + t->cur_x];
    c->codepoint = cp;
    c->fg = fg; c->bg = bg;
    c->flags = t->cur_flags | (wide ? CELL_FLAG_WIDE : 0);
    t->cur_x++;

    if (wide) {
        /* 後続セルを WIDE_CONT でマーク */
        term_cell_t *c2 = &t->cells[t->cur_y * t->cols + t->cur_x];
        c2->codepoint = 0;
        c2->fg = fg; c2->bg = bg;
        c2->flags = CELL_FLAG_WIDE_CONT;
        t->cur_x++;
    }
}

/* ── 256色変換 ── */
static uint32_t ansi_256_to_rgba(int idx) {
    static const uint32_t ansi16[16] = {
        0x1e1e2eff, 0xf38ba8ff, 0xa6e3a1ff, 0xf9e2afff,
        0x89b4faff, 0xcba6f7ff, 0x89dcebff, 0xcdd6f4ff,
        0x585b70ff, 0xf38ba8ff, 0xa6e3a1ff, 0xf9e2afff,
        0x89b4faff, 0xcba6f7ff, 0x89dcebff, 0xcdd6f4ff,
    };
    if (idx < 16) return ansi16[idx];
    if (idx >= 232) { uint8_t v = 8 + (idx-232)*10; return (v<<24)|(v<<16)|(v<<8)|0xff; }
    idx -= 16;
    uint8_t b=(idx%6)*51, g=((idx/6)%6)*51, r=(idx/36)*51;
    return (r<<24)|(g<<16)|(b<<8)|0xff;
}

/* ── SGR ── */
static void handle_sgr(terminal_t *t, int *params, int nparams) {
    for (int i = 0; i < nparams; ) {
        int p = params[i];
        switch (p) {
            case 0:  t->cur_fg=DEFAULT_FG; t->cur_bg=DEFAULT_BG; t->cur_flags=0; break;
            case 1:  t->cur_flags |=  CELL_FLAG_BOLD;      break;
            case 2:  break; // dim - ignore
            case 3:  break; // italic - ignore
            case 4:  t->cur_flags |=  CELL_FLAG_UNDERLINE; break;
            case 5:  t->cur_flags |=  CELL_FLAG_BLINK;     break;
            case 7:  t->cur_flags |=  CELL_FLAG_REVERSE;   break;
            case 21: case 22: t->cur_flags &= ~CELL_FLAG_BOLD;      break;
            case 24: t->cur_flags &= ~CELL_FLAG_UNDERLINE; break;
            case 25: t->cur_flags &= ~CELL_FLAG_BLINK;     break;
            case 27: t->cur_flags &= ~CELL_FLAG_REVERSE;   break;
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                t->cur_fg = ansi_256_to_rgba(p-30); break;
            case 39: t->cur_fg = DEFAULT_FG; break;
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                t->cur_bg = ansi_256_to_rgba(p-40); break;
            case 49: t->cur_bg = DEFAULT_BG; break;
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                t->cur_fg = ansi_256_to_rgba(p-90+8); break;
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                t->cur_bg = ansi_256_to_rgba(p-100+8); break;
            case 38:
                if (i+1 < nparams && params[i+1]==5 && i+2 < nparams) {
                    t->cur_fg = ansi_256_to_rgba(params[i+2]); i+=2;
                } else if (i+1 < nparams && params[i+1]==2 && i+4 < nparams) {
                    uint8_t r=params[i+2], g=params[i+3], b=params[i+4];
                    t->cur_fg = (r<<24)|(g<<16)|(b<<8)|0xff; i+=4;
                }
                break;
            case 48:
                if (i+1 < nparams && params[i+1]==5 && i+2 < nparams) {
                    t->cur_bg = ansi_256_to_rgba(params[i+2]); i+=2;
                } else if (i+1 < nparams && params[i+1]==2 && i+4 < nparams) {
                    uint8_t r=params[i+2], g=params[i+3], b=params[i+4];
                    t->cur_bg = (r<<24)|(g<<16)|(b<<8)|0xff; i+=4;
                }
                break;
            default: break;
        }
        i++;
    }
}

/* ── CSI ── */
static void handle_csi(terminal_t *t, char final, const char *param_str) {
    int params[32] = {0};
    int nparams = 0;
    const char *p = param_str;
    /* skip leading '?' '>' '!' */
    int priv = (*p == '?' || *p == '>' || *p == '!');
    if (priv) p++;
    if (*p) {
        char tmp[256];
        int tmplen = 0;
        while (*p && tmplen < 254) tmp[tmplen++] = *p++;
        tmp[tmplen] = '\0';
        char *tok = tmp, *end;
        while (*tok && nparams < 32) {
            params[nparams++] = (int)strtol(tok, &end, 10);
            if (*end == ';') tok = end+1; else break;
        }
    }
    if (nparams == 0) nparams = 1;
    int p1 = params[0];

    switch (final) {
        /* ── カーソル移動 ── */
        case 'A': t->cur_y -= (p1?p1:1); if(t->cur_y<0) t->cur_y=0; break;
        case 'B': t->cur_y += (p1?p1:1); if(t->cur_y>=t->rows) t->cur_y=t->rows-1; break;
        case 'C': t->cur_x += (p1?p1:1); if(t->cur_x>=t->cols) t->cur_x=t->cols-1; break;
        case 'D': t->cur_x -= (p1?p1:1); if(t->cur_x<0) t->cur_x=0; break;
        case 'E': t->cur_y += (p1?p1:1); t->cur_x=0; if(t->cur_y>=t->rows) t->cur_y=t->rows-1; break;
        case 'F': t->cur_y -= (p1?p1:1); t->cur_x=0; if(t->cur_y<0) t->cur_y=0; break;
        case 'G': {
            int col = (p1?p1:1)-1;
            t->cur_x = (col<0)?0:(col>=t->cols?t->cols-1:col);
            break;
        }
        case 'H': case 'f': {
            int row = (params[0]?params[0]:1)-1;
            int col = (nparams>1 && params[1]?params[1]:1)-1;
            t->cur_y = (row<0)?0:(row>=t->rows?t->rows-1:row);
            t->cur_x = (col<0)?0:(col>=t->cols?t->cols-1:col);
            break;
        }
        case 'd': {
            int row = (p1?p1:1)-1;
            t->cur_y = (row<0)?0:(row>=t->rows?t->rows-1:row);
            break;
        }
        /* ── 消去 ── */
        case 'J':
            if (p1==0) {
                for (int i=t->cur_y*t->cols+t->cur_x; i<t->cols*t->rows; i++) cell_reset(&t->cells[i]);
            } else if (p1==1) {
                for (int i=0; i<=t->cur_y*t->cols+t->cur_x; i++) cell_reset(&t->cells[i]);
            } else if (p1==2 || p1==3) {
                for (int i=0; i<t->cols*t->rows; i++) cell_reset(&t->cells[i]);
                t->cur_x=0; t->cur_y=0;
            }
            break;
        case 'K':
            if (p1==0) { for(int x=t->cur_x;x<t->cols;x++) cell_reset(&t->cells[t->cur_y*t->cols+x]); }
            else if (p1==1) { for(int x=0;x<=t->cur_x;x++) cell_reset(&t->cells[t->cur_y*t->cols+x]); }
            else if (p1==2) { for(int x=0;x<t->cols;x++) cell_reset(&t->cells[t->cur_y*t->cols+x]); }
            break;
        case 'X': { /* erase chars */
            int n = p1?p1:1;
            for (int x=t->cur_x; x<t->cur_x+n && x<t->cols; x++) cell_reset(&t->cells[t->cur_y*t->cols+x]);
            break;
        }
        /* ── 行操作 ── */
        case 'L': { /* insert lines */
            int n = p1?p1:1;
            int bot = t->scroll_bottom;
            for (int i=0;i<n;i++) {
                memmove(&t->cells[(t->cur_y+1)*t->cols], &t->cells[t->cur_y*t->cols],
                        (bot-t->cur_y)*t->cols*sizeof(term_cell_t));
                for(int x=0;x<t->cols;x++) cell_reset(&t->cells[t->cur_y*t->cols+x]);
            }
            break;
        }
        case 'M': { /* delete lines */
            int n = p1?p1:1;
            int bot = t->scroll_bottom;
            for (int i=0;i<n;i++) {
                memmove(&t->cells[t->cur_y*t->cols], &t->cells[(t->cur_y+1)*t->cols],
                        (bot-t->cur_y)*t->cols*sizeof(term_cell_t));
                for(int x=0;x<t->cols;x++) cell_reset(&t->cells[bot*t->cols+x]);
            }
            break;
        }
        /* ── 文字操作 ── */
        case 'P': { /* delete chars */
            int n = p1?p1:1;
            int end = t->cols-n;
            if (t->cur_x < end)
                memmove(&t->cells[t->cur_y*t->cols+t->cur_x],
                        &t->cells[t->cur_y*t->cols+t->cur_x+n],
                        (end-t->cur_x)*sizeof(term_cell_t));
            for(int x=(t->cur_x<end?end:t->cur_x);x<t->cols;x++) cell_reset(&t->cells[t->cur_y*t->cols+x]);
            break;
        }
        case '@': { /* insert chars */
            int n = p1?p1:1;
            int mv = t->cols-t->cur_x-n;
            if (mv > 0)
                memmove(&t->cells[t->cur_y*t->cols+t->cur_x+n],
                        &t->cells[t->cur_y*t->cols+t->cur_x],
                        mv*sizeof(term_cell_t));
            for(int x=t->cur_x;x<t->cur_x+n&&x<t->cols;x++) cell_reset(&t->cells[t->cur_y*t->cols+x]);
            break;
        }
        /* ── 属性 ── */
        case 'm': handle_sgr(t, params, nparams); break;
        /* ── スクロール領域 ── */
        case 'r':
            t->scroll_top    = (params[0]?params[0]:1)-1;
            t->scroll_bottom = (nparams>1&&params[1]?params[1]:t->rows)-1;
            if (t->scroll_top < 0) t->scroll_top = 0;
            if (t->scroll_bottom >= t->rows) t->scroll_bottom = t->rows-1;
            t->cur_x = 0; t->cur_y = 0;
            break;
        case 'S': for(int i=0;i<(p1?p1:1);i++) scroll_up_one(t); break;
        case 'T': for(int i=0;i<(p1?p1:1);i++) scroll_down_one(t); break;
        /* ── カーソル保存/復元 ── */
        case 's':
            t->saved_x=t->cur_x; t->saved_y=t->cur_y;
            t->saved_fg=t->cur_fg; t->saved_bg=t->cur_bg; t->saved_flags=t->cur_flags;
            break;
        case 'u':
            t->cur_x=t->saved_x; t->cur_y=t->saved_y;
            t->cur_fg=t->saved_fg; t->cur_bg=t->saved_bg; t->cur_flags=t->saved_flags;
            break;
        /* ── DEC private モード ── */
        case 'h': case 'l': {
            int on = (final=='h');
            if (priv) {
                switch (p1) {
                    case 25: t->cursor_visible = on; break;
                    case 1047:
                    case 1049:
                        if (on) { /* enter alt screen */
                            if (!t->alt_cells) {
                                t->alt_cells = calloc(t->cols*t->rows, sizeof(term_cell_t));
                                if (!t->alt_cells) break;
                                for(int i=0;i<t->cols*t->rows;i++) cell_reset(&t->alt_cells[i]);
                            }
                            if (p1==1049) { /* save cursor */
                                t->saved_x=t->cur_x; t->saved_y=t->cur_y;
                                t->saved_fg=t->cur_fg; t->saved_bg=t->cur_bg; t->saved_flags=t->cur_flags;
                            }
                            /* swap main ↔ alt */
                            term_cell_t *tmp=t->cells; t->cells=t->alt_cells; t->alt_cells=tmp;
                            t->use_alt=1;
                            /* clear alt + home */
                            for(int i=0;i<t->cols*t->rows;i++) cell_reset(&t->cells[i]);
                            t->cur_x=0; t->cur_y=0;
                            t->scroll_top=0; t->scroll_bottom=t->rows-1;
                        } else { /* leave alt screen */
                            if (t->alt_cells) {
                                term_cell_t *tmp=t->cells; t->cells=t->alt_cells; t->alt_cells=tmp;
                                t->use_alt=0;
                            }
                            if (p1==1049) { /* restore cursor */
                                t->cur_x=t->saved_x; t->cur_y=t->saved_y;
                                t->cur_fg=t->saved_fg; t->cur_bg=t->saved_bg; t->cur_flags=t->saved_flags;
                            }
                        }
                        break;
                    case 1048:
                        if (on) { t->saved_x=t->cur_x; t->saved_y=t->cur_y; }
                        else    { t->cur_x=t->saved_x; t->cur_y=t->saved_y; }
                        break;
                    default: break;
                }
            }
            break;
        }
        default: break;
    }
}

/* ── UTF-8 デコード ── */
static int utf8_decode(const char *s, int len, uint32_t *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp=c; return 1; }
    if (c < 0xc0) { *cp='?'; return 1; }
    int n; uint32_t min;
    if      (c<0xe0) { n=2; min=0x80;    *cp=c&0x1f; }
    else if (c<0xf0) { n=3; min=0x800;   *cp=c&0x0f; }
    else             { n=4; min=0x10000; *cp=c&0x07; }
    if (len<n) { *cp='?'; return 1; }
    for (int i=1;i<n;i++) {
        unsigned char b=(unsigned char)s[i];
        if ((b&0xc0)!=0x80) { *cp='?'; return 1; }
        *cp=(*cp<<6)|(b&0x3f);
    }
    if (*cp<min) *cp='?';
    return n;
}

/* ── メインパーサー ── */
void terminal_write_n(terminal_t *t, const char *data, int len) {
    t->sb_offset = 0;  /* 新データ受信時はスクロールをリセット */
    int i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)data[i];

        if (t->parse_state == STATE_ESC) {
            switch (c) {
                case '[': t->parse_state=STATE_CSI; t->parse_len=0; t->parse_buf[0]='\0'; break;
                case ']': t->parse_state=STATE_OSC; t->parse_len=0; break;
                case 'c': terminal_reset(t); t->parse_state=STATE_NORMAL; break;
                case '7': /* save cursor */
                    t->saved_x=t->cur_x; t->saved_y=t->cur_y;
                    t->saved_fg=t->cur_fg; t->saved_bg=t->cur_bg; t->saved_flags=t->cur_flags;
                    t->parse_state=STATE_NORMAL; break;
                case '8': /* restore cursor */
                    t->cur_x=t->saved_x; t->cur_y=t->saved_y;
                    t->cur_fg=t->saved_fg; t->cur_bg=t->saved_bg; t->cur_flags=t->saved_flags;
                    t->parse_state=STATE_NORMAL; break;
                case 'M': /* reverse index */
                    if (t->cur_y <= t->scroll_top) scroll_down_one(t);
                    else t->cur_y--;
                    t->parse_state=STATE_NORMAL; break;
                case 'D': /* index (like LF) */
                    newline(t); t->parse_state=STATE_NORMAL; break;
                case 'E': /* next line */
                    t->cur_x=0; newline(t); t->parse_state=STATE_NORMAL; break;
                default:  t->parse_state=STATE_NORMAL; break;
            }
            i++; continue;
        }

        if (t->parse_state == STATE_CSI) {
            if ((c>=0x20&&c<=0x3f)) {
                if (t->parse_len<(int)sizeof(t->parse_buf)-1)
                    t->parse_buf[t->parse_len++]=c;
            } else if (c>=0x40&&c<=0x7e) {
                t->parse_buf[t->parse_len]='\0';
                handle_csi(t, (char)c, t->parse_buf);
                t->parse_state=STATE_NORMAL;
            }
            i++; continue;
        }

        if (t->parse_state == STATE_OSC) {
            /* OSC: terminated by BEL (0x07) or ESC \ (ST) */
            if (c==0x07) {
                t->parse_state=STATE_NORMAL;
            } else if (c==0x1b) {
                /* next char should be '\' — just swallow both */
                t->parse_state=STATE_NORMAL;
                if (i+1<len && (unsigned char)data[i+1]=='\\') i++;
            } else {
                if (t->parse_len<(int)sizeof(t->parse_buf)-1)
                    t->parse_buf[t->parse_len++]=c;
            }
            i++; continue;
        }

        /* STATE_NORMAL */
        switch (c) {
            case 0x1b: t->parse_state=STATE_ESC; i++; break;
            case '\r':  t->cur_x=0; i++; break;
            case '\n':  newline(t); i++; break;
            case '\t': {
                int next_tab=((t->cur_x/8)+1)*8;
                while(t->cur_x<next_tab&&t->cur_x<t->cols) put_char(t,' ');
                i++; break;
            }
            case '\b': if(t->cur_x>0) t->cur_x--; i++; break;
            case 0x07: i++; break; /* BEL */
            case 0x0e: case 0x0f: i++; break; /* SO/SI charset switch - ignore */
            default:
                if (c < 0x20) { i++; break; }
                {
                    uint32_t cp;
                    int consumed=utf8_decode(data+i, len-i, &cp);
                    put_char(t, cp);
                    i+=consumed;
                }
                break;
        }
    }
}

void terminal_write(terminal_t *t, const char *data) {
    terminal_write_n(t, data, (int)strlen(data));
}

void terminal_scroll_view(terminal_t *t, int delta) {
    t->sb_offset += delta;
    if (t->sb_offset < 0) t->sb_offset = 0;
    if (t->sb_offset > t->sb_size) t->sb_offset = t->sb_size;
}

term_cell_t terminal_get_cell(terminal_t *t, int x, int y) {
    if (x<0||x>=t->cols||y<0||y>=t->rows) {
        term_cell_t e={0,DEFAULT_FG,DEFAULT_BG,0}; return e;
    }
    if (t->sb_offset > 0) {
        /* スクロールバック表示: combined = scrollback上の絶対行 */
        int combined = t->sb_size - t->sb_offset + y;
        if (combined >= 0 && combined < t->sb_size) {
            int ring = (t->sb_head - t->sb_size + combined + TERM_SCROLLBACK) % TERM_SCROLLBACK;
            return t->scrollback[ring * t->cols + x];
        }
        int cell_y = combined - t->sb_size;
        if (cell_y >= 0 && cell_y < t->rows)
            return t->cells[cell_y * t->cols + x];
        term_cell_t e={0,DEFAULT_FG,DEFAULT_BG,0}; return e;
    }
    return t->cells[y*t->cols+x];
}
