#pragma once

/*
 * Anthropic-style salmon-pink crab mascot that scampers along the
 * bottom row of the soft keyboard.
 *
 * States (internal):
 *   WALK    slow horizontal traversal, bouncing off the configured x range.
 *   IDLE    stops and bobs in place.
 *   FLEE    triggered by a touch on the crab; runs fast away for ~1 sec
 *           then returns to WALK.
 *   ALERT   set externally by main.c when the SSH connection has stalled.
 *           Crab freezes and holds up a red ✕, waving it gently until
 *           the alert is cleared.
 *   LOOKUP  reconnect in progress: crab freezes, looks up, and three
 *           dots "..." cycle above its head.  Set externally while the
 *           blocking SSH handshake is in flight; cleared on success/fail.
 *   HAPPY   reconnect succeeded: a brief hop with a green ✓, then back
 *           to WALK.  One-shot, auto-expires.
 *   SAD     reconnect failed: crab tilts and shivers with a red !,
 *           then back to ALERT.  One-shot, auto-expires.
 *   RECORD  voice recording: crab holds up a microphone icon and bobs
 *           in time to the beat, like it's listening.  Set externally
 *           while the mic is capturing.
 *   THINK   voice transcribing: crab pauses in a "thinking" pose with a
 *           ? above its head while the server transcribes.  Set externally
 *           around the blocking ASR round-trip.
 *   TYPE    typing: crab taps its claws as if on a keyboard.  Kicked per
 *           character by mascot_type_kick() — re-kicks refresh the timer
 *           so a run of typing (soft-key press, handwriting, voice TYPING
 *           streaming) keeps the crab typing continuously; once input
 *           stops it auto-expires back to its prior state (~300 ms).
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

/* Toggle the alert overlay (red ✕ above the body, waving).  Called by
 * main.c when SSH stall detection fires; idempotent — repeated calls
 * with the same value are no-ops. */
void mascot_set_alert(mascot_t *m, int alert);

/* Toggle the reconnecting state (crab looks up, "..." dots above).
 * Called by main.c around the blocking SSH handshake; idempotent. */
void mascot_set_reconnecting(mascot_t *m, int on);

/* One-shot reconnect-succeeded celebration (hop + green ✓).
 * Auto-expires back to WALK after a moment. */
void mascot_celebrate(mascot_t *m);

/* One-shot reconnect-failed reaction (tilt/shiver + red !).
 * Auto-expires back to ALERT after a moment. */
void mascot_sadden(mascot_t *m);

/* Toggle the voice-recording state (crab holds a mic, bobs to beat).
 * Called by main.c while the mic is capturing; idempotent. */
void mascot_set_recording(mascot_t *m, int on);

/* Toggle the thinking state (crab pauses, "?" above head) while the
 * server transcribes voice input; idempotent. */
void mascot_set_thinking(mascot_t *m, int on);

/* Kick one "typing" cycle (~300 ms): the crab taps its claws like it's
 * mashing a keyboard.  Call once per character sent to SSH — re-kicks
 * while active just refresh the timer so continuous typing stays in the
 * pose, and it auto-expires ~300 ms after the last character. */
void mascot_type_kick(mascot_t *m);
