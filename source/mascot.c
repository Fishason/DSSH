#include "mascot.h"
#include <citro2d.h>
#include <stdlib.h>
#include <stdint.h>

/*
 * Q-style salmon-pink crab — Anthropic Claude vibe at chibi scale.
 *
 * Sprite: 18 px wide × 12 px tall, drawn as one C2D_DrawRectSolid call
 * per lit pixel.  Compared to the v0.3.0 sprite we now have:
 *
 *   - 3-tone body shading: a light row at the top, main pink in the
 *     middle, a darker row at the bottom — gives the silhouette a
 *     2.5-D feel (overhead light, ground shadow) without needing
 *     proper 3D rendering at this scale.
 *   - Bigger Q-style eyes: a 3×3 white-sclera block with a single
 *     1-px black pupil dead center.  Reads as "alive" much more than
 *     the previous flat 2×2 black squares.
 *   - Walking bob: anim_frame 1 and 3 raise the body by 1 px, so
 *     between leg lifts the crab visibly bounces.
 *   - Occasional blink: every ~3 seconds the eye rows briefly squash
 *     to a single 2-px black bar (`-`) for ~6 frames, then re-open.
 *
 * State machine unchanged from v0.3.0:
 *   WALK / IDLE / FLEE / ALERT — see mascot.h.
 */

typedef enum {
    STATE_WALK,
    STATE_IDLE,
    STATE_FLEE,
    STATE_ALERT,
} mascot_state_t;

struct mascot_t {
    int   x_min, x_max, y_top;
    float fx;
    int   facing;
    mascot_state_t state;
    mascot_state_t prev_state;
    int   state_frames;
    int   anim_frame;     /* 0..3 walk cycle */
    int   anim_timer;
    int   bob_phase;      /* 0..59, drives idle bob */
    int   blink_phase;    /* 0..179, eye-blink counter */
    int   alert_phase;    /* X waving when alerting */
};

#define CRAB_W   18
#define CRAB_H   12
#define HIT_PAD  4

/* 3-tone salmon body palette */
#define COL_HL    0xf2bcabff   /* highlight (top row + transitions) */
#define COL_BODY  0xe89b89ff   /* main pink */
#define COL_SHD   0xc4756aff   /* shadow (bottom transition) */
/* Eyes */
#define COL_EYE   0x000000ff   /* black pupil */
#define COL_SCL   0xffffffff   /* white sclera */
/* Alert */
#define COL_X     0xe54040ff   /* red ✕ */

/* Body sprite — char palette: H = highlight, @ = body, S = shadow,
 * w = sclera, # = pupil, . = transparent.  Rows 0-9 are body; legs
 * occupy rows 10-11 with frame-dependent patterns below.  Eye rows
 * 3-5 get overridden when blinking. */
static const char *const crab_body_open[10] = {
    "...HHHHHHHHHHHH...",   /* 0  highlight arc */
    ".HHHH@@@@@@@@HHHH.",   /* 1  highlight transition */
    "HHH@@@@@@@@@@@@HHH",   /* 2  transition row */
    "@@@www@@@@@@www@@@",   /* 3  eye top (sclera) */
    "@@@w#w@@@@@@w#w@@@",   /* 4  eye middle (pupil) */
    "@@@www@@@@@@www@@@",   /* 5  eye bottom (sclera) */
    "@@@@@@@@@@@@@@@@@@",   /* 6  widest body */
    "SSS@@@@@@@@@@@@SSS",   /* 7  shadow transition */
    "SSSSSSSSSSSSSSSSSS",   /* 8  shadow band */
    "...SSSSSSSSSSSS...",   /* 9  bottom arc */
};

/* Eye area when blinking — replaces rows 3-5.  Eyes squash to a single
 * 2-px-wide black bar on row 4 (the pupil row), giving a `-` look. */
static const char *const crab_body_blink[3] = {
    "@@@@@@@@@@@@@@@@@@",   /* row 3 — sclera covered by body */
    "@@@@##@@@@@@##@@@@",   /* row 4 — pupil-only blink line */
    "@@@@@@@@@@@@@@@@@@",   /* row 5 — sclera covered by body */
};

