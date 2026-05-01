#include "keyboard.h"
#include "terminal.h"
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────
 * D-pad / B auto-repeat tuning
 * ──────────────────────────────────────────────────────────────────────
 * 3DS runs at 60 fps — 1 frame ≈ 16.7 ms.  Tuned faster in M7 polish
 * after user feedback that long-press navigation / delete felt sluggish.
 *
 * DPAD_INITIAL_DELAY: frames before auto-repeat kicks in.
 * dpad_repeat_period(phase): frames between successive emissions.
 *                            Lower = faster.  Three-step accelerating ramp.
 */
#define DPAD_INITIAL_DELAY 15        /* 250 ms initial pause (was 25/417ms) */

static int dpad_repeat_period(int phase) {
    if (phase <  30) return 5;    /* first 0.5 s: 12/sec  (was  6/sec) */
    if (phase <  90) return 2;    /* next  1.0 s: 30/sec  (was 12/sec) */
    return 1;                     /* after 1.5 s: 60/sec  (was 20/sec) */
}

/* ────────────────────────────────────────────────────────────────────── */

keyboard_t *keyboard_init(void) {
    keyboard_t *k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    k->mode = MODE_EN;
    k->shift_press_f = -1;
    k->ctrl_press_f  = -1;
    k->alt_press_f   = -1;
    return k;
}

void keyboard_free(keyboard_t *kbd) { free(kbd); }

void keyboard_set_ime(keyboard_t *kbd, ime_t *ime) {
    if (!kbd) return;
    kbd->ime = ime;
}

/* ── byte emission helpers ─────────────────────────────────────────── */

static const char *emit_seq(keyboard_t *k, const char *seq) {
    int n = (int)strlen(seq);
    if (n >= (int)sizeof(k->out_buf)) n = sizeof(k->out_buf) - 1;
    memcpy(k->out_buf, seq, n);
    k->out_buf[n] = 0;
    k->out_len    = n;
    return k->out_buf;
}

static const char *emit_byte(keyboard_t *k, char c) {
    k->out_buf[0] = c;
    k->out_buf[1] = 0;
    k->out_len    = 1;
    return k->out_buf;
}

static void mark_event(keyboard_t *k, const char *label) {
    k->last_event_label = label;
    k->last_event_frame = k->frame;
}

/* Convert a printable ASCII char to its Ctrl- equivalent, or 0 if no
 * sensible mapping. */
static char to_ctrl(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 1;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
    if (c == ' ' || c == '@') return 0x00;     /* Ctrl-Space = NUL */
    if (c == '[')  return 0x1b;                /* Ctrl-[ = Esc */
    if (c == '\\') return 0x1c;
    if (c == ']')  return 0x1d;
    if (c == '^')  return 0x1e;
    if (c == '_')  return 0x1f;
    if (c == '?')  return 0x7f;
    return 0;
}

/* ── physical-key handler ──────────────────────────────────────────── */

