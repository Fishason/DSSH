#include "mascot.h"
#include <citro2d.h>
#include <stdlib.h>
#include <stdint.h>

/*
 * Front-facing chibi crab — banana-cat sway gait.
 *
 * Sprite: 18 px wide × 11 px tall.  Rounded oval body, two simple
 * 2×2 black-square eyes, two small foot stubs at the bottom.
 *
 * Walk: 8-frame pendulum sway around the anchored feet.  Body rocks
 * left → return → right → return repeatedly; the upper rows shift
 * more horizontally than the lower rows (row-by-row shear), and the
 * foot row stays planted.  At each tilt peak the unweighted foot
 * nudges up 1 px; the body itself rises 1 px on its way through the
 * apex.  The result reads as a "duang duang" toddle, the same beat
 * as the banana-cat reference GIF.
 *
 * State machine unchanged from prior versions:
 *   WALK    sway + horizontal traversal between [x_min, x_max].
 *   IDLE    sway frozen at neutral; small idle bob.
 *   FLEE    sway + faster horizontal speed away from a touch.
 *   ALERT   sway frozen, red ✕ floats above the body.
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
    int   anim_frame;     /* 0..7 sway cycle index */
    int   anim_timer;
    int   bob_phase;
    int   alert_phase;
};

#define CRAB_W   18
#define CRAB_H   11
#define HIT_PAD  4

#define COL_BODY  0xe89b89ff
#define COL_FOOT  0xd88271ff
#define COL_EYE   0x000000ff
#define COL_X     0xe54040ff

/* Body rows 0-9.  Row 10 is the feet, animated separately.
 * Char palette: '@' body, '#' eye, '.' transparent. */
static const char *const crab_body[10] = {
    "......@@@@@@......",   /* 0  top arc */
    "....@@@@@@@@@@....",   /* 1  */
    "..@@@@@@@@@@@@@@..",   /* 2  wide */
    ".@@@@##@@@@##@@@@.",   /* 3  eyes top */
    ".@@@@##@@@@##@@@@.",   /* 4  eyes bottom */
    "@@@@@@@@@@@@@@@@@@",   /* 5  widest */
    "@@@@@@@@@@@@@@@@@@",   /* 6  */
    ".@@@@@@@@@@@@@@@@.",   /* 7  narrowing */
    "..@@@@@@@@@@@@@@..",   /* 8  */
    "...@@@@@@@@@@@@...",   /* 9  bottom curve */
};

/* Foot row — left foot at cols 3-5, right foot at cols 12-14. */
static const char *const crab_feet =
    "...FFF......FFF...";

/* Sway tilt sequence — 8 frames going neutral → left peak → return
 * → right peak → return.  Body bob is -1 px when |tilt| == 2. */
static const int8_t sway_tilts[8] = { 0, -1, -2, -1, 0, +1, +2, +1 };

/* Pre-computed row-shear tables: shear[tilt+2][row] = horizontal
 * pixel offset for that body row.  Bottom row (9) is anchored at 0,
 * top row (0) gets the full tilt amount.  Generated as
 * round(tilt * (9-row) / 9.0). */
static const int8_t shear_dx[5][10] = {
    { -2,-2,-2,-1,-1,-1,-1, 0, 0, 0 },   /* tilt = -2 */
    { -1,-1,-1,-1,-1, 0, 0, 0, 0, 0 },   /* tilt = -1 */
    {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },   /* tilt =  0 */
    {  1, 1, 1, 1, 1, 0, 0, 0, 0, 0 },   /* tilt = +1 */
    {  2, 2, 2, 1, 1, 1, 1, 0, 0, 0 },   /* tilt = +2 */
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

    /* Sway frame advances every 6 frames in WALK/FLEE → 8-frame cycle
     * completes in ~0.8 s, the toddle pace from the banana-cat ref. */
    if (m->state == STATE_WALK || m->state == STATE_FLEE) {
        if (++m->anim_timer >= 6) {
            m->anim_timer = 0;
            m->anim_frame = (m->anim_frame + 1) & 7;
        }
    }
    m->bob_phase   = (m->bob_phase + 1) % 60;
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

void mascot_draw(mascot_t *m) {
    if (!m) return;
    int xi = (int)m->fx;
    int yi = m->y_top;

    /* Idle small bob. */
    if (m->state == STATE_IDLE && ((m->bob_phase / 8) & 1)) yi -= 1;

    /* Sway state — only WALK/FLEE animate; IDLE/ALERT freeze at tilt 0. */
    int tilt = 0;
    int body_dy = 0;
    int foot_l_dy = 0, foot_r_dy = 0;
    if (m->state == STATE_WALK || m->state == STATE_FLEE) {
        tilt = sway_tilts[m->anim_frame];
        if (tilt == +2 || tilt == -2) body_dy = -1;
        /* Tilt right (+2) → left foot unweighted (lifts 1 px).
         * Tilt left (-2) → right foot unweighted. */
        if (tilt == +2) foot_l_dy = -1;
        if (tilt == -2) foot_r_dy = -1;
    }
    const int8_t *dx_row = shear_dx[tilt + 2];

    u32 body_c = rgba_to_c2d(COL_BODY);
    u32 foot_c = rgba_to_c2d(COL_FOOT);
    u32 eye_c  = rgba_to_c2d(COL_EYE);

    /* Body rows 0-9 — apply per-row shear and shared body_dy. */
    for (int row = 0; row < 10; row++) {
        const char *src = crab_body[row];
        int dx = dx_row[row];
        for (int col = 0; col < CRAB_W; col++) {
            char ch = src[col];
            u32 c = (ch == '@') ? body_c : (ch == '#') ? eye_c : 0;
            if (!c) continue;
            C2D_DrawRectSolid((float)(xi + col + dx),
                              (float)(yi + row + body_dy),
                              0.6f, 1, 1, c);
        }
    }

    /* Foot row — anchored, with separate dy per foot. */
    for (int col = 0; col < CRAB_W; col++) {
        if (crab_feet[col] != 'F') continue;
        int dy;
        if (col >= 3 && col <= 5)         dy = foot_l_dy;
        else if (col >= 12 && col <= 14)  dy = foot_r_dy;
        else                               dy = 0;
        C2D_DrawRectSolid((float)(xi + col),
                          (float)(yi + 10 + dy),
                          0.6f, 1, 1, foot_c);
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
