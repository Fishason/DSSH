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
    STATE_LOOKUP,   /* reconnecting: look up + "..." dots */
    STATE_HAPPY,    /* reconnect succeeded: hop + green ✓ */
    STATE_SAD,      /* reconnect failed: shiver + red ! */
    STATE_RECORD,   /* voice recording: mic + bob to beat */
    STATE_THINK,    /* voice transcribing: thinking pose + "?" */
    STATE_TYPE,     /* typing: claw-tap as if on a keyboard */
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
    int   dot_phase;      /* LOOKUP: cycles the "..." dots */
    int   hop_dy;         /* HAPPY: vertical hop offset */
    int   shiver_phase;   /* SAD: shiver tick */
    int   rec_bob;        /* RECORD: beat-bob offset */
    int   think_phase;    /* THINK: "?" sway */
    int   type_phase;     /* TYPE: claw-tap cycle (0..11) */
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

/* 5×5 green ✓ for HAPPY. */
static const char *const happy_check[5] = {
    "....@",
    "...@.",
    "@.@..",
    ".@...",
    "@....",
};

/* 3×5 red ! for SAD. */
static const char *const sad_bang[5] = {
    "@@.",
    "@@.",
    "@@.",
    "...",
    "@@.",
};

/* 5×6 mic for RECORD (capsule head + stem + little base). */
static const char *const mic_icon[6] = {
    ".@@@.",
    "@@@@@",
    "@@@@@",
    ".@@@.",
    "..@..",
    ".@@@.",
};

/* 5×5 blue ? for THINK. */
static const char *const think_q[5] = {
    ".@@@.",
    "@...@",
    "..@@.",
    "..@..",
    "..@..",
};

#define COL_CHECK 0x40b061ff   /* green */
#define COL_MIC   0xe0c06cff   /* warm gold */
#define COL_Q     0x6fa8dcff   /* soft blue */
#define COL_BANG  0xe54040ff   /* red (same hue as ✕) */

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
static void enter_lookup(mascot_t *m) {
    m->prev_state  = m->state;
    m->state       = STATE_LOOKUP;
    m->dot_phase   = 0;
    m->state_frames = 0;
}
static void enter_happy(mascot_t *m) {
    m->prev_state   = m->state;
    m->state        = STATE_HAPPY;
    m->hop_dy       = 0;
    m->state_frames = 1;
}
static void enter_sad(mascot_t *m) {
    m->prev_state   = m->state;
    m->state        = STATE_SAD;
    m->shiver_phase = 0;
    m->state_frames = 1;
}
static void enter_record(mascot_t *m) {
    m->prev_state  = m->state;
    m->state       = STATE_RECORD;
    m->rec_bob     = 0;
    m->state_frames = 0;
}
static void enter_think(mascot_t *m) {
    m->prev_state   = m->state;
    m->state        = STATE_THINK;
    m->think_phase  = 0;
    m->state_frames = 0;
}

#define TYPE_TIMEOUT_FRAMES 18   /* ~300 ms at 60 fps */

