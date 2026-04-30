#pragma once
#include <3ds.h>

/*
 * Physical-button input layer for 3dssh M3.
 *
 * Maps 3DS hardware (D-pad, A/B/X/Y, L/R, START, SELECT, Circle Pad) to
 * SSH-bound byte sequences. Sticky Ctrl modifier driven by SELECT:
 *
 *   OFF ──[SELECT]──> ARMED ──[next produced char]──> OFF
 *
 * Just two states. ARMED transforms the next character into Ctrl-X form
 * (one-shot) and reverts to OFF. There is no "lock" mode (the user
 * explicitly didn't want it).  L button is still a hold-style Ctrl
 * alternative (L+key always sends Ctrl-key regardless of sticky state).
 *
 * Circle Pad always scrolls the local terminal scrollback (no mode
 * toggle needed). Any other key snaps the view back to live output.
 */

typedef enum {
    MOD_OFF = 0,
    MOD_ARMED,
} mod_state_t;

typedef struct keyboard_t {
    mod_state_t sticky_ctrl;
    int         scroll_timer;   /* debounce: N frames between scroll steps */

    /* Output buffer for the byte sequence produced this frame. */
    char  out_buf[16];
    int   out_len;
} keyboard_t;

/* Lifecycle. */
keyboard_t *keyboard_init(void);
void        keyboard_free(keyboard_t *kbd);

/* Per-frame: examine input edge events, update sticky state, optionally
 * scroll the terminal, and produce a byte sequence to send over SSH.
 *
 * Returns: pointer to internal byte buffer with .out_len bytes ready to
 * write to SSH. Returns NULL if nothing to send this frame.
 *
 * The pointer is valid until the next call to keyboard_handle_input().
 *
 * Pass NULL for term to disable scrollback navigation entirely.
 */
struct terminal_t;
const char *keyboard_handle_input(keyboard_t *kbd,
                                  struct terminal_t *term,
                                  u32 keys_down, u32 keys_held,
                                  int circle_dy);

/* For the status panel: short label of current sticky-Ctrl state. */
const char *keyboard_mod_label(const keyboard_t *kbd);

/* Apply the current sticky Ctrl state to a buffer in place.
 *
 * ARMED  -> transform the first byte to its Ctrl- form, then drop to OFF.
 * LOCKED -> transform every byte (Ctrl is held until SELECT pressed again).
 * OFF    -> no-op.
 *
 * Used by the swkbd path in main.c so that pressing SELECT then opening
 * the system keyboard and typing 'c' produces Ctrl-C on the wire.
 *
 * Returns the (possibly unchanged) buffer length. */
int keyboard_apply_modifiers(keyboard_t *kbd, char *buf, int len);