const char *keyboard_handle_input(keyboard_t *kbd,
                                  struct terminal_t *term,
                                  u32 keys_down, u32 keys_held,
                                  int circle_dy) {
    if (!kbd) return NULL;
    kbd->out_len = 0;
    kbd->frame++;

    /* Update modifier hold state from keys_held. */
    kbd->shift_held = (keys_held & KEY_L) != 0;
    kbd->ctrl_held  = (keys_held & KEY_Y) != 0;
    kbd->alt_held   = (keys_held & KEY_X) != 0;
    /* Stamp press frame on transition not-held → held. */
    if (keys_down & KEY_L) kbd->shift_press_f = kbd->frame;
    if (keys_down & KEY_Y) kbd->ctrl_press_f  = kbd->frame;
    if (keys_down & KEY_X) kbd->alt_press_f   = kbd->frame;

    /* SELECT → Esc */
    if (keys_down & KEY_SELECT) {
        mark_event(kbd, "ESC");
        return emit_byte(kbd, '\x1b');
    }

    /* R → toggle IME mode.  Transient label "R→C" / "R→E" reads as
     * "R toggles to CN / EN".  Three display columns wide (R + arrow
     * + letter); the renderer is UTF-8 aware so the arrow draws
     * correctly. */
    if (keys_down & KEY_R) {
        kbd->mode = (kbd->mode == MODE_EN) ? MODE_CN : MODE_EN;
        mark_event(kbd, kbd->mode == MODE_CN ? "R→C" : "R→E");
        return NULL;
    }

    /* Circle Pad scroll — same throttling as M3 polish:
     *   deadband 80, sustained-push 6 frames before first event,
     *   then one event per 5 frames.
     *
     * Pane targeting in tmux mouse mode: tmux routes wheel events to
     * whichever pane contains the (col,row) we send.  Default (1,1)
     * lands in the left/top pane.  Holding L (Shift modifier) shifts
     * the target to (60,12), which lands in the right pane of a
     * vertical split or the bottom pane of a horizontal split — the
     * 3DS has no real cursor so this hold-style toggle is the
     * simplest way to give both panes access without a UI mode. */
    int scroll_active = (circle_dy > 80 || circle_dy < -80);
    if (scroll_active) {
        kbd->scroll_timer++;
        int armed = (kbd->scroll_timer == 6) ||
                    (kbd->scroll_timer > 6 && (kbd->scroll_timer - 6) % 5 == 0);
        if (armed) {
            if (term && term->mouse_proto && term->mouse_sgr) {
                const char *seq;
                if (kbd->shift_held) {
                    seq = (circle_dy > 0) ? "\x1b[<64;60;12M"
                                           : "\x1b[<65;60;12M";
                } else {
                    seq = (circle_dy > 0) ? "\x1b[<64;1;1M"
                                           : "\x1b[<65;1;1M";
                }
                return emit_seq(kbd, seq);
            }
            if (term) terminal_scroll_view(term, circle_dy > 0 ? 1 : -1);
        }
    } else {
        kbd->scroll_timer = 0;
    }

    /* D-pad auto-repeat (priority Up > Down > Right > Left). */
    u32 dpad = keys_held & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);
    u32 dpad_active = 0;
    if      (dpad & KEY_DUP)    dpad_active = KEY_DUP;
    else if (dpad & KEY_DDOWN)  dpad_active = KEY_DDOWN;
    else if (dpad & KEY_DRIGHT) dpad_active = KEY_DRIGHT;
    else if (dpad & KEY_DLEFT)  dpad_active = KEY_DLEFT;

    if (dpad_active != kbd->dpad_last) {
        kbd->dpad_last   = dpad_active;
        kbd->dpad_frames = 0;
    }
    if (dpad_active) {
        int fire = 0;
        if (kbd->dpad_frames == 0) {
            fire = 1;
        } else if (kbd->dpad_frames >= DPAD_INITIAL_DELAY) {
            int phase = kbd->dpad_frames - DPAD_INITIAL_DELAY;
            int period = dpad_repeat_period(phase);
            if (period > 0 && phase % period == 0) fire = 1;
        }
        kbd->dpad_frames++;
        if (fire) {
            /* IME mode swaps the D-pad onto candidate navigation:
             *   Up    → previous page
             *   Down  → next page
             *   Left  → selection cursor left within page
             *   Right → selection cursor right within page
             * Arrow keys still go through normally when no buffer. */
            if (kbd->ime && ime_active(kbd->ime)) {
                if      (dpad_active == KEY_DUP)    ime_page_prev(kbd->ime);
                else if (dpad_active == KEY_DDOWN)  ime_page_next(kbd->ime);
                else if (dpad_active == KEY_DLEFT)  ime_selection_left(kbd->ime);
                else if (dpad_active == KEY_DRIGHT) ime_selection_right(kbd->ime);
                return NULL;
            }
            const char *seq;
            if      (dpad_active == KEY_DUP)    seq = "\x1b[A";
            else if (dpad_active == KEY_DDOWN)  seq = "\x1b[B";
            else if (dpad_active == KEY_DRIGHT) seq = "\x1b[C";
            else                                 seq = "\x1b[D";
            return emit_seq(kbd, seq);
        }
    }

    /* A in IME mode is the "oops, I meant English" escape hatch:
     * dumps the pinyin buffer to SSH as raw ASCII letters and clears
     * the buffer.  This way you don't have to backspace + toggle
     * mode + retype if you accidentally typed in CN.
     *
     * Space remains the "commit candidate" key (handled by softkb).
     * A in EN mode (or with empty buffer) sends the literal Enter. */
    if (keys_down & KEY_A) {
        if (kbd->ime && ime_active(kbd->ime)) {
            const char *result = emit_seq(kbd, ime_buffer(kbd->ime));
            ime_clear(kbd->ime);
            mark_event(kbd, "ENT");
            return result;
        }
        mark_event(kbd, "ENT");
        if (term && term->sb_offset) terminal_scroll_view(term, -term->sb_offset);
        return emit_byte(kbd, '\r');
    }
    /* B → Backspace, with the same auto-repeat ramp as D-pad so holding
     * B for a couple seconds rapid-deletes (super useful in Claude Code's
     * input editor).  In IME mode, B chews the pinyin buffer instead. */
    if (keys_held & KEY_B) {
        int b_pressed_now = (keys_down & KEY_B) != 0;
        int fire = 0;
        if (b_pressed_now) {
            kbd->b_held_frames = 0;
            mark_event(kbd, "BSP");
            fire = 1;
        } else {
            kbd->b_held_frames++;
            if (kbd->b_held_frames >= DPAD_INITIAL_DELAY) {
                int phase  = kbd->b_held_frames - DPAD_INITIAL_DELAY;
                int period = dpad_repeat_period(phase);
                if (period > 0 && phase % period == 0) fire = 1;
            }
        }
        if (fire) {
            if (kbd->ime && ime_active(kbd->ime)) {
                ime_backspace(kbd->ime);
                return NULL;
            }
            return emit_byte(kbd, 0x7f);
        }
    } else {
        kbd->b_held_frames = 0;
    }
    /* X / Y / L are modifiers — no byte on their own; flags already updated. */
    return NULL;
}

