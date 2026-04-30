#include "mascot.h"
#include <citro2d.h>
#include <stdlib.h>
#include <stdint.h>

/*
 * Anthropic-red crab mascot.  18×10 px pixel art, pure C2D rect blits
 * (one DrawRectSolid per lit pixel).  At ~30 lit pixels per frame and
 * 60 fps that's 1800 rects/sec — negligible vs the terminal grid (~1500
 * rects/frame), so we don't bother batching.
 *
 * State machine:
 *   WALK   default; bounces between [x_min, x_max - CRAB_W].  0.5 px/frame.
 *   IDLE   entered randomly from WALK every ~10 sec for 1-3 sec of no-x
 *          movement + a small vertical bobble.
 *   FLEE   entered when touched.  2 px/frame opposite from the touch point
 *          for ~1 sec, or until we hit a wall.
 *
 * Leg animation: 2 frames swapped every 8 update ticks for a "scuttle".
 * Eyes are simple white pixels embedded in the body.
 */

typedef enum {
    STATE_WALK,
    STATE_IDLE,
    STATE_FLEE,
} mascot_state_t;

struct mascot_t {
    int   x_min, x_max, y_top;
    float fx;             /* sub-pixel x */
    int   facing;         /* -1 = left, +1 = right */
    mascot_state_t state;
    int   state_frames;   /* frames left in IDLE/FLEE; 0 in WALK = open-ended */
    int   anim_frame;     /* 0/1 leg animation */
    int   anim_timer;
    int   bob_phase;
};

#define CRAB_W   18
#define CRAB_H   10
#define HIT_PAD  4

#define COL_BODY 0xee5050ff
#define COL_EYE  0xffffffff

/* Pixel art for the 8 fixed rows + leg row 8 + leg row 9 (frame 0).
 * '@' = body fill, 'o' = white eye, ' ' = transparent.  Each row is
 * exactly CRAB_W chars + NUL. */
static const char *const crab_art[CRAB_H] = {
    " @@           @@  ",
    " @@           @@  ",
    "  @           @   ",
    "  @@@@@@@@@@@@@   ",
    " @@@@@@@@@@@@@@@  ",
    "@@@@o@@@@@@@@o@@@@",
    "@@@@@@@@@@@@@@@@@@",
    " @@@@@@@@@@@@@@@@ ",
    " @ @  @@  @@  @ @ ",   /* legs frame 0, row 8 */
    "@ @ @@@@  @@@@ @ @",   /* legs frame 0, row 9 */
};

/* Frame 1 swaps the leg row patterns — gives a small vertical shuffle. */
static const char *const crab_legs_alt[2] = {
    "@ @ @@@@  @@@@ @ @",   /* row 8 in frame 1 */
    " @ @  @@  @@  @ @ ",   /* row 9 in frame 1 */
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
    m->state_frames = 60 + (rand() % 120);   /* 1-3 sec */
}
static void enter_flee(mascot_t *m) {
    m->state = STATE_FLEE;
    m->state_frames = 50 + (rand() % 30);    /* ~0.8-1.3 sec */
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

    /* Leg animation runs in WALK and FLEE; freezes in IDLE. */
    if (m->state != STATE_IDLE) {
        if (++m->anim_timer >= 8) {
            m->anim_timer = 0;
            m->anim_frame ^= 1;
        }
    }
    m->bob_phase = (m->bob_phase + 1) % 60;

    int hit_wall = 0;
    switch (m->state) {
        case STATE_WALK:
            m->fx += 0.5f * (float)m->facing;
            clamp_and_bounce(m, &hit_wall);
            /* Random transition to idle (~once every 10 sec). */
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
    }
}

void mascot_draw(mascot_t *m) {
    if (!m) return;
    int xi = (int)m->fx;
    int yi = m->y_top;
    /* Idle: small 1-px vertical bob alternating every ~8 frames. */
    if (m->state == STATE_IDLE && ((m->bob_phase / 8) & 1)) yi -= 1;

    u32 body = rgba_to_c2d(COL_BODY);
    u32 eye  = rgba_to_c2d(COL_EYE);

    for (int row = 0; row < CRAB_H; row++) {
        const char *src;
        if (row >= 8 && m->anim_frame == 1)
            src = crab_legs_alt[row - 8];
        else
            src = crab_art[row];
        for (int col = 0; col < CRAB_W; col++) {
            char ch = src[col];
            if (ch == '@')
                C2D_DrawRectSolid((float)(xi + col), (float)(yi + row),
                                  0.6f, 1, 1, body);
            else if (ch == 'o')
                C2D_DrawRectSolid((float)(xi + col), (float)(yi + row),
                                  0.65f, 1, 1, eye);
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
    int center = (int)m->fx + CRAB_W / 2;
    /* Run away from the finger. */
    m->facing = (from_tx < center) ? +1 : -1;
    enter_flee(m);
}