static void enter_type(mascot_t *m) {
    /* Don't interrupt the higher-priority status poses. */
    if (m->state == STATE_ALERT || m->state == STATE_LOOKUP ||
        m->state == STATE_HAPPY || m->state == STATE_SAD) return;
    /* Only reset the dot-cycle phase when freshly entering TYPE.  If the
     * crab is already typing, leave type_phase running so the "." → "..."
     * growth animation completes instead of restarting on every keystroke
     * (which would freeze it at the first dot during continuous input). */
    if (m->state != STATE_TYPE) {
        m->prev_state = m->state;
        m->type_phase = 0;
    }
    m->state        = STATE_TYPE;
    m->state_frames = TYPE_TIMEOUT_FRAMES;   /* refresh the auto-expire timer */
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

void mascot_set_reconnecting(mascot_t *m, int on) {
    if (!m) return;
    if (on && m->state != STATE_LOOKUP) {
        enter_lookup(m);
    } else if (!on && m->state == STATE_LOOKUP) {
        /* Handshake ended; hand back to caller / alert loop.  If the
         * session was dead when we started looking up, returning to
         * ALERT keeps the ✕ up until success clears it. */
        if (m->prev_state == STATE_ALERT) {
            m->state       = STATE_ALERT;
            m->alert_phase = 0;
        } else {
            enter_walk(m);
        }
    }
}

void mascot_celebrate(mascot_t *m) {
    if (!m) return;
    enter_happy(m);
}

void mascot_sadden(mascot_t *m) {
    if (!m) return;
    enter_sad(m);
}

void mascot_set_recording(mascot_t *m, int on) {
    if (!m) return;
    if (on && m->state != STATE_RECORD) {
        enter_record(m);
    } else if (!on && m->state == STATE_RECORD) {
        enter_walk(m);
    }
}

void mascot_set_thinking(mascot_t *m, int on) {
    if (!m) return;
    if (on && m->state != STATE_THINK) {
        enter_think(m);
    } else if (!on && m->state == STATE_THINK) {
        enter_walk(m);
    }
}

void mascot_type_kick(mascot_t *m) {
    if (!m) return;
    enter_type(m);
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
    m->bob_phase     = (m->bob_phase + 1) % 60;
    m->alert_phase   = (m->alert_phase + 1) % 24;
    m->dot_phase     = (m->dot_phase + 1) % 36;     /* LOOKUP dot cycle */
    m->shiver_phase  = (m->shiver_phase + 1) % 8;   /* SAD shiver */
    m->rec_bob       = (m->rec_bob + 1) % 24;       /* RECORD beat */
    m->think_phase   = (m->think_phase + 1) % 30;   /* THINK sway */
    m->type_phase    = (m->type_phase + 1) % 18;    /* TYPE bubble dot cycle */

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

        case STATE_LOOKUP:
            /* Frozen in place; the "..." dots animate in draw(). */
            break;

        case STATE_HAPPY: {
            /* 40-frame hop (~0.67 s): rise then fall via a parabola,
             * peak ≈ 5 px up at the midpoint.  Then return to WALK. */
            int t = m->state_frames;
            int h = (t < 20) ? t : (40 - t);
            m->hop_dy = -(h / 4);   /* 0 → -5 → 0 */
            if (++m->state_frames >= 40) enter_walk(m);
            break;
        }

        case STATE_SAD:
            /* Shiver in place for ~90 frames (~1.5 s), then back to ALERT
             * so the ✕ (and the dead-session signal) returns. */
            if (++m->state_frames >= 90) {
                m->state       = STATE_ALERT;
                m->alert_phase = 0;
            }
            break;

        case STATE_RECORD:
        case STATE_THINK:
            /* Held externally; no auto-transition.  Body animation is
             * driven off rec_bob / think_phase in draw(). */
            break;

        case STATE_TYPE:
            /* Auto-expire ~300 ms after the last kick; return to whatever
             * the crab was doing before typing started (usually WALK). */
            if (m->state_frames > 0) m->state_frames--;
            if (m->state_frames == 0) {
                mascot_state_t p = m->prev_state;
                /* Don't restore into an obsolete pose; WALK is the safe
                 * default.  ALERT/etc. are never stored as prev by
                 * enter_type (it refuses to interrupt them). */
                enter_walk(m);
                (void)p;
            }
            break;
    }
}

