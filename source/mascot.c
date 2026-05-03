#include "mascot.h"
#include <citro2d.h>
#include <stdlib.h>
#include <stdint.h>

/*
 * Q-style salmon-pink crab — Anthropic Claude vibe at chibi scale.
 *
 * Sprite: 18 px wide × 13 px tall, drawn one C2D_DrawRectSolid per
 * lit pixel (~70 rects/frame, trivial).
 *
 * Compared to the v0.3-polish-3 version this redesign drops the
 * 4-leg horizontal-glide gait for a 2-leg arcing biped walk:
 *
 *   - Two big chunky feet (4-px wide, ~5 px apart) instead of 4 thin
 *     legs.  More cartoon, less centipede.
 *   - 4-frame walk cycle: rest → right foot arcs forward → rest →
 *     left foot arcs.  Lifted foot is drawn one row higher and
 *     slightly forward of the planted foot, the body bobs up 1 px
 *     during each lift — gives the silhouette the "duang duang"
 *     bouncy thump the user wants.
 *   - Q-style 3×3 white-sclera eyes with single black pupil, ~3 sec
 *     blink rhythm preserved from the previous version.
 *   - 3-tone salmon body shading (highlight top / main / shadow
 *     bottom) for a 2.5-D feel.
 *
 * State machine unchanged — see mascot.h.  ALERT freezes the walk
 * cycle, suppresses the blink, and overlays a red ✕ that sways.
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
    int   blink_phase;    /* 0..179, drives eye blinks */
    int   alert_phase;    /* drives ✕ sway when alerting */
};

#define CRAB_W   18
#define CRAB_H   13
#define HIT_PAD  4

/* 3-tone salmon body palette */
#define COL_HL    0xf2bcabff
#define COL_BODY  0xe89b89ff
#define COL_SHD   0xc4756aff
/* Eyes */
#define COL_EYE   0x000000ff
#define COL_SCL   0xffffffff
/* Alert */
#define COL_X     0xe54040ff

/* Body sprite (rows 0-9).  Char palette:
 *   H = highlight   @ = body main   S = shadow
 *   w = sclera      # = pupil       . = transparent
 * Eye rows 3-5 get overridden when blinking. */
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

/* Eye area when blinking — replaces rows 3-5 with a flat body row +
 * a single 2-px black pupil bar at row 4. */
static const char *const crab_body_blink[3] = {
    "@@@@@@@@@@@@@@@@@@",
    "@@@@##@@@@@@##@@@@",
    "@@@@@@@@@@@@@@@@@@",
};

/* 4-frame leg cycle on rows 10-12.  Two thick legs:
 *
 *   left  trunk cols 3-4   foot 2-5 (4-wide)
 *   right trunk cols 13-14 foot 12-15 (4-wide)
 *
 * In the lift frames (1, 3) the airborne foot moves up to row 11
 * and the corresponding shin row 11 is empty — reads as "foot in
 * mid-arc above the ground". */
static const char *const crab_legs[4][3] = {
    /* frame 0: rest, both feet on the ground */
    { "...@@........@@...",
      "...@@........@@...",
      "..@@@@......@@@@.." },
    /* frame 1: right foot lifted and arcing */
    { "...@@........@@...",
      "...@@.......@@@@..",
      "..@@@@............" },
    /* frame 2: rest again — short pause between strides */
    { "...@@........@@...",
      "...@@........@@...",
      "..@@@@......@@@@.." },
    /* frame 3: left foot lifted and arcing */
    { "...@@........@@...",
      "..@@@@.......@@...",
      "............@@@@.." },
};

/* 5×5 red ✕ for ALERT. */
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

    /* Walk cycle: advance every 8 frames in WALK/FLEE.  Slower than
     * the v0.3 version (was 6) so the duang-duang rhythm reads as
     * heavy thumping rather than a quick scuttle. */
    if (m->state == STATE_WALK || m->state == STATE_FLEE) {
        if (++m->anim_timer >= 8) {
            m->anim_timer = 0;
            m->anim_frame = (m->anim_frame + 1) & 3;
        }
    }
    m->bob_phase   = (m->bob_phase + 1) % 60;
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

    /* Idle bob: small 1-px wiggle every ~8 frames. */
    if (m->state == STATE_IDLE && ((m->bob_phase / 8) & 1)) yi -= 1;

    /* Walk bob: body lifts 1 px on the leg-arc frames (1 and 3) so
     * the silhouette pulses up-down-up-down — the "duang duang" beat. */
    int body_dy = 0;
    if ((m->state == STATE_WALK || m->state == STATE_FLEE) &&
        (m->anim_frame & 1)) body_dy = -1;

    int blinking = (m->state != STATE_ALERT) && (m->blink_phase < 6);

    /* Body rows 0-9 — bob with body_dy. */
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

    /* Legs rows 10-12 — anchored at full y_top regardless of body
     * bob.  When the body bobs up 1 px there's a 1-row gap between
     * body bottom and leg top that reads as the body "lifting off
     * the ground".  Lifted foot in this frame is drawn at row 11
     * (one row higher than rest), making the leg look mid-arc. */
    int frame = (m->state == STATE_ALERT || m->state == STATE_IDLE)
              ? 0 : m->anim_frame;
    for (int row = 0; row < 3; row++) {
        const char *src = crab_legs[frame][row];
        for (int col = 0; col < CRAB_W; col++) {
            if (src[col] == '@') {
                C2D_DrawRectSolid((float)(xi + col),
                                  (float)(yi + 10 + row),
                                  0.6f, 1, 1, rgba_to_c2d(COL_BODY));
            }
        }
    }

    /* Red ✕ overlay for ALERT. */
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