/* ── modifier queries for softkb ────────────────────────────────────── */

int keyboard_shift_held(const keyboard_t *k) { return k && k->shift_held; }
int keyboard_ctrl_held (const keyboard_t *k) { return k && k->ctrl_held;  }
int keyboard_alt_held  (const keyboard_t *k) { return k && k->alt_held;   }
ime_mode_t keyboard_get_mode(const keyboard_t *k) {
    return k ? k->mode : MODE_EN;
}

/* ── status-indicator label ────────────────────────────────────────── */

#define EVENT_LIFETIME_FRAMES 12

const char *keyboard_status_label(const keyboard_t *k) {
    if (!k) return "   ";
    /* 1) Held modifier (most recent press wins). */
    int s_f = k->shift_held ? k->shift_press_f : -1;
    int c_f = k->ctrl_held  ? k->ctrl_press_f  : -1;
    int a_f = k->alt_held   ? k->alt_press_f   : -1;
    if (s_f >= 0 || c_f >= 0 || a_f >= 0) {
        int max = -1;
        const char *lbl = "   ";
        if (s_f > max) { max = s_f; lbl = "SFT"; }
        if (c_f > max) { max = c_f; lbl = "CTL"; }
        if (a_f > max) { max = a_f; lbl = "ALT"; }
        return lbl;
    }
    /* 2) Recent transient event. */
    if (k->last_event_label &&
        (k->frame - k->last_event_frame) < EVENT_LIFETIME_FRAMES) {
        return k->last_event_label;
    }
    /* 3) Idle. */
    return "   ";
}

/* ── softkb dispatch helper ────────────────────────────────────────── */

const char *keyboard_emit_for(keyboard_t *k, char base) {
    if (!k || base == 0) return NULL;

    /* Special-case Shift + . → 。 (Chinese full-width period, U+3002).
     * US Shift+. = '>' which we don't bind anyway (the soft keyboard
     * has '>' on the symbols page).  Mode-agnostic so it also works
     * when typing English with the period — Chinese sentence-end is
     * a frequent enough need. */
    if (k->shift_held && base == '.') {
        return emit_seq(k, "\xe3\x80\x82");
    }

    /* Apply Shift to letters (only). */
    char c = base;
    if (k->shift_held && c >= 'a' && c <= 'z') c = c - 'a' + 'A';

    /* Ctrl: emit Ctrl-X form. */
    if (k->ctrl_held) {
        char x = to_ctrl(c);
        if (x == 0) return emit_byte(k, c);  /* fallback: literal */
        return emit_byte(k, x);
    }

    /* Alt: ESC + char (Linux/xterm convention). */
    if (k->alt_held) {
        k->out_buf[0] = '\x1b';
        k->out_buf[1] = c;
        k->out_buf[2] = 0;
        k->out_len    = 2;
        return k->out_buf;
    }

    return emit_byte(k, c);
}
