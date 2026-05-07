#include "ai_modal.h"
#include "renderer.h"

#include <citro2d.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ── Geometry (bottom screen 320×240) ───────────────────────────────
 *
 *   x ∈ [0, 320),  y ∈ [0, 240)
 *
 *   modal      = (6, 6, 308, 228)        — 6 px gap from each edge
 *   padding    = 10                       — inside the modal frame
 *   question   = at y=14, 12-px-tall font, single line, ellipsis on overflow
 *   separator  = horizontal hairline at y=29 (1 px)
 *   answer     = wrap-rendered from y=32 down to y=216 → 14 lines × 13 px
 *
 * z layers — citro2d/citro3d uses higher z = drawn on top.  softkb
 * draws keys around z=0.07-0.30 and renderer_draw_text_px puts label
 * glyphs at z=0.5; the modal layers must therefore sit ABOVE 0.5 so
 * the modal's own text can stack above its own background:
 *   0.55  outside-modal dimming overlay
 *   0.56  modal background
 *   0.57  modal border (subtle)
 *   0.60  separator line
 *   0.62  question row + answer text  (via renderer_draw_text_px_z)
 */

#define MODAL_X       6
#define MODAL_Y       6
#define MODAL_W       308
#define MODAL_H       228
#define MODAL_PAD     10
#define INNER_X       (MODAL_X + MODAL_PAD)
#define INNER_W       (MODAL_W - 2 * MODAL_PAD)

#define Q_Y           (MODAL_Y + 8)              /* y=14 */
#define SEP_Y         (Q_Y + 14)                 /* y=28 */
#define ANSWER_Y      (SEP_Y + 4)                /* y=32 */
#define ANSWER_BOTTOM (MODAL_Y + MODAL_H - MODAL_PAD)    /* y=224 */
#define LINE_H        13
#define MAX_ANSWER_LINES 14

#define FADE_IN_FRAMES   30      /* 0.5 s @ 60 fps */
#define FADE_OUT_FRAMES  12      /* 0.2 s */

#define COL_OVERLAY_RGB   0x00000000u   /* black; alpha set per-frame */
#define COL_MODAL_BG      0x24283bff    /* tn-storm a tad lifted */
#define COL_MODAL_BORDER  0x414868ff    /* subtle 1 px border */
#define COL_QUESTION      0xa9b1d6ff    /* dim grey */
#define COL_SEP           0x414868ff
#define COL_ANSWER        0xc0caf5ff    /* tn fg / normal text */
#define COL_HEADER        0xe0af68ff    /* tn yellow / # ## ### lines */
#define COL_CODE          0x7dcfc0ff    /* tn cyan / inline code + fenced */
#define COL_BULLET        0x9ca0b3ff    /* tn dim grey / leading • prefix */

#define Q_BUF_CAP   384
#define A_BUF_CAP   4096
/* The transformed markdown can grow slightly: each "- " bullet expands
 * to "• " (3 bytes) but each `…`/`**`/etc. shrinks the source.  Allow
 * 1.5 × the raw answer cap as a safe upper bound. */
#define DOC_BUF_CAP (A_BUF_CAP * 3 / 2)

/* Per-byte style attribute looked up at render time to colour each
 * codepoint.  All bytes of a UTF-8 multibyte sequence share the same
 * style (we set them in lock-step inside the transform). */
typedef enum {
    STY_NORMAL = 0,
    STY_HEADER,
    STY_CODE,
    STY_BULLET,
} md_style_t;

typedef struct {
    char         text[DOC_BUF_CAP];   /* markdown-stripped UTF-8 */
    uint8_t      style[DOC_BUF_CAP];  /* parallel — one md_style_t per byte */
    int          len;
} md_doc_t;

typedef enum {
    PHASE_CLOSED = 0,
    PHASE_FADE_IN,
    PHASE_OPEN,
    PHASE_FADE_OUT,
} modal_phase_t;

struct ai_modal_t {
    modal_phase_t phase;
    int           frame;          /* in current phase */
    char          q[Q_BUF_CAP];
    md_doc_t      doc;            /* answer post markdown transform */
};

/* ── tiny UTF-8 walk + width measurement (modal-local) ──────────── */

/* Decode one codepoint, return byte length consumed (≥1).  *cp_out is
 * U+FFFD on malformed input (and we still advance 1 byte to make
 * progress). */
