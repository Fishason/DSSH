#include "softkb.h"
#include "renderer.h"
#include "keyboard.h"
#include "audio.h"
#include <citro2d.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ──────────────────────────────────────────────────────────────────────
 * Geometry — bottom screen 320×240, top-left = (0,0)
 * ──────────────────────────────────────────────────────────────────────
 *  y =  0 .. 29   status / candidate row (30 px — taller than M4-first
 *                  for the IME candidate strip in M7)
 *  y = 30 .. 31   margin
 *  y = 32 .. 75   key row 0 (44 px)
 *  y = 77 .. 120  key row 1
 *  y = 122 .. 165 key row 2
 *  y = 167 .. 210 key row 3
 *  y = 211..239   bottom margin (29 px)
 *
 *  Stagger (mimics PC keyboards: each row is offset by ~half a key):
 *    row 0: x offset =  0   (10 keys × 32 px = 320 — exact fit)
 *    row 1: x offset = 16   (9 keys → ends at 304, 16 px right margin)
 *    row 2: x offset = 32   (9 keys → ends at 320 — exact fit)
 *    row 3: x offset =  0   (control row; large keys, no stagger)
 *
 *  z layers (citro2d uses depth for overdraw ordering with alpha):
 *    0.05  bottom screen background
 *    0.10  key shadow / outline
 *    0.15  key body
 *    0.18  key top highlight
 *    0.20  key label glyph
 *    0.30  status row (drawn earlier so labels overlay)
 */

#define KEY_W       32
#define KEY_H       44
#define KEY_GAP_Y   2
#define ROW_BASE_Y  32
#define ROW_Y(r)    (ROW_BASE_Y + (r) * (KEY_H + KEY_GAP_Y))

#define ROW_X_0     0
#define ROW_X_1     16
#define ROW_X_2     32
#define ROW_X_3     0

#define STATUS_Y    0
#define STATUS_H    30
#define CELL_W      6
#define CELL_H      12

/* ── Colors (RGBA 0xRRGGBBAA, Catppuccin Mocha-ish) ────────────────── */
#define COL_BG              0x11111bff   /* darkest, bottom screen base */
#define COL_STATUS_BG       0x181825ff   /* 1 step lighter — status row */
#define COL_CANDIDATE_BG    0x1e1e2eff   /* candidate strip background */

#define COL_KEY_BODY        0x313244ff   /* normal key */
#define COL_KEY_TOP         0x45475aff   /* 1px highlight on top edge */
#define COL_KEY_BOT_SHADOW  0x06060eff   /* 1px shadow under key */
#define COL_KEY_BORDER      0x1e1e2eff   /* outline */
#define COL_KEY_LABEL       0xcdd6f4ff

#define COL_KEY_SPECIAL     0x45475a8b   /* slightly different grey */
#define COL_KEY_PG_BODY     0x74c7ecff   /* page-switch — sky blue */
#define COL_KEY_SPACE_BODY  0x6c7086ff   /* space — neutral mid grey */

#define COL_KEY_PRESSED     0x89b4faff   /* tap highlight */
#define COL_KEY_PRESSED_FG  0x11111bff   /* dark label on bright press */

#define COL_STATUS_FG_HOLD  0xa6e3a1ff   /* green when modifier held */
#define COL_STATUS_FG_FLASH 0xfab387ff   /* orange when transient flash */
#define COL_STATUS_DIM      0x45475aff
#define COL_STATUS_HOLD_BG  0x89b4fa50   /* faint blue tint behind held mod */

#define COL_MODE_EN         0xf5c2e7ff
#define COL_MODE_CN         0xf9e2afff
#define COL_MODE_LBL_BG     0x313244ff

/* ── key kinds ─────────────────────────────────────────────────────── */

typedef enum {
    KIND_CHAR,
    KIND_SEQ,
    KIND_PAGE_TOGGLE,
    KIND_SPACE,        /* visual variant: large neutral */
    KIND_PAGE_BTN,     /* visual variant: page switch — accent color */
} key_kind_t;

