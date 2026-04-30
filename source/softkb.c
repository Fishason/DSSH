#include "softkb.h"
#include "renderer.h"
#include "keyboard.h"
#include <citro2d.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ──────────────────────────────────────────────────────────────────────
 * Geometry — bottom screen 320×240, top-left = (0,0)
 * ──────────────────────────────────────────────────────────────────────
 *  y =  0 .. 33   status / candidate row (34 px — bumped from 30 in
 *                  M4 polish so IME candidates have more vertical room)
 *  y = 34 .. 35   margin
 *  y = 36 .. 78   key row 0 (43 px — 1 px shaved from M4-first to free
 *                  space for the bottom row at y=214..239)
 *  y = 81 .. 123  key row 1
 *  y = 126 .. 168 key row 2
 *  y = 171 .. 213 key row 3
 *  y = 214..239   bottom row owned by main.c — clock on the left,
 *                  Anthropic crab mascot on the right.  softkb does NOT
 *                  draw here.
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
#define KEY_H       43
#define KEY_GAP_Y   2
#define ROW_BASE_Y  36
#define ROW_Y(r)    (ROW_BASE_Y + (r) * (KEY_H + KEY_GAP_Y))

#define ROW_X_0     0
#define ROW_X_1     16
#define ROW_X_2     32
#define ROW_X_3     0

#define STATUS_Y    0
#define STATUS_H    34
#define CELL_W      6
#define CELL_H      12

/* ── Colors (RGBA 0xRRGGBBAA, Catppuccin Mocha-ish) ────────────────── */
#define COL_BG              0x11111bff   /* darkest, bottom screen base */
#define COL_STATUS_BG       0x181825ff   /* 1 step lighter — status row */
#define COL_CANDIDATE_BG    0x1e1e2eff   /* candidate strip background */
#define COL_CHAMFER         0x181825ff   /* corner punch-out — must match
                                          * the C2D_TargetClear bg color
                                          * in main.c so the rounded
                                          * corners blend invisibly */

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
            /* Keep the key visually engaged through the full press-down
             * animation (see PRESS_ANIM_FRAMES below). */
            if (kb->pressed_frames > 14) kb->pressed_idx = -1;
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

/* Press-down animation timing.  When a key is tapped, pressed_frames
 * counts up from 0; press_depth() turns that frame counter into a
 * pixel-offset and an interpolation factor used to smoothly translate
 * the key downward and blend its body color toward the press color.
 *
 *   0          PEAK              END
 *   |-----------|-----------------|
 *   depth: 0 → MAX               → 0
 *
 * 60 fps means PRESS_ANIM_FRAMES=14 ≈ 230 ms — slow enough to feel
 * like a key going down and snapping back, fast enough to keep up
 * with rapid typing. */
#define PRESS_ANIM_FRAMES  14
#define PRESS_ANIM_PEAK     6
#define PRESS_MAX_DEPTH     2

static int press_depth(int frames) {
    if (frames < 0 || frames >= PRESS_ANIM_FRAMES) return 0;
    if (frames <= PRESS_ANIM_PEAK) {
        /* down phase: 0 → MAX */
        return (frames * PRESS_MAX_DEPTH + PRESS_ANIM_PEAK / 2)
             / PRESS_ANIM_PEAK;
    }
    /* up phase: MAX → 0 */
    int up    = PRESS_ANIM_FRAMES - PRESS_ANIM_PEAK;
    int phase = PRESS_ANIM_FRAMES - frames;
    return (phase * PRESS_MAX_DEPTH + up / 2) / up;
}

/* Channel-wise lerp between two RGBA colors.  t / range → 0..1 weight
 * toward `b`.  Saturating, so out-of-range t just clamps. */
static uint32_t blend_rgba(uint32_t a, uint32_t b, int t, int range) {
    if (range <= 0 || t <= 0) return a;
    if (t >= range) return b;
    uint32_t ar = (a >> 24) & 0xff, ag = (a >> 16) & 0xff;
    uint32_t ab = (a >>  8) & 0xff, aa =  a        & 0xff;
    uint32_t br = (b >> 24) & 0xff, bg = (b >> 16) & 0xff;
    uint32_t bb = (b >>  8) & 0xff, ba =  b        & 0xff;
    uint32_t r  = (ar * (range - t) + br * t) / range;
    uint32_t g  = (ag * (range - t) + bg * t) / range;
    uint32_t bl = (ab * (range - t) + bb * t) / range;
    uint32_t al = (aa * (range - t) + ba * t) / range;
    return (r << 24) | (g << 16) | (bl << 8) | al;
}

/* 3-pixel triangular round-corner.  Punches the key's four corners with
 * the screen bg color so the rectangle looks rounded:
 *
 *   ###..      (row 0:  3 px)
 *   ##...      (row 1:  2 px)
 *   #....      (row 2:  1 px)
 *   .....
 *
 * 3 px is ~10% of the 32 px key width — the same proportion iOS uses
 * for its keyboard, and visibly more rounded than the previous 2-px
 * L-shaped chamfer. */