static int u8_next(const char *p, uint32_t *cp_out) {
    unsigned char b0 = (unsigned char)p[0];
    if (b0 < 0x80) { *cp_out = b0; return 1; }
    if (b0 < 0xC0) { *cp_out = 0xFFFD; return 1; }
    int n;
    uint32_t cp;
    if      (b0 < 0xE0) { n = 2; cp = b0 & 0x1F; }
    else if (b0 < 0xF0) { n = 3; cp = b0 & 0x0F; }
    else                { n = 4; cp = b0 & 0x07; }
    for (int i = 1; i < n; i++) {
        unsigned char b = (unsigned char)p[i];
        if ((b & 0xC0) != 0x80) { *cp_out = 0xFFFD; return 1; }
        cp = (cp << 6) | (b & 0x3F);
    }
    *cp_out = cp;
    return n;
}

/* Same wide-cell rule as font_is_wide() in font_atlas.c.  Inlined here
 * so ai_modal.c doesn't depend on font_atlas internals. */
static int cp_is_wide(uint32_t cp) {
    if (cp >= 0x3000 && cp <= 0x9FFF) return 1;
    if (cp >= 0xAC00 && cp <= 0xD7FF) return 1;
    if (cp >= 0xFF01 && cp <= 0xFF60) return 1;
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return 1;
    return 0;
}

static int cp_pixel_width(uint32_t cp) {
    return cp_is_wide(cp) ? 12 : 6;
}

/* ── alpha multiply helper ──────────────────────────────────────── */

/* Multiply the alpha channel of an RGBA uint32 by `mul01` ∈ [0..256].
 * 256 = unchanged; 0 = fully transparent. */
static uint32_t alpha_scale(uint32_t rgba, int mul01) {
    if (mul01 >= 256) return rgba;
    if (mul01 <= 0)   return rgba & 0xFFFFFF00u;
    uint32_t a = rgba & 0xFFu;
    a = (a * (uint32_t)mul01) >> 8;
    return (rgba & 0xFFFFFF00u) | (a & 0xFFu);
}

/* ── current animation alpha ────────────────────────────────────── */

static int current_alpha_x256(const ai_modal_t *m) {
    switch (m->phase) {
        case PHASE_CLOSED:    return 0;
        case PHASE_FADE_IN: {
            /* Ease-out:  α = 1 - (1 - t)² */
            int f = m->frame;
            if (f >= FADE_IN_FRAMES) return 256;
            int t = (f * 256) / FADE_IN_FRAMES;
            int u = 256 - t;
            int eased = 256 - ((u * u) >> 8);
            return eased;
        }
        case PHASE_OPEN:      return 256;
        case PHASE_FADE_OUT: {
            int f = m->frame;
            if (f >= FADE_OUT_FRAMES) return 0;
            return 256 - ((f * 256) / FADE_OUT_FRAMES);
        }
    }
    return 0;
}

/* ── Markdown → (text, style) transform ─────────────────────────────
 *
 * Walks the DeepSeek answer line by line; emits a flat UTF-8 byte
 * stream into doc.text plus a parallel style byte per output byte.
 * The renderer below picks colour by looking up doc.style[i] at byte
 * offset i.  Multi-byte UTF-8 codepoints get the same style on all
 * their bytes (we copy them as a unit).
 *
 * Supported markdown forms:
 *   #/##/### + space + content    → STY_HEADER for the whole line
 *   - / *  + space + content      → "• " prefix (STY_BULLET) + content
 *   ``` line                       → toggles STY_CODE for following lines
 *   `inline code`                  → strip backticks, STY_CODE for content
 *   **bold** / *italic*            → strip asterisks, STY_NORMAL content
 *   ~~strike~~                     → strip ~~, STY_NORMAL content
 *   [text](url)                    → emit text only, drop (url)
 * Anything else passes through verbatim.
 */

static void md_emit_byte(md_doc_t *d, char b, md_style_t s) {
    if (d->len < (int)sizeof(d->text) - 1) {
        d->text[d->len]  = b;
        d->style[d->len] = (uint8_t)s;
        d->len++;
    }
}

static void md_emit_run(md_doc_t *d, const char *p, int n, md_style_t s) {
    for (int i = 0; i < n; i++) md_emit_byte(d, p[i], s);
}

/* Bullet glyph U+2022 in UTF-8: 0xE2 0x80 0xA2.  Followed by a space.
 * Both styled STY_BULLET so the • appears in the dim accent colour
 * regardless of the bullet's content style. */
static void md_emit_bullet_prefix(md_doc_t *d) {
    md_emit_byte(d, (char)0xE2, STY_BULLET);
    md_emit_byte(d, (char)0x80, STY_BULLET);
    md_emit_byte(d, (char)0xA2, STY_BULLET);
    md_emit_byte(d, ' ',         STY_BULLET);
}