typedef struct {
    int x, y, w, h;
    char base;
    const char *seq;
    const char *label;
    key_kind_t kind;
} softkey_t;

#define K(col, row, ch, lbl) \
    { ROW_X_##row + (col) * KEY_W + 1, ROW_Y(row), KEY_W - 2, KEY_H, \
      (ch), NULL, (lbl), KIND_CHAR }

#define KW(col, row, span, ch, lbl, kind_) \
    { ROW_X_##row + (col) * KEY_W + 1, ROW_Y(row), (span) * KEY_W - 2, KEY_H, \
      (ch), NULL, (lbl), (kind_) }

#define KS(col, row, span, seq_, lbl, kind_) \
    { ROW_X_##row + (col) * KEY_W + 1, ROW_Y(row), (span) * KEY_W - 2, KEY_H, \
      0, (seq_), (lbl), (kind_) }

#define KP(col, row, span, lbl) \
    { ROW_X_##row + (col) * KEY_W + 1, ROW_Y(row), (span) * KEY_W - 2, KEY_H, \
      0, NULL, (lbl), KIND_PAGE_BTN }

/* ── Page 1: Letters ───────────────────────────────────────────────── */
static const softkey_t keys_letters[] = {
    /* row 0 (qwerty), 10 keys, no stagger */
    K(0,0,'q',"q"), K(1,0,'w',"w"), K(2,0,'e',"e"), K(3,0,'r',"r"), K(4,0,'t',"t"),
    K(5,0,'y',"y"), K(6,0,'u',"u"), K(7,0,'i',"i"), K(8,0,'o',"o"), K(9,0,'p',"p"),
    /* row 1 (asdf), 9 keys, half-key stagger */
    K(0,1,'a',"a"), K(1,1,'s',"s"), K(2,1,'d',"d"), K(3,1,'f',"f"), K(4,1,'g',"g"),
    K(5,1,'h',"h"), K(6,1,'j',"j"), K(7,1,'k',"k"), K(8,1,'l',"l"),
    /* row 2 (zxcv), 9 keys, full-key stagger */
    K(0,2,'z',"z"), K(1,2,'x',"x"), K(2,2,'c',"c"), K(3,2,'v',"v"), K(4,2,'b',"b"),
    K(5,2,'n',"n"), K(6,2,'m',"m"), K(7,2,',',","), K(8,2,'.',"."),
    /* row 3 (controls): [123] | tab | space (6 cols) */
    KP(0,3,3,"123"),
    KS(3,3,1,"\t","tab", KIND_SEQ),
    KW(4,3,6,' ',"space", KIND_SPACE),
};
#define N_LETTERS (sizeof(keys_letters) / sizeof(keys_letters[0]))

/* ── Page 2: Symbols / Numbers ─────────────────────────────────────── */
static const softkey_t keys_symbols[] = {
    /* row 0: 1-9 0 */
    K(0,0,'1',"1"), K(1,0,'2',"2"), K(2,0,'3',"3"), K(3,0,'4',"4"), K(4,0,'5',"5"),
    K(5,0,'6',"6"), K(6,0,'7',"7"), K(7,0,'8',"8"), K(8,0,'9',"9"), K(9,0,'0',"0"),
    /* row 1: shifted-numbers (with stagger) */
    K(0,1,'!',"!"), K(1,1,'@',"@"), K(2,1,'#',"#"), K(3,1,'$',"$"), K(4,1,'%',"%"),
    K(5,1,'^',"^"), K(6,1,'&',"&"), K(7,1,'*',"*"), K(8,1,'(',"("),
    /* row 2: more symbols (with bigger stagger) */
    K(0,2,'-',"-"), K(1,2,'+',"+"), K(2,2,'=',"="), K(3,2,'[',"["), K(4,2,']',"]"),
    K(5,2,';',";"), K(6,2,':',":"), K(7,2,'\'',"'"), K(8,2,'/',"/"),
    /* row 3: [abc] | ` < > | space (4 cols) | \ ~ */
    KP(0,3,2,"abc"),
    K(2,3,'`',"`"),
    K(3,3,'<',"<"),
    K(4,3,'>',">"),
    KW(5,3,4,' ',"space", KIND_SPACE),
    K(9,3,'\\',"\\"),
};
#define N_SYMBOLS (sizeof(keys_symbols) / sizeof(keys_symbols[0]))