/* 4-frame leg cycle on rows 10-11.  Four legs at cols 2-3, 6-7, 11-12,
 * 15-16; rest pose has all 4 touching ground.  Frames 1/3 lift
 * alternate pairs by removing the lower row only. */
static const char *const crab_legs[4][2] = {
    /* frame 0: rest, all 4 down */
    { "..@@..@@...@@..@@.",
      "..@@..@@...@@..@@." },
    /* frame 1: legs 1 and 3 lifted */
    { "..@@..@@...@@..@@.",
      "......@@.......@@." },
    /* frame 2: rest */
    { "..@@..@@...@@..@@.",
      "..@@..@@...@@..@@." },
    /* frame 3: legs 2 and 4 lifted */
    { "..@@..@@...@@..@@.",
      "..@@.......@@....." },
};

/* 5×5 red ✕ for ALERT.  Drawn above the body, swaying ±1 px. */
static const char *const alert_x[5] = {
    "@...@",
    ".@.@.",
    "..@..",
    ".@.@.",
    "@...@",
};

static u32 rgba_to_c2d(uint32_t rgba) {
    return C2D_Color32((rgba >> 24) & 0xff,
                       (rgba >> 16) & 0xff,
                       (rgba >>  8) & 0xff,
                        rgba        & 0xff);
}

static void enter_walk(mascot_t *m) {
    m->state = STATE_WALK;
    m->state_frames = 0;
}
static void enter_idle(mascot_t *m) {
    m->state = STATE_IDLE;
    m->state_frames = 60 + (rand() % 120);
}
static void enter_flee(mascot_t *m) {
    m->state = STATE_FLEE;
    m->state_frames = 50 + (rand() % 30);
}

mascot_t *mascot_init(int x_min, int x_max, int y) {
    mascot_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->x_min  = x_min;
    m->x_max  = x_max;
    m->y_top  = y;
    m->fx     = (float)x_min + 4.0f;
    m->facing = +1;
    enter_walk(m);
    return m;
}

void mascot_free(mascot_t *m) { free(m); }

void mascot_set_alert(mascot_t *m, int alert) {
    if (!m) return;
    if (alert && m->state != STATE_ALERT) {
        m->prev_state   = m->state;
        m->state        = STATE_ALERT;
        m->alert_phase  = 0;
    } else if (!alert && m->state == STATE_ALERT) {
        enter_walk(m);
    }
}

static void clamp_and_bounce(mascot_t *m, int *hit_wall) {
    *hit_wall = 0;
    if (m->fx <= (float)m->x_min) {
        m->fx = (float)m->x_min;
        m->facing = +1;
        *hit_wall = 1;
    }
    if (m->fx >= (float)(m->x_max - CRAB_W)) {
        m->fx = (float)(m->x_max - CRAB_W);
        m->facing = -1;
        *hit_wall = 1;
    }
}

void mascot_update(mascot_t *m) {
    if (!m) return;

    /* 4-frame walk cycle: advance every 6 frames in WALK/FLEE. */
    if (m->state == STATE_WALK || m->state == STATE_FLEE) {
        if (++m->anim_timer >= 6) {
            m->anim_timer = 0;
            m->anim_frame = (m->anim_frame + 1) & 3;
        }
    }
    m->bob_phase   = (m->bob_phase + 1) % 60;
    /* Blink runs in any non-alert state — even when idle/fleeing the
     * crab still occasionally blinks.  Period 180 = 3 seconds at 60 fps;
     * the eye is closed for the first 6 frames of each cycle (~100 ms). */
    if (m->state != STATE_ALERT) {
        m->blink_phase = (m->blink_phase + 1) % 180;
    }
    m->alert_phase = (m->alert_phase + 1) % 24;

    int hit_wall = 0;
    switch (m->state) {
        case STATE_WALK:
            m->fx += 0.5f * (float)m->facing;
            clamp_and_bounce(m, &hit_wall);
            if ((rand() % 600) == 0) enter_idle(m);
            break;

        case STATE_IDLE:
            if (m->state_frames > 0) m->state_frames--;
            if (m->state_frames == 0) enter_walk(m);
            break;

        case STATE_FLEE:
            m->fx += 2.0f * (float)m->facing;
            clamp_and_bounce(m, &hit_wall);
            if (m->state_frames > 0) m->state_frames--;
            if (m->state_frames == 0 || hit_wall) enter_walk(m);
            break;

        case STATE_ALERT:
            break;
    }
}

