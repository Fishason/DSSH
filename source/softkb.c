#include "softkb.h"
#include "renderer.h"
#include "keyboard.h"
#include <citro2d.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ──────────────────────────────────────────────────────────────────────
 * Geometry (bottom screen 320×240, top-left is (0, 0))
 * ──────────────────────────────────────────────────────────────────────
 *  y =  0 .. 14   status row (3-char status slot, candidate area, mode)
 *  y = 15 .. 19   margin
 *  y = 20 .. 69   key row 1 (50 px)
 *  y = 72 .. 121  key row 2
 *  y = 124 .. 173 key row 3
 *  y = 176 .. 225 key row 4
 *
 *  10 keys per row × 32 px wide = 320 px exact.  Each key has a 1 px
 *  visual gap on each side so the rendered rectangle is 30×46.
 */

#define KEY_W       32
#define KEY_H       50
#define KEY_GAP     2
#define ROW_Y(r)    (20 + (r) * (KEY_H + KEY_GAP))
#define KEY_X(c)    ((c) * KEY_W)

#define STATUS_Y    0
#define STATUS_H    14
#define CELL_W      6
#define CELL_H      12

/* Colors (RGBA 0xRRGGBBAA, matches our citro2d helper) */
#define COL_BG          0x181825ff   /* dark base */
#define COL_KEY_BG      0x313244ff   /* normal key */
#define COL_KEY_FG      0xcdd6f4ff   /* key label */
#define COL_KEY_PRESSED 0x89b4faff   /* tap highlight */
#define COL_KEY_SPECIAL 0x45475aff   /* page switch / Tab — slight darker */
#define COL_STATUS_BG   0x11111bff
#define COL_STATUS_FG   0xa6e3a1ff   /* green when status active */
#define COL_STATUS_DIM  0x6c7086ff   /* dim when idle */
#define COL_MODE_EN     0xf5c2e7ff   /* pink-ish */
#define COL_MODE_CN     0xf9e2afff   /* yellow-ish */

/* ── key kinds ─────────────────────────────────────────────────────── */

typedef enum {
    KIND_CHAR,        /* emit base char through keyboard_emit_for */
    KIND_SEQ,         /* emit literal seq bytes (no modifier transform) */
    KIND_PAGE_TOGGLE, /* switch between letters / symbols */
} key_kind_t;

typedef struct {
    int x, y, w, h;        /* bounding rect on bottom screen */
    char base;             /* for KIND_CHAR */
    const char *seq;       /* for KIND_SEQ (e.g. "\t") */
    const char *label;     /* what to draw on the key face */
    key_kind_t kind;
} softkey_t;

/* helper to define a normal char key spanning one cell at (col, row) */
#define K(col, row, ch, lbl) \
    { KEY_X(col) + 1, ROW_Y(row), KEY_W - 2, KEY_H, (ch), NULL, (lbl), KIND_CHAR }

/* a wide key spanning 'span' cols */
#define KW(col, row, span, ch, lbl) \
    { KEY_X(col) + 1, ROW_Y(row), (span) * KEY_W - 2, KEY_H, \
      (ch), NULL, (lbl), KIND_CHAR }

/* a literal sequence key */
#define KS(col, row, span, seq_, lbl) \
    { KEY_X(col) + 1, ROW_Y(row), (span) * KEY_W - 2, KEY_H, \
      0, (seq_), (lbl), KIND_SEQ }

/* the page-toggle key */
#define KP(col, row, span, lbl) \
    { KEY_X(col) + 1, ROW_Y(row), (span) * KEY_W - 2, KEY_H, \
      0, NULL, (lbl), KIND_PAGE_TOGGLE }

/* ── page 1: letters ───────────────────────────────────────────────── */
static const softkey_t keys_letters[] = {
    /* row 0: q w e r t y u i o p */
    K(0,0,'q',"q"), K(1,0,'w',"w"), K(2,0,'e',"e"), K(3,0,'r',"r"), K(4,0,'t',"t"),
    K(5,0,'y',"y"), K(6,0,'u',"u"), K(7,0,'i',"i"), K(8,0,'o',"o"), K(9,0,'p',"p"),
    /* row 1: a s d f g h j k l ' */
    K(0,1,'a',"a"), K(1,1,'s',"s"), K(2,1,'d',"d"), K(3,1,'f',"f"), K(4,1,'g',"g"),
    K(5,1,'h',"h"), K(6,1,'j',"j"), K(7,1,'k',"k"), K(8,1,'l',"l"), K(9,1,'\'',"'"),
    /* row 2: z x c v b n m , . / */
    K(0,2,'z',"z"), K(1,2,'x',"x"), K(2,2,'c',"c"), K(3,2,'v',"v"), K(4,2,'b',"b"),
    K(5,2,'n',"n"), K(6,2,'m',"m"), K(7,2,',',","), K(8,2,'.',"."), K(9,2,'/',"/"),
    /* row 3: [123]  SPACE(6 cols)  TAB */
    KP(0,3,2,"123"),
    KW(2,3,6,' ',"space"),
    KS(8,3,2,"\t","tab"),
};
#define N_LETTERS (sizeof(keys_letters) / sizeof(keys_letters[0]))