/* Walk a single line's content (already stripped of any block-level
 * prefix like #+ or -+) and emit it with inline markdown applied.
 * `base_style` is the line-level style (STY_NORMAL or STY_HEADER for
 * lines under a `#` heading; STY_CODE if we're inside a ``` block).  */
static void md_emit_inline(md_doc_t *d, const char *p, int len,
                           md_style_t base_style) {
    int in_code = 0;     /* toggled by ` */
    int i = 0;
    while (i < len) {
        char c = p[i];

        /* Inline code: ` toggle.  Don't mix with block-level STY_CODE
         * (we leave that to fenced blocks). */
        if (c == '`') {
            in_code = !in_code;
            i++;
            continue;
        }

        /* Bold / italic — just strip syntax, keep content with current
         * (in_code-aware) style. */
        if (c == '*') {
            if (i + 1 < len && p[i + 1] == '*') i += 2;  /* ** */
            else                                  i += 1;  /* *  */
            continue;
        }
        /* Strikethrough ~~ — strip without styling. */
        if (c == '~' && i + 1 < len && p[i + 1] == '~') {
            i += 2;
            continue;
        }
        /* Markdown link [text](url) — render text only, drop url. */
        if (c == '[') {
            int close_bracket = -1;
            for (int j = i + 1; j < len; j++) {
                if (p[j] == ']') { close_bracket = j; break; }
            }
            if (close_bracket > 0 && close_bracket + 1 < len &&
                p[close_bracket + 1] == '(') {
                int close_paren = -1;
                for (int j = close_bracket + 2; j < len; j++) {
                    if (p[j] == ')') { close_paren = j; break; }
                }
                if (close_paren > 0) {
                    md_style_t s = in_code ? STY_CODE : base_style;
                    md_emit_run(d, p + i + 1, close_bracket - i - 1, s);
                    i = close_paren + 1;
                    continue;
                }
            }
            /* Not a real link — pass `[` through. */
        }

        md_style_t s = in_code ? STY_CODE : base_style;
        md_emit_byte(d, c, s);
        i++;
    }
}

static void md_transform(const char *src, md_doc_t *out) {
    out->len = 0;
    if (!src) return;

    int in_code_block = 0;
    const char *p = src;

    while (*p) {
        const char *line_end = p;
        while (*line_end && *line_end != '\n') line_end++;
        int line_len = (int)(line_end - p);

        /* Fenced code: a line whose only non-space content is ``` toggles
         * the in_code_block flag and is itself NOT rendered. */
        int strip_pos = 0;
        while (strip_pos < line_len && (p[strip_pos] == ' ' || p[strip_pos] == '\t'))
            strip_pos++;
        if (line_len - strip_pos >= 3 &&
            p[strip_pos] == '`' && p[strip_pos + 1] == '`' && p[strip_pos + 2] == '`') {
            in_code_block = !in_code_block;
            /* Skip this line entirely. */
            p = (*line_end == '\n') ? line_end + 1 : line_end;
            continue;
        }

        if (in_code_block) {
            /* Whole line is code-styled, no inline markdown. */
            md_emit_run(out, p, line_len, STY_CODE);
        } else {
            /* Detect header: 1-3 leading # then space. */
            int hdr = 0;
            while (hdr < 3 && hdr < line_len && p[hdr] == '#') hdr++;
            if (hdr > 0 && hdr < line_len && p[hdr] == ' ') {
                md_emit_inline(out, p + hdr + 1, line_len - hdr - 1, STY_HEADER);
            } else {
                /* Detect bullet: leading "- " or "* " (after optional spaces). */
                int b = 0;
                while (b < line_len && (p[b] == ' ' || p[b] == '\t')) b++;
                if (b + 1 < line_len && (p[b] == '-' || p[b] == '*') && p[b + 1] == ' ') {
                    /* Preserve any leading indent as-is, then the • prefix. */
                    md_emit_run(out, p, b, STY_NORMAL);
                    md_emit_bullet_prefix(out);
                    md_emit_inline(out, p + b + 2, line_len - b - 2, STY_NORMAL);
                } else {
                    md_emit_inline(out, p, line_len, STY_NORMAL);
                }
            }
        }

        md_emit_byte(out, '\n', STY_NORMAL);
        p = (*line_end == '\n') ? line_end + 1 : line_end;
    }
}

static uint32_t color_for_style(md_style_t s) {
    switch (s) {
        case STY_HEADER:  return COL_HEADER;
        case STY_CODE:    return COL_CODE;
        case STY_BULLET:  return COL_BULLET;
        default:          return COL_ANSWER;
    }
}