static void round_corners(int x, int y, int w, int h, float z) {
    uint32_t c = rgba_to_c2d_(COL_CHAMFER);
    /* TL */
    C2D_DrawRectSolid((float)x,       (float)y,       z, 3, 1, c);
    C2D_DrawRectSolid((float)x,       (float)(y + 1), z, 2, 1, c);
    C2D_DrawRectSolid((float)x,       (float)(y + 2), z, 1, 1, c);
    /* TR */
    C2D_DrawRectSolid((float)(x+w-3), (float)y,       z, 3, 1, c);
    C2D_DrawRectSolid((float)(x+w-2), (float)(y + 1), z, 2, 1, c);
    C2D_DrawRectSolid((float)(x+w-1), (float)(y + 2), z, 1, 1, c);
    /* BL */
    C2D_DrawRectSolid((float)x,       (float)(y+h-1), z, 3, 1, c);
    C2D_DrawRectSolid((float)x,       (float)(y+h-2), z, 2, 1, c);
    C2D_DrawRectSolid((float)x,       (float)(y+h-3), z, 1, 1, c);
    /* BR */
    C2D_DrawRectSolid((float)(x+w-3), (float)(y+h-1), z, 3, 1, c);
    C2D_DrawRectSolid((float)(x+w-2), (float)(y+h-2), z, 2, 1, c);
    C2D_DrawRectSolid((float)(x+w-1), (float)(y+h-3), z, 1, 1, c);
}

/* Render one key.  `depth` is 0 when at rest and grows up to
 * PRESS_MAX_DEPTH (2 px) at the peak of the press animation.  The
 * whole key — border, body, top highlight, label, corners — translates
 * downward by `depth`, mimicking a real keyboard cap going down.
 *
 *   - At rest (depth=0):  shadow under the key + top highlight strip
 *                          give the 3D lifted look.
 *   - Mid-press:           shadow fades, body slides down, color blends
 *                          toward the press color.
 *
 * Z layers (base 0.10, increments of 0.01):
 *   0.10  shadow strip below resting key
 *   0.12  border (dark stroke around body)
 *   0.15  body fill
 *   0.18  top highlight strip (resting only)
 *   0.21  rounded-corner chamfer
 */
static void draw_key_button(int x, int y, int w, int h,
                            uint32_t body, int depth) {
    int yd = y + depth;

    /* Resting drop shadow, fades out as depth grows. */
    if (depth == 0) {
        C2D_DrawRectSolid((float)(x + 1), (float)(y + h), 0.10f,
                          (float)(w - 2), 1,
                          rgba_to_c2d_(COL_KEY_BOT_SHADOW));
    }

    /* Border around the (possibly translated) body. */
    C2D_DrawRectSolid((float)x, (float)yd, 0.12f,
                      (float)w, (float)h,
                      rgba_to_c2d_(COL_KEY_BORDER));
    /* Body fill, inset 1 px from the border. */
    C2D_DrawRectSolid((float)(x + 1), (float)(yd + 1), 0.15f,
                      (float)(w - 2), (float)(h - 2),
                      rgba_to_c2d_(body));
    /* Top highlight only when resting (the sliver at the top of a
     * physical keycap that catches light). */
    if (depth == 0) {
        C2D_DrawRectSolid((float)(x + 1), (float)(yd + 1), 0.18f,
                          (float)(w - 2), 1,
                          rgba_to_c2d_(COL_KEY_TOP));
    }
    /* Corners follow the body's translated position. */
    round_corners(x, yd, w, h, 0.21f);
}

/* Centered text label.  Like draw_key_button, the label translates
 * downward by `depth` so it stays glued to the moving body. */
static void draw_label(int rx, int ry, int rw, int rh,
                       const char *text, u32 fg_rgba, int depth) {
    if (!text) return;
    int tlen = (int)strlen(text);     /* labels are ASCII; bytes == cols */
    int tw   = tlen * CELL_W;
    int x0   = rx + (rw - tw) / 2;
    int y0   = ry + (rh - CELL_H) / 2 + depth;
    renderer_draw_text_px(x0, y0, text, fg_rgba);
}

/* Resting (un-pressed) body fill color per key kind.  The press
 * animation blends from this toward COL_KEY_PRESSED rather than
 * snapping. */
static uint32_t key_body_color_resting(const softkey_t *k) {
    switch (k->kind) {
        case KIND_PAGE_BTN:    return COL_KEY_PG_BODY;
        case KIND_SPACE:       return COL_KEY_SPACE_BODY;
        case KIND_PAGE_TOGGLE: return COL_KEY_SPECIAL;
        case KIND_SEQ:         return COL_KEY_SPECIAL;
        case KIND_CHAR:
        default:               return COL_KEY_BODY;
    }
}