/* ── softkb_t ──────────────────────────────────────────────────────── */

struct softkb_t {
    softkb_page_t page;
    int           pressed_idx;     /* index of currently-touched key, -1 = none */
    int           pressed_frames;  /* visual press-down animation timer */
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

/* ── touch hit-test ────────────────────────────────────────────────── */

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

    if (!pressed) {
        if (kb->pressed_idx >= 0) {
            kb->pressed_frames++;
            if (kb->pressed_frames > 6) kb->pressed_idx = -1;
        }
        return NULL;
    }
    if (tx < 0 || ty < 0) return NULL;

    int idx = hit_test(kb, tx, ty);
    if (idx < 0) {
        kb->pressed_idx = -1;
        return NULL;
    }
    kb->pressed_idx    = idx;
    kb->pressed_frames = 0;
    audio_play_click();   /* tactile audio feedback on every soft-kb tap */

    int n;
    const softkey_t *layout = current_layout(kb, &n);
    const softkey_t *k = &layout[idx];

    switch (k->kind) {
        case KIND_PAGE_TOGGLE:
        case KIND_PAGE_BTN:
            kb->page = (kb->page == PAGE_LETTERS) ? PAGE_SYMBOLS : PAGE_LETTERS;
            return NULL;
        case KIND_SEQ:
            return k->seq;
        case KIND_SPACE:
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

/* "Raised button" rendering — multiple stacked rects for a tactile look:
 *   1) bottom shadow: 1 px below body
 *   2) left/right outlines: dark border
 *   3) main body
 *   4) top highlight: 1 px lighter at top edge (skip when pressed)
 *
 * When pressed=1 we shift the body down 1 px so the key visually "depresses".
 */
static void draw_key_button(int x, int y, int w, int h,
                            uint32_t body, int pressed) {
    int dy = pressed ? 1 : 0;

    /* bottom shadow (only when not pressed) */
    if (!pressed) {
        C2D_DrawRectSolid((float)x, (float)(y + h),
                          0.10f, (float)w, 1,
                          rgba_to_c2d_(COL_KEY_BOT_SHADOW));
    }

    /* dark border (the whole rect) */
    C2D_DrawRectSolid((float)x, (float)(y + dy), 0.11f,
                      (float)w, (float)h,
                      rgba_to_c2d_(COL_KEY_BORDER));
    /* main body, inset 1 px on every side */
    C2D_DrawRectSolid((float)(x + 1), (float)(y + 1 + dy), 0.15f,
                      (float)(w - 2), (float)(h - 2),
                      rgba_to_c2d_(body));
    /* top highlight */
    if (!pressed) {
        C2D_DrawRectSolid((float)(x + 1), (float)(y + 1), 0.18f,
                          (float)(w - 2), 1,
                          rgba_to_c2d_(COL_KEY_TOP));
    }
}

/* Centered text label.  Note: renderer_draw_text takes cell-grid coords,
 * so we approximate centering by computing the closest cell offset. */
static void draw_label(int rx, int ry, int rw, int rh,
                       const char *text, u32 fg_rgba, int pressed) {
    if (!text) return;
    int tlen = (int)strlen(text);
    int tw   = tlen * CELL_W;
    int x0   = rx + (rw - tw) / 2;
    int y0   = ry + (rh - CELL_H) / 2 + (pressed ? 1 : 0);
    int cx   = x0 / CELL_W;
    int cy   = y0 / CELL_H;
    renderer_draw_text(NULL, cx, cy, text, fg_rgba);
}

/* Decide the body fill color for a key based on its kind and press state. */
static uint32_t key_body_color(const softkey_t *k, int is_pressed) {
    if (is_pressed) return COL_KEY_PRESSED;
    switch (k->kind) {
        case KIND_PAGE_BTN:    return COL_KEY_PG_BODY;
        case KIND_SPACE:       return COL_KEY_SPACE_BODY;
        case KIND_PAGE_TOGGLE: return COL_KEY_SPECIAL;
        case KIND_SEQ:         return COL_KEY_SPECIAL;
        case KIND_CHAR:
        default:               return COL_KEY_BODY;
    }
}

static uint32_t key_label_color(const softkey_t *k, int is_pressed) {
    if (is_pressed) return COL_KEY_PRESSED_FG;
    if (k->kind == KIND_PAGE_BTN) return COL_KEY_PRESSED_FG; /* dark on bright */
    return COL_KEY_LABEL;
}

/* ── status / candidate row ─────────────────────────────────────────── */

static void draw_status_row(renderer_t *r, const keyboard_t *kbd) {
    /* Status row band */
    C2D_DrawRectSolid(0, 0, 0.05f, 320, STATUS_H,
                      rgba_to_c2d_(COL_STATUS_BG));
    /* Candidate area background (between [STA] and mode badge) */
    C2D_DrawRectSolid(3 * CELL_W + 4, 4, 0.06f,
                      320 - 3 * CELL_W - 4 - 3 * CELL_W - 8, STATUS_H - 8,
                      rgba_to_c2d_(COL_CANDIDATE_BG));

    /* [STA] indicator slot — 3 cells wide on the very left */
    const char *status = kbd ? keyboard_status_label(kbd) : "   ";
    int active = (status && strcmp(status, "   ") != 0);
    if (active) {
        C2D_DrawRectSolid(2, 2, 0.07f,
                          3 * CELL_W + 4, STATUS_H - 4,
                          rgba_to_c2d_(COL_STATUS_HOLD_BG));
    }
    int label_color = active ? COL_STATUS_FG_HOLD : COL_STATUS_DIM;
    /* Center the 3-char label vertically in the 30 px status row.
     * status is at row 0 (cell-grid Y), but the area starts at y=0.
     * Cell row 1 puts the text at y=12. With 30 px row, we want y≈9
     * so 1 cell row down (12) is close enough. */
    renderer_draw_text(r, 1, 1, status, label_color);

    /* Mode badge on the very right */
    ime_mode_t m = kbd ? keyboard_get_mode(kbd) : MODE_EN;
    const char *mode_label = (m == MODE_CN) ? " CN" : " EN";
    u32 mode_color = (m == MODE_CN) ? COL_MODE_CN : COL_MODE_EN;
    /* Background pill behind the mode label */
    C2D_DrawRectSolid(320 - (3 * CELL_W + 6), 2, 0.07f,
                      3 * CELL_W + 4, STATUS_H - 4,
                      rgba_to_c2d_(COL_MODE_LBL_BG));
    renderer_draw_text(r, 53 - 3, 1, mode_label, mode_color);
}

/* ── public draw ───────────────────────────────────────────────────── */

void softkb_draw(softkb_t *kb, renderer_t *r, const keyboard_t *kbd) {
    if (!kb || !r) return;

    draw_status_row(r, kbd);

    int n;
    const softkey_t *layout = current_layout(kb, &n);
    for (int i = 0; i < n; i++) {
        const softkey_t *k = &layout[i];
        int is_pressed = (i == kb->pressed_idx);
        uint32_t body  = key_body_color(k, is_pressed);
        uint32_t lbl   = key_label_color(k, is_pressed);
        draw_key_button(k->x, k->y, k->w, k->h, body, is_pressed);
        draw_label(k->x, k->y, k->w, k->h, k->label, lbl, is_pressed);
    }
}