/* ── public API ─────────────────────────────────────────────────── */

ai_modal_t *ai_modal_init(void) {
    ai_modal_t *m = (ai_modal_t *)calloc(1, sizeof(*m));
    return m;
}

void ai_modal_free(ai_modal_t *m) { free(m); }

void ai_modal_open(ai_modal_t *m, const char *question, const char *answer) {
    if (!m) return;
    snprintf(m->q, sizeof(m->q), "%s", question ? question : "");
    md_transform(answer, &m->doc);
    m->phase = PHASE_FADE_IN;
    m->frame = 0;
}

void ai_modal_close(ai_modal_t *m) {
    if (!m) return;
    if (m->phase == PHASE_CLOSED || m->phase == PHASE_FADE_OUT) return;
    m->phase = PHASE_FADE_OUT;
    m->frame = 0;
}

void ai_modal_tick(ai_modal_t *m) {
    if (!m) return;
    switch (m->phase) {
        case PHASE_CLOSED:
            break;
        case PHASE_FADE_IN:
            m->frame++;
            if (m->frame >= FADE_IN_FRAMES) {
                m->phase = PHASE_OPEN;
                m->frame = 0;
            }
            break;
        case PHASE_OPEN:
            m->frame++;
            break;
        case PHASE_FADE_OUT:
            m->frame++;
            if (m->frame >= FADE_OUT_FRAMES) {
                m->phase = PHASE_CLOSED;
                m->frame = 0;
            }
            break;
    }
}

int ai_modal_visible(const ai_modal_t *m) {
    return m && m->phase != PHASE_CLOSED;
}

/* Modal text z — must be HIGHER than any modal-bg layer (0.56) so the
 * glyphs render on top of the modal frame instead of getting depth-
 * test-occluded by it. */
#define MODAL_TEXT_Z 0.62f

/* Render the markdown-transformed doc inside the answer area, applying
 * per-byte style colours.  Walks codepoint-by-codepoint, hard-wrapping
 * at INNER_W and yielding one rendered codepoint at a time so we can
 * mix colours within a single visual row (e.g. "press . to toggle"
 * where `.` is highlighted as inline code).
 *
 * Returns 1 if the doc was truncated (didn't fit in the answer area)
 * so the caller can append a "..." indicator. */
static int draw_md_doc(const md_doc_t *doc, int alpha_x256) {
    int x = INNER_X;
    int y = ANSWER_Y;

    int i = 0;
    while (i < doc->len) {
        char c = doc->text[i];

        /* Hard line break — newline in the source. */
        if (c == '\n') {
            x = INNER_X;
            y += LINE_H;
            if (y + 12 > ANSWER_BOTTOM) return 1;   /* truncated */
            i++;
            continue;
        }

        /* Decode one UTF-8 codepoint to know its visual width. */
        uint32_t cp;
        int adv = u8_next(doc->text + i, &cp);
        int cw  = cp_pixel_width(cp);

        /* Soft wrap — codepoint won't fit on the current visual row. */
        if (x + cw > INNER_X + INNER_W) {
            x = INNER_X;
            y += LINE_H;
            if (y + 12 > ANSWER_BOTTOM) return 1;
        }

        /* Render the single codepoint with the colour mapped from its
         * (per-byte) style attribute, alpha-modulated by the modal's
         * current animation phase.  We have to do this per codepoint
         * because adjacent bytes can have different styles (e.g. the
         * boundary of inline code).  Performance-wise, on a typical
         * answer ~600 codepoints × one draw call each is well within
         * citro2d's per-frame budget. */
        md_style_t s = (md_style_t)doc->style[i];
        uint32_t rgba = alpha_scale(color_for_style(s), alpha_x256);

        /* Build a tiny NUL-terminated string for renderer_draw_text_px_z.
         * adv is at most 4 bytes for a UTF-8 codepoint, so 8 is plenty. */
        char one[8];
        int n = adv;
        if (n > 4) n = 4;
        memcpy(one, doc->text + i, (size_t)n);
        one[n] = 0;
        renderer_draw_text_px_z(x, y, MODAL_TEXT_Z, one, rgba);

        x += cw;
        i += adv;
    }
    return 0;
}

/* Draw the question row, applying ellipsis if the rendered width
 * would overflow.  Returns nothing — best-effort. */