/* ── page 2: symbols / numbers ─────────────────────────────────────── */
static const softkey_t keys_symbols[] = {
    /* row 0: 1 2 3 4 5 6 7 8 9 0 */
    K(0,0,'1',"1"), K(1,0,'2',"2"), K(2,0,'3',"3"), K(3,0,'4',"4"), K(4,0,'5',"5"),
    K(5,0,'6',"6"), K(6,0,'7',"7"), K(7,0,'8',"8"), K(8,0,'9',"9"), K(9,0,'0',"0"),
    /* row 1: ! @ # $ % ^ & * ( ) */
    K(0,1,'!',"!"), K(1,1,'@',"@"), K(2,1,'#',"#"), K(3,1,'$',"$"), K(4,1,'%',"%"),
    K(5,1,'^',"^"), K(6,1,'&',"&"), K(7,1,'*',"*"), K(8,1,'(',"("), K(9,1,')',")"),
    /* row 2: - + = [ ] ; : ' " / */
    K(0,2,'-',"-"), K(1,2,'+',"+"), K(2,2,'=',"="), K(3,2,'[',"["), K(4,2,']',"]"),
    K(5,2,';',";"), K(6,2,':',":"), K(7,2,'\'',"'"), K(8,2,'"',"\""), K(9,2,'/',"/"),
    /* row 3: [abc] ` < > |  SPACE  \  ~ */
    KP(0,3,2,"abc"),
    K(2,3,'`',"`"), K(3,3,'<',"<"), K(4,3,'>',">"), K(5,3,'|',"|"),
    KW(6,3,2,' ',"space"),
    K(8,3,'\\',"\\"), K(9,3,'~',"~"),
};
#define N_SYMBOLS (sizeof(keys_symbols) / sizeof(keys_symbols[0]))

/* ── softkb_t ──────────────────────────────────────────────────────── */

struct softkb_t {
    softkb_page_t page;
    int           pressed_idx;        /* index of currently-touched key, -1 = none */
    int           pressed_frames;     /* for visual highlight fade */
    char          out_buf[16];
    int           out_len;
};

softkb_t *softkb_init(void) {
    softkb_t *kb = calloc(1, sizeof(*kb));
    if (!kb) return NULL;
    kb->page = PAGE_LETTERS;
    kb->pressed_idx = -1;
    return kb;
}

void softkb_free(softkb_t *kb) { free(kb); }

softkb_page_t softkb_current_page(const softkb_t *kb) {
    return kb ? kb->page : PAGE_LETTERS;
}

static const softkey_t *current_layout(const softkb_t *kb, int *count) {
    if (kb->page == PAGE_SYMBOLS) {
        *count = N_SYMBOLS; return keys_symbols;
    }
    *count = N_LETTERS; return keys_letters;
}

/* ── touch handling ────────────────────────────────────────────────── */

static int hit_test(const softkb_t *kb, int tx, int ty) {
    int n;
    const softkey_t *layout = current_layout(kb, &n);
    for (int i = 0; i < n; i++) {
        const softkey_t *k = &layout[i];
        if (tx >= k->x && tx < k->x + k->w &&
            ty >= k->y && ty < k->y + k->h)
            return i;
    }
    return -1;
}

const char *softkb_touch(softkb_t *kb,
                         keyboard_t *kbd,
                         int tx, int ty,
                         int pressed) {
    if (!kb || !kbd) return NULL;

    if (!pressed && kb->pressed_idx >= 0) {
        /* Touch lifted — clear highlight after a short fade. */
        kb->pressed_frames++;
        if (kb->pressed_frames > 6) kb->pressed_idx = -1;
        return NULL;
    }
    if (!pressed) return NULL;
    if (tx < 0 || ty < 0) return NULL;

    /* Fresh press — locate hit. */
    int idx = hit_test(kb, tx, ty);
    if (idx < 0) {
        kb->pressed_idx = -1;
        return NULL;
    }
    kb->pressed_idx    = idx;
    kb->pressed_frames = 0;

    int n;
    const softkey_t *layout = current_layout(kb, &n);
    const softkey_t *k = &layout[idx];

    switch (k->kind) {
        case KIND_PAGE_TOGGLE:
            kb->page = (kb->page == PAGE_LETTERS) ? PAGE_SYMBOLS : PAGE_LETTERS;
            return NULL;
        case KIND_SEQ:
            /* Send raw sequence; modifier keys don't currently transform
             * the sequence (rare to need Shift+Tab via touch — TODO). */
            return k->seq;
        case KIND_CHAR:
            return keyboard_emit_for(kbd, k->base);
    }
    return NULL;
}

