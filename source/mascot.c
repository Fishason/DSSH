#include "mascot.h"
#include <citro2d.h>
#include <stdlib.h>
#include <stdint.h>

/*
 * Salmon-pink crab mascot — Anthropic-Claude style.
 *
 * Sprite: 18 px wide × 11 px tall, drawn as one C2D_DrawRectSolid call
 * per lit pixel (~50-70 rects/frame, trivial vs the terminal grid).
 *
 * Body shape is a squat rounded rectangle with two black-square eyes
 * inset near the top.  Legs are 4 short stubs at the bottom that
 * animate through a 4-frame walk cycle for a "scuttle" rhythm —
 * frame 0/2 = rest, frame 1 lifts legs 1&3, frame 3 lifts legs 2&4.
 *
 * State machine:
 *   WALK    horizontal traversal between [x_min, x_max - W], 0.5 px/f.
 *   IDLE    randomly entered from WALK; bobs in place 1-3 sec.
 *   FLEE    triggered by mascot_on_touched — runs opposite the touch
 *           at 2 px/f for ~1 sec, or until a wall stops it.
 *   ALERT   set by main.c when the SSH connection has stalled (no
 *           bytes received for STALL_THRESHOLD seconds, including
 *           libssh2 keepalive replies).  Crab stops dead and holds
 *           up a red ✕, waving it gently until alert clears.
 */

typedef enum {
    STATE_WALK,
    STATE_IDLE,
    STATE_FLEE,
    STATE_ALERT,
} mascot_state_t;

struct mascot_t {
    int   x_min, x_max, y_top;
    float fx;             /* sub-pixel x */
    int   facing;         /* -1 left, +1 right */
    mascot_state_t state;
    mascot_state_t prev_state;  /* state to return to after ALERT clears */
    int   state_frames;
    int   anim_frame;     /* 0..3 leg cycle */
    int   anim_timer;
    int   bob_phase;
    int   alert_phase;    /* used by ALERT for X waving */
};

#define CRAB_W   18
#define CRAB_H   11
#define HIT_PAD  4

#define COL_BODY  0xe89b89ff   /* salmon pink — Anthropic-ish */
#define COL_EYE   0x000000ff   /* black square eyes */
#define COL_X     0xe54040ff   /* red ✕ for the alert sign */

/* Rows 0-8 are the static body.  Rows 9-10 are the legs and have
 * 4 different patterns selected by anim_frame.  '@' = body fill,
 * '#' = eye black, ' ' = transparent. */
static const char *const crab_body[9] = {
    "..@@@@@@@@@@@@@@..",   /* 0  top arc */
    ".@@@@@@@@@@@@@@@@.",   /* 1  */
    "@@@@@@@@@@@@@@@@@@",   /* 2  widest */
    "@@@##@@@@@@@@##@@@",   /* 3  eye row 1 */
    "@@@##@@@@@@@@##@@@",   /* 4  eye row 2 */
    "@@@@@@@@@@@@@@@@@@",   /* 5  */
    "@@@@@@@@@@@@@@@@@@",   /* 6  */
    ".@@@@@@@@@@@@@@@@.",   /* 7  bottom narrows */
    "..@@@@@@@@@@@@@@..",   /* 8  */
};

/* 4-frame walk cycle for the bottom 2 rows (legs).  All 4 legs sit at
 * cols 2-3, 7-8, 11-12, 15-16 in the rest pattern.  Frames 1 and 3
 * lift alternate pairs of legs by removing their tip row to simulate
 * the foot leaving the ground. */
static const char *const crab_legs[4][2] = {
    /* frame 0: all 4 down (rest pose) */
    { "..@@..@@..@@..@@..",
      "..@@..@@..@@..@@.." },
    /* frame 1: legs 1 and 3 lifted */
    { "..@@..@@..@@..@@..",
      "......@@......@@.." },
    /* frame 2: rest again — gives a "step beat" between lift phases */
    { "..@@..@@..@@..@@..",
      "..@@..@@..@@..@@.." },
    /* frame 3: legs 2 and 4 lifted */
    { "..@@..@@..@@..@@..",
      "..@@......@@......" },
};

/* 5×5 red ✕ for the ALERT state.  Drawn floating above the crab's
 * right shoulder, waving ±1 px every ~12 frames. */
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
        /* Remember the pre-alert state so we can resume cleanly. */
        m->prev_state   = m->state;
        m->state        = STATE_ALERT;
        m->alert_phase  = 0;
    } else if (!alert && m->state == STATE_ALERT) {
        /* Recover to walking — even if pre-alert was IDLE/FLEE, those
         * are short-lived and walk is the safe default. */
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

    /* 4-frame walk cycle: advance every 6 frames in WALK/FLEE.  IDLE
     * and ALERT freeze the leg animation. */
    if (m->state == STATE_WALK || m->state == STATE_FLEE) {
        if (++m->anim_timer >= 6) {
            m->anim_timer = 0;
            m->anim_frame = (m->anim_frame + 1) & 3;
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
            /* Frozen.  Cleared only by mascot_set_alert(m, 0). */
            break;
    }
}

void mascot_draw(mascot_t *m) {
    if (!m) return;
    int xi = (int)m->fx;
    int yi = m->y_top;
    /* Idle: small 1-px vertical bob alternating every ~8 frames. */
    if (m->state == STATE_IDLE && ((m->bob_phase / 8) & 1)) yi -= 1;

    u32 body_c  = rgba_to_c2d(COL_BODY);
    u32 eye_c   = rgba_to_c2d(COL_EYE);

    /* Body (rows 0-8). */
    for (int row = 0; row < 9; row++) {
        const char *src = crab_body[row];
        for (int col = 0; col < CRAB_W; col++) {
            char ch = src[col];
            if (ch == '@')
                C2D_DrawRectSolid((float)(xi + col), (float)(yi + row),
                                  0.6f, 1, 1, body_c);
            else if (ch == '#')
                C2D_DrawRectSolid((float)(xi + col), (float)(yi + row),
                                  0.65f, 1, 1, eye_c);
        }
    }

    /* Legs (rows 9-10) — frozen at frame 0 in ALERT/IDLE, animated
     * otherwise. */
    int frame = (m->state == STATE_ALERT || m->state == STATE_IDLE)
              ? 0 : m->anim_frame;
    for (int row = 0; row < 2; row++) {
        const char *src = crab_legs[frame][row];
        for (int col = 0; col < CRAB_W; col++) {
            if (src[col] == '@')
                C2D_DrawRectSolid((float)(xi + col), (float)(yi + 9 + row),
                                  0.6f, 1, 1, body_c);
        }
    }

    /* Red ✕ — drawn above the body when alerting, with a slight
     * horizontal sway so it reads as "waving for attention". */
    if (m->state == STATE_ALERT) {
        u32 x_c = rgba_to_c2d(COL_X);
        int sway = ((m->alert_phase / 12) & 1) ? 1 : -1;
        int x_x  = xi + (CRAB_W - 5) / 2 + sway;
        int x_y  = yi - 6;
        for (int row = 0; row < 5; row++) {
            const char *src = alert_x[row];
            for (int col = 0; col < 5; col++) {
                if (src[col] == '@')
                    C2D_DrawRectSolid((float)(x_x + col), (float)(x_y + row),
                                      0.7f, 1, 1, x_c);
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
    /* Don't let touch interrupt an alert — the user needs to see the X. */
    if (m->state == STATE_ALERT) return;
    int center = (int)m->fx + CRAB_W / 2;
    m->facing = (from_tx < center) ? +1 : -1;
    enter_flee(m);
}