void mascot_draw(mascot_t *m) {
    if (!m) return;
    int xi = (int)m->fx;
    int yi = m->y_top;

    /* Idle small bob. */
    if (m->state == STATE_IDLE && ((m->bob_phase / 8) & 1)) yi -= 1;

    /* HAPPY hop: whole body rises by hop_dy (≤ 0). */
    if (m->state == STATE_HAPPY) yi += m->hop_dy;

    /* SAD shiver: jitter ±1 px horizontally. */
    if (m->state == STATE_SAD) xi += (m->shiver_phase < 4) ? 0 : -1;

    /* RECORD beat-bob: bounce ±1 px in time to a ~5 Hz beat, like the
     * crab is nodding along to your voice. */
    if (m->state == STATE_RECORD) yi -= (m->rec_bob < 12) ? 1 : 0;

    /* THINK gentle sway: drift ±1 px horizontally, slower than SAD. */
    if (m->state == STATE_THINK) xi += (m->think_phase < 15) ? 0 : -1;

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

    /* TYPE "typing…" speech bubble: a salmon-pink rounded-rect outline
     * floats above the crab with a little tail pointing down at it, and
     * 1-3 dots grow inside in sequence (. → .. → ... → blank → repeat)
     * to read as "typing".  More legible at 18×11 body scale than a
     * limb animation. */
    if (m->state == STATE_TYPE) {
        u32 c = rgba_to_c2d(COL_BODY);
        /* Bubble geometry: 11 wide × 7 tall, centered over the body. */
        const int BW = 11, BH = 7;
        int bx = xi + (CRAB_W - BW) / 2;
        int by = yi - BH - 3;   /* 3-px gap above the body */
        /* Rounded-rect outline (corners left open → reads rounded). */
        for (int y = 0; y < BH; y++) {
            for (int x = 0; x < BW; x++) {
                int edge = (y == 0 || y == BH - 1 || x == 0 || x == BW - 1);
                int corner = (x < 1 || x > BW - 2) && (y < 1 || y > BH - 2);
                if (edge && !corner) {
                    C2D_DrawRectSolid((float)(bx + x), (float)(by + y),
                                      0.7f, 1, 1, c);
                }
            }
        }
        /* Tail: two pixels dropping from the bubble's bottom edge toward
         * the crab's head, offset left of center. */
        int tx = bx + 3;
        C2D_DrawRectSolid((float)(tx), (float)(by + BH), 0.7f, 1, 1, c);
        C2D_DrawRectSolid((float)(tx), (float)(by + BH + 1), 0.7f, 1, 1, c);
        /* Growing dots: one new dot lights every 5 frames, then a brief
         * blank pause before the cycle restarts. */
        int ndots = (m->type_phase < 15) ? (m->type_phase / 5) + 1 : 0;
        if (ndots > 3) ndots = 3;
        for (int d = 0; d < ndots; d++) {
            int dx = bx + 3 + d * 2;   /* dots at x=3,5,7 inside bubble */
            int dy = by + 3;            /* vertically centered */
            C2D_DrawRectSolid((float)(dx), (float)(dy), 0.75f, 1, 1, c);
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

    /* "..." dots above the head for LOOKUP.  Three dots, each 2×2, spaced
     * 3 px apart and centered over the body; they brighten in sequence to
     * read as a loading animation (one new dot lights up every ~0.2 s). */
    if (m->state == STATE_LOOKUP) {
        u32 dim = C2D_Color32(0xcc, 0xcc, 0xcc, 0x80);
        u32 lit = C2D_Color32(0xff, 0xff, 0xff, 0xff);
        int lit_count = (m->dot_phase / 6) % 4;   /* 0..3 dots */
        int base_x = xi + (CRAB_W - 10) / 2;       /* center 10-px strip */
        int base_y = yi - 5;
        for (int d = 0; d < 3; d++) {
            u32 c = (d < lit_count) ? lit : dim;
            int dx = base_x + d * 4;
            for (int row = 0; row < 2; row++) {
                for (int col = 0; col < 2; col++) {
                    C2D_DrawRectSolid((float)(dx + col),
                                      (float)(base_y + row),
                                      0.7f, 1, 1, c);
                }
            }
        }
    }

    /* Green ✓ overlay for HAPPY (static, centered above the hop). */
    if (m->state == STATE_HAPPY) {
        u32 c = rgba_to_c2d(COL_CHECK);
        int cx = xi + (CRAB_W - 5) / 2;
        int cy = yi - 6;
        for (int row = 0; row < 5; row++) {
            const char *src = happy_check[row];
            for (int col = 0; col < 5; col++) {
                if (src[col] == '@') {
                    C2D_DrawRectSolid((float)(cx + col),
                                      (float)(cy + row),
                                      0.7f, 1, 1, c);
                }
            }
        }
    }

    /* Red ! overlay for SAD (sways gently with the shiver). */
    if (m->state == STATE_SAD) {
        u32 c = rgba_to_c2d(COL_BANG);
        int sway = ((m->shiver_phase < 4) ? 0 : -1);
        int bx = xi + (CRAB_W - 3) / 2 + sway;
        int by = yi - 7;
        for (int row = 0; row < 5; row++) {
            const char *src = sad_bang[row];
            for (int col = 0; col < 3; col++) {
                if (src[col] == '@') {
                    C2D_DrawRectSolid((float)(bx + col),
                                      (float)(by + row),
                                      0.7f, 1, 1, c);
                }
            }
        }
    }

    /* Gold mic overlay for RECORD — held just above the body; a 1-px
     * arm connects it to the crab so it reads as "held up". */
    if (m->state == STATE_RECORD) {
        u32 c = rgba_to_c2d(COL_MIC);
        int mx = xi + (CRAB_W - 5) / 2;
        int my = yi - 7;
        for (int row = 0; row < 6; row++) {
            const char *src = mic_icon[row];
            for (int col = 0; col < 5; col++) {
                if (src[col] == '@') {
                    C2D_DrawRectSolid((float)(mx + col),
                                      (float)(my + row),
                                      0.7f, 1, 1, c);
                }
            }
        }
    }

    /* Blue ? overlay for THINK (sways gently with the think drift). */
    if (m->state == STATE_THINK) {
        u32 c = rgba_to_c2d(COL_Q);
        int sway = ((m->think_phase < 15) ? 0 : -1);
        int qx = xi + (CRAB_W - 5) / 2 + sway;
        int qy = yi - 6;
        for (int row = 0; row < 5; row++) {
            const char *src = think_q[row];
            for (int col = 0; col < 5; col++) {
                if (src[col] == '@') {
                    C2D_DrawRectSolid((float)(qx + col),
                                      (float)(qy + row),
                                      0.7f, 1, 1, c);
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
    /* Don't flee out of the reconnect- or voice-related states — the
     * crab is busy signaling and shouldn't run away. */
    if (m->state == STATE_ALERT || m->state == STATE_LOOKUP ||
        m->state == STATE_HAPPY || m->state == STATE_SAD  ||
        m->state == STATE_RECORD || m->state == STATE_THINK ||
        m->state == STATE_TYPE) return;
    int center = (int)m->fx + CRAB_W / 2;
    m->facing = (from_tx < center) ? +1 : -1;
    enter_flee(m);
}
