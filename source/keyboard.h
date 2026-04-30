#pragma once
#include <3ds.h>

/*
 * Physical-button input layer for 3dssh M3.
 *
 * Maps 3DS hardware (D-pad, A/B/X/Y, L/R, START, SELECT, Circle Pad) to
 * SSH-bound byte sequences. Includes a sticky Ctrl modifier state machine
 * driven by the SELECT key:
 *
 *   OFF ──[SELECT]──> ARMED ──[SELECT]──> LOCKED ──[SELECT]──> OFF
 *           ↑                                  │
 *           └────[next non-modifier key]───────┘  (auto-clear after one use)
 *
 * In ARMED, the next produced character (a-z, ,, ., etc.) is converted to
 * Ctrl-X form and the state drops back to OFF. In LOCKED, every produced
 * character is Ctrl-X until SELECT is pressed again. The L button provides
 * a hold-style alternative: L+key always sends Ctrl-key regardless of the
 * sticky state.
 *
 * Y toggles a "scroll mode" — while in scroll mode, the Circle Pad scrolls
 * the terminal scrollback buffer instead of being routed elsewhere. Press
 * Y again to leave.
 */

typedef enum {
    MOD_OFF = 0,
    MOD_ARMED,
    MOD_LOCKED,
} mod_state_t;

typedef struct keyboard_t {
    mod_state_t sticky_ctrl;
    int         scroll_mode;    /* Y toggles */

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

/* For the status panel: returns an immutable display string for the current
 * sticky Ctrl state ("CTL", "[CTL]", "(ctl)" or empty). */
const char *keyboard_mod_label(const keyboard_t *kbd);

/* True if scroll mode is active (renderer can show indicator). */
int keyboard_in_scroll_mode(const keyboard_t *kbd);