static void draw_question_with_ellipsis(int x, int y, const char *q,
                                        int max_w, uint32_t rgba) {
    char buf[Q_BUF_CAP + 4];
    int o = 0;
    int w = 0;
    int ellipsis_w = 3 * 6;  /* "..." = 3 ASCII chars × 6 px */
    const char *p = q;
    /* First pass: append while fits. */
    while (*p && o < (int)sizeof(buf) - 4) {
        uint32_t cp;
        int adv = u8_next(p, &cp);
        int cw = cp_pixel_width(cp);
        if (w + cw + ellipsis_w > max_w) {
            /* Need to truncate — append "..." and stop. */
            if (o + 3 < (int)sizeof(buf)) {
                buf[o++] = '.'; buf[o++] = '.'; buf[o++] = '.';
            }
            buf[o] = 0;
            renderer_draw_text_px_z(x, y, MODAL_TEXT_Z, buf, rgba);
            return;
        }
        if (o + adv >= (int)sizeof(buf)) break;
        memcpy(buf + o, p, (size_t)adv);
        o += adv;
        p += adv;
        w += cw;
    }
    buf[o] = 0;
    renderer_draw_text_px_z(x, y, MODAL_TEXT_Z, buf, rgba);
}

void ai_modal_draw(ai_modal_t *m) {
    if (!m || m->phase == PHASE_CLOSED) return;
    int alpha_x256 = current_alpha_x256(m);
    if (alpha_x256 <= 0) return;

    /* 1. Outside-modal dimming overlay covering the full bottom screen. */
    {
        /* Peak overlay = 60 % black; scaled by current animation alpha. */
        uint32_t base = COL_OVERLAY_RGB | 0x99u;       /* 0x99 ≈ 60 % */
        uint32_t col  = alpha_scale(base, alpha_x256);
        C2D_DrawRectSolid(0, 0, 0.55f, 320, 240, C2D_Color32(
            (col >> 24) & 0xff, (col >> 16) & 0xff,
            (col >>  8) & 0xff,  col        & 0xff));
    }

    /* 2. Modal body. */
    {
        uint32_t col = alpha_scale(COL_MODAL_BG, alpha_x256);
        C2D_DrawRectSolid(MODAL_X, MODAL_Y, 0.56f, MODAL_W, MODAL_H,
            C2D_Color32((col >> 24) & 0xff, (col >> 16) & 0xff,
                        (col >>  8) & 0xff,  col        & 0xff));
    }

    /* 3. Subtle 1 px border on top + bottom edges (corners stay clipped
     * because we don't have rounded corners in v1 — pretend they're
     * there via the gap from the screen edges). */
    {
        uint32_t col = alpha_scale(COL_MODAL_BORDER, alpha_x256);
        u32 c2 = C2D_Color32((col >> 24) & 0xff, (col >> 16) & 0xff,
                             (col >>  8) & 0xff,  col        & 0xff);
        C2D_DrawRectSolid(MODAL_X, MODAL_Y, 0.57f, MODAL_W, 1, c2);
        C2D_DrawRectSolid(MODAL_X, MODAL_Y + MODAL_H - 1, 0.57f, MODAL_W, 1, c2);
        C2D_DrawRectSolid(MODAL_X, MODAL_Y, 0.57f, 1, MODAL_H, c2);
        C2D_DrawRectSolid(MODAL_X + MODAL_W - 1, MODAL_Y, 0.57f, 1, MODAL_H, c2);
    }

    /* 4. Question row (single line + ellipsis).  Drawn at z=0.62 so it
     * sits ABOVE the modal bg (0.56) — using the default z=0.5 in
     * renderer_draw_text_px would put it BELOW the bg and disappear. */
    {
        uint32_t col = alpha_scale(COL_QUESTION, alpha_x256);
        draw_question_with_ellipsis(INNER_X, Q_Y, m->q, INNER_W, col);
    }

    /* 5. Separator line. */
    {
        uint32_t col = alpha_scale(COL_SEP, alpha_x256);
        u32 c2 = C2D_Color32((col >> 24) & 0xff, (col >> 16) & 0xff,
                             (col >>  8) & 0xff,  col        & 0xff);
        C2D_DrawRectSolid(INNER_X, SEP_Y, 0.60f, INNER_W, 1, c2);
    }

    /* 6. Answer — markdown-styled walk of m->doc.  draw_md_doc handles
     * its own wrapping + per-codepoint colour from the style table. */
    {
        int truncated = draw_md_doc(&m->doc, alpha_x256);
        if (truncated) {
            uint32_t col = alpha_scale(COL_ANSWER, alpha_x256);
            renderer_draw_text_px_z(INNER_X + INNER_W - 18,
                                    ANSWER_BOTTOM - LINE_H,
                                    MODAL_TEXT_Z, "...", col);
        }
    }
}