/* ── rendering helpers ─────────────────────────────────────────────── */

static u32 rgba_to_c2d_(uint32_t rgba) {
    return C2D_Color32((rgba >> 24) & 0xff,
                       (rgba >> 16) & 0xff,
                       (rgba >>  8) & 0xff,
                        rgba        & 0xff);
}

/* Rounded-ish rectangle for keys: just draw a slightly darker outer rect
 * and a brighter inner.  Cheap "raised button" look. */
static void draw_key_bg(int x, int y, int w, int h, u32 bg_rgba) {
    /* outer (slightly darker) — use status_bg as outline */
    C2D_DrawRectSolid((float)x, (float)y, 0.1f,
                      (float)w, (float)h,
                      rgba_to_c2d_(COL_STATUS_BG));
    /* inner */
    C2D_DrawRectSolid((float)(x + 1), (float)(y + 1), 0.15f,
                      (float)(w - 2), (float)(h - 2),
                      rgba_to_c2d_(bg_rgba));
}

/* Draw a label string centered horizontally + vertically in a given rect.
 * Uses the global font_atlas via the renderer's narrow-glyph path. */
static void draw_label(int rx, int ry, int rw, int rh,
                       const char *text, u32 fg_rgba) {
    if (!text) return;
    int tlen = (int)strlen(text);
    int tw   = tlen * CELL_W;
    int x0   = rx + (rw - tw) / 2;
    int y0   = ry + (rh - CELL_H) / 2;
    /* Use renderer_draw_text by passing cell-grid coords. */
    int cx = x0 / CELL_W;
    int cy = y0 / CELL_H;
    /* renderer_draw_text expects cell-aligned coords; nudge to closest. */
    renderer_draw_text(NULL, cx, cy, text, fg_rgba);
}

/* ── public draw ───────────────────────────────────────────────────── */

void softkb_draw(softkb_t *kb, renderer_t *r, const keyboard_t *kbd) {
    if (!kb || !r) return;

    /* (Caller already C2D_TargetClear'd to COL_BG and called C2D_SceneBegin.) */

    /* ── status row (y = 0..14) ── */
    C2D_DrawRectSolid(0, 0, 0.05f, 320, STATUS_H,
                      rgba_to_c2d_(COL_STATUS_BG));

    /* [STA] 3-char status slot (cols 0-2) */
    const char *status = kbd ? keyboard_status_label(kbd) : "   ";
    int active = (status && strcmp(status, "   ") != 0);
    /* highlighted background when active */
    if (active) {
        C2D_DrawRectSolid(0, 0, 0.08f, 3 * CELL_W + 2, STATUS_H,
                          rgba_to_c2d_(COL_KEY_PRESSED));
    }
    renderer_draw_text(r, 0, 0, status,
                       active ? COL_BG : COL_STATUS_DIM);

    /* [EN] / [CN] mode slot (rightmost 3 cols) */
    ime_mode_t m = kbd ? keyboard_get_mode(kbd) : MODE_EN;
    const char *mode_label = (m == MODE_CN) ? "CN " : "EN ";
    u32 mode_color = (m == MODE_CN) ? COL_MODE_CN : COL_MODE_EN;
    renderer_draw_text(r, 53 - 3, 0, mode_label, mode_color);

    /* ── keys ── */
    int n;
    const softkey_t *layout = current_layout(kb, &n);
    for (int i = 0; i < n; i++) {
        const softkey_t *k = &layout[i];
        u32 bg;
        if (i == kb->pressed_idx) bg = COL_KEY_PRESSED;
        else if (k->kind != KIND_CHAR) bg = COL_KEY_SPECIAL;
        else bg = COL_KEY_BG;
        draw_key_bg(k->x, k->y, k->w, k->h, bg);
        draw_label(k->x, k->y, k->w, k->h, k->label,
                   (i == kb->pressed_idx) ? COL_BG : COL_KEY_FG);
    }
}