static uint32_t key_label_color_resting(const softkey_t *k) {
    if (k->kind == KIND_PAGE_BTN) return COL_KEY_PRESSED_FG; /* dark on bright */
    return COL_KEY_LABEL;
}

/* ── status / candidate row ─────────────────────────────────────────── */

/* Layout (left → right):
 *   [2..2+slot_w]              left slot  (status indicator)
 *   [slot_w+6..320-slot_w-8]   candidate strip
 *   [320-slot_w-2..318]        right slot (mode badge)
 *
 * slot_w = 3 chars * 6 px + 4 px padding = 22 px.
 * slot_h = STATUS_H - 4 (2 px top/bot margin).
 *
 * All labels rendered with renderer_draw_text_px so 3-char labels land
 * exactly on their geometric centers, no cell-grid snapping. */
static void draw_status_row(renderer_t *r, const keyboard_t *kbd) {
    const int slot_w = 3 * CELL_W + 4;     /* 22 px */
    const int slot_h = STATUS_H - 4;
    const int slot_y = 2;
    const int label_tw = 3 * CELL_W;       /* 18 px */
    const int label_y  = slot_y + (slot_h - CELL_H) / 2;

    /* Status row band (full width). */
    C2D_DrawRectSolid(0, 0, 0.05f, 320, STATUS_H,
                      rgba_to_c2d_(COL_STATUS_BG));

    /* Candidate strip between the two slots. */
    C2D_DrawRectSolid((float)(slot_w + 6), (float)slot_y, 0.06f,
                      (float)(320 - 2 * slot_w - 12), (float)slot_h,
                      rgba_to_c2d_(COL_CANDIDATE_BG));

    /* ── Left slot: status indicator ─────────────────────────────────── */
    const char *status = kbd ? keyboard_status_label(kbd) : "   ";
    int active = (status && strcmp(status, "   ") != 0);

    /* Always-drawn slot bg keeps the layout visually anchored. */
    C2D_DrawRectSolid(2, (float)slot_y, 0.07f,
                      (float)slot_w, (float)slot_h,
                      rgba_to_c2d_(COL_MODE_LBL_BG));
    if (active) {
        /* Highlight overlay: faint blue tint behind the active label. */
        C2D_DrawRectSolid(2, (float)slot_y, 0.072f,
                          (float)slot_w, (float)slot_h,
                          rgba_to_c2d_(COL_STATUS_HOLD_BG));
        int x0 = 2 + (slot_w - label_tw) / 2;
        renderer_draw_text_px(x0, label_y, status, COL_STATUS_FG_HOLD);
    }

    /* ── Right slot: 3-char mode badge "ENG"/"CHN" ───────────────────── */
    ime_mode_t m = kbd ? keyboard_get_mode(kbd) : MODE_EN;
    const char *mode_label = (m == MODE_CN) ? "CHN" : "ENG";
    uint32_t mode_color    = (m == MODE_CN) ? COL_MODE_CN : COL_MODE_EN;
    int rx = 320 - slot_w - 2;
    C2D_DrawRectSolid((float)rx, (float)slot_y, 0.07f,
                      (float)slot_w, (float)slot_h,
                      rgba_to_c2d_(COL_MODE_LBL_BG));
    int mx = rx + (slot_w - label_tw) / 2;
    renderer_draw_text_px(mx, label_y, mode_label, mode_color);

    (void)r;  /* unused with the px path */
}

/* ── public draw ───────────────────────────────────────────────────── */

void softkb_draw(softkb_t *kb, renderer_t *r, const keyboard_t *kbd) {
    if (!kb || !r) return;

    draw_status_row(r, kbd);

    int n;
    const softkey_t *layout = current_layout(kb, &n);
    for (int i = 0; i < n; i++) {
        const softkey_t *k = &layout[i];
        /* Only the actively-pressed key has nonzero depth/blend; every
         * other key renders at rest with depth=0, which makes both
         * blend_rgba calls below short-circuit to the resting color. */
        int depth = (i == kb->pressed_idx)
                  ? press_depth(kb->pressed_frames)
                  : 0;
        uint32_t resting_body = key_body_color_resting(k);
        uint32_t resting_lbl  = key_label_color_resting(k);
        uint32_t body = blend_rgba(resting_body, COL_KEY_PRESSED,
                                   depth, PRESS_MAX_DEPTH);
        uint32_t lbl  = blend_rgba(resting_lbl,  COL_KEY_PRESSED_FG,
                                   depth, PRESS_MAX_DEPTH);
        draw_key_button(k->x, k->y, k->w, k->h, body, depth);
        draw_label(k->x, k->y, k->w, k->h, k->label, lbl, depth);
    }
}