/* Map a sprite character to its color, or 0 for transparent. */
static uint32_t color_for_char(char ch) {
    switch (ch) {
        case 'H': return COL_HL;
        case '@': return COL_BODY;
        case 'S': return COL_SHD;
        case 'w': return COL_SCL;
        case '#': return COL_EYE;
        default:  return 0;
    }
}

void mascot_draw(mascot_t *m) {
    if (!m) return;
    int xi = (int)m->fx;
    int yi = m->y_top;

    /* Idle bob: small 1-px vertical wiggle every ~8 frames. */
    if (m->state == STATE_IDLE && ((m->bob_phase / 8) & 1)) yi -= 1;
    /* Walk bob: anim_frame 1 and 3 lift the body 1 px (puts a
     * spring-step rhythm in the silhouette). */
    int body_dy = 0;
    if ((m->state == STATE_WALK || m->state == STATE_FLEE) &&
        (m->anim_frame & 1)) body_dy = -1;

    /* Choose eye art — closed for the first 6 frames of each blink
     * cycle, otherwise the open 3-row sclera+pupil. */
    int blinking = (m->state != STATE_ALERT) && (m->blink_phase < 6);

    /* Body rows 0-9.  Rows 3-5 either show open eyes or the blink line
     * depending on `blinking`. */
    for (int row = 0; row < 10; row++) {
        const char *src = crab_body_open[row];
        if (blinking && row >= 3 && row <= 5)
            src = crab_body_blink[row - 3];
        for (int col = 0; col < CRAB_W; col++) {
            uint32_t c = color_for_char(src[col]);
            if (c) {
                C2D_DrawRectSolid((float)(xi + col),
                                  (float)(yi + row + body_dy),
                                  0.6f, 1, 1, rgba_to_c2d(c));
            }
        }
    }

    /* Legs (rows 10-11).  Stay anchored to the ground (no body_dy)
     * during the walk cycle so the body bobs up and the feet stay
     * planted — gives the cleanest "stride" look at this resolution. */
    int frame = (m->state == STATE_ALERT || m->state == STATE_IDLE)
              ? 0 : m->anim_frame;
    for (int row = 0; row < 2; row++) {
        const char *src = crab_legs[frame][row];
        for (int col = 0; col < CRAB_W; col++) {
            if (src[col] == '@') {
                C2D_DrawRectSolid((float)(xi + col),
                                  (float)(yi + 10 + row),
                                  0.6f, 1, 1, rgba_to_c2d(COL_BODY));
            }
        }
    }

    /* Red ✕ overlay for ALERT — the warning sign the crab waves to
     * tell the user the network is unresponsive. */
    if (m->state == STATE_ALERT) {
        u32 x_c = rgba_to_c2d(COL_X);
        int sway = ((m->alert_phase / 12) & 1) ? 1 : -1;
        int x_x  = xi + (CRAB_W - 5) / 2 + sway;
        int x_y  = yi - 6;
        for (int row = 0; row < 5; row++) {
            const char *src = alert_x[row];
            for (int col = 0; col < 5; col++) {
                if (src[col] == '@') {
                    C2D_DrawRectSolid((float)(x_x + col),
                                      (float)(x_y + row),
                                      0.7f, 1, 1, x_c);
                }
            }
        }
    }
}

int mascot_hit_test(const mascot_t *m, int tx, int ty) {
    if (!m) return 0;
    int xi = (int)m->fx;
    int yi = m->y_top;
    return tx >= xi - HIT_PAD && tx < xi + CRAB_W + HIT_PAD &&
           ty >= yi - HIT_PAD && ty < yi + CRAB_H + HIT_PAD;
}

void mascot_on_touched(mascot_t *m, int from_tx) {
    if (!m) return;
    if (m->state == STATE_ALERT) return;
    int center = (int)m->fx + CRAB_W / 2;
    m->facing = (from_tx < center) ? +1 : -1;
    enter_flee(m);
}
