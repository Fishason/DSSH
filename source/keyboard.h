#pragma once
#include <3ds.h>
#include "ime_pinyin.h"

/*
 * Physical-button input layer for 3dssh M4.
 *
 * Final key bindings (M4 design):
 *   START   quit program
 *   SELECT  → emit \x1b (Esc)
 *   A       → emit \r  (Enter)
 *   B       → emit 0x7f (Backspace)
 *   Y       → Ctrl modifier  (hold-style: only updates flag, no direct emit)
 *   X       → Alt  modifier  (hold-style)
 *   L       → Shift modifier (hold-style)
 *   R       → toggle IME mode (EN ↔ CN)
 *   D-pad   → arrow keys with accelerating auto-repeat
 *   Circle  → tmux mouse-wheel forwarding / local scrollback
 *
 * The modifier keys (Y/X/L) don't produce any byte themselves — they
 * just update flags that the soft keyboard (softkb.c) queries when
 * dispatching a tapped letter.  That's why pressing Y on its own
 * sends nothing; you have to press Y AND tap a screen letter.
 */

typedef enum {
    MODE_EN = 0,    /* English direct typing */
    MODE_CN,        /* Chinese pinyin (M7 will route through IME; M4 = passthrough) */
} ime_mode_t;

typedef struct keyboard_t {
    /* D-pad auto-repeat state */
    u32 dpad_last;
    int dpad_frames;
    /* Backspace (B) auto-repeat state — same ramp as D-pad. */
    int b_held_frames;
    /* Circle Pad scroll throttling */
    int scroll_timer;

    /* IME mode toggled by R */
    ime_mode_t mode;

    /* Monotonic frame counter (incremented each handle_input call) */
    int frame;

    /* Modifier "is currently held" — mirrors keys_held bits */
    int shift_held, ctrl_held, alt_held;
    /* Frame stamp of most-recent press for each modifier (so we can
     * pick the "most recent" modifier when several are held). */
    int shift_press_f, ctrl_press_f, alt_press_f;

    /* Last transient event for the status indicator (Enter/BS/Esc/R toggle).
     * Shown for ~12 frames after the press, then fades. */
    const char *last_event_label;   /* "ENT", "BSP", "ESC", "R→C", "R→E", NULL */
    int         last_event_frame;

    /* Output byte buffer for one frame's worth of emission. */
    char out_buf[16];
    int  out_len;

    /* M7: pinyin IME pointer (may be NULL).  When set and active, the
     * B key does ime_backspace instead of emitting 0x7f, and D-pad
     * left/right page through candidates instead of sending arrow keys. */
    ime_t *ime;
} keyboard_t;

keyboard_t *keyboard_init(void);
void        keyboard_free(keyboard_t *kbd);

/* Wire the pinyin IME to the keyboard.  Pass NULL to disable.  Until
 * this is called the keyboard runs in pure-passthrough M4 behavior. */
void        keyboard_set_ime(keyboard_t *kbd, ime_t *ime);

/* Per-frame physical-key handling.  Emits bytes for SELECT/A/B/D-pad and
 * sustained Circle Pad scroll/wheel events.  Modifier keys (L/Y/X) and
 * R only update internal state and return NULL.
 *
 * Returns a NUL-terminated byte sequence to send to SSH, or NULL if
 * nothing to send this frame. */
struct terminal_t;
const char *keyboard_handle_input(keyboard_t *kbd,
                                  struct terminal_t *term,
                                  u32 keys_down, u32 keys_held,
                                  int circle_dy);

/* Modifier-state queries used by the soft keyboard. */
int        keyboard_shift_held(const keyboard_t *kbd);
int        keyboard_ctrl_held(const keyboard_t *kbd);
int        keyboard_alt_held (const keyboard_t *kbd);
ime_mode_t keyboard_get_mode (const keyboard_t *kbd);

/* Status-indicator label.  Returns a NUL-terminated UTF-8 string that
 * renders to exactly 3 display cells in the leftmost slot of the soft
 * keyboard's top row.  Priority:
 *   1) any held modifier        →  "SFT" / "CTL" / "ALT" (most recent)
 *   2) transient event < 12 frames ago →  "ENT" / "BSP" / "ESC" / "R→C" / "R→E"
 *   3) idle                      →  "   " (3 spaces)
 *
 * Note that "R→C" is 5 bytes (UTF-8 encodes → as E2 86 92) but renders
 * as 3 display columns — renderer_draw_text_px iterates by codepoint. */
const char *keyboard_status_label(const keyboard_t *kbd);

/* Combine the currently-held physical modifiers with a base ASCII char
 * (typically from a soft-keyboard tap) and return the byte sequence to
 * send to SSH.
 *
 * Mappings:
 *   L + letter   → uppercase (a→A …)
 *   L + digit    → no special handling (use page 2 for symbols)
 *   Y + letter   → Ctrl-letter (a → 0x01 …)
 *   Y + special  → Ctrl-form where applicable (Ctrl-[ = 0x1b, etc.)
 *   X + char     → Alt-char = ESC then char (2-byte sequence)
 *   none         → the literal char
 *
 * Returns pointer to internal buffer, NULL on nonsensical combination. */
const char *keyboard_emit_for(keyboard_t *kbd, char base);
