#pragma once

/*
 * The Anthropic-red crab mascot that scampers along the bottom row of
 * the soft keyboard.  Has three states:
 *   WALK   slow horizontal traversal, bouncing off the configured x range
 *   IDLE   stops and bobs in place
 *   FLEE   triggered by a touch on the crab, runs fast away from where
 *          the touch landed for ~1 second, then returns to WALK
 *
 * All coordinates are bottom-screen pixel space (320×240, top-left origin).
 */

typedef struct mascot_t mascot_t;

/* Allocate a mascot bouncing inside [x_min, x_max] at fixed y row. */
mascot_t *mascot_init(int x_min, int x_max, int y);
void      mascot_free(mascot_t *m);

/* Advance one frame. */
void mascot_update(mascot_t *m);

/* Draw at current position. Caller must already be inside C2D_SceneBegin(bot). */
void mascot_draw(mascot_t *m);

/* Touch helpers. */
int  mascot_hit_test(const mascot_t *m, int tx, int ty);
void mascot_on_touched(mascot_t *m, int from_tx);
