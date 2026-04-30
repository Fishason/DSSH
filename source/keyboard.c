#include "keyboard.h"
#include "terminal.h"
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────
 * D-pad auto-repeat tuning (carry-over from M3 polish; you can re-tune)
 * ──────────────────────────────────────────────────────────────────────
 * 3DS runs at 60 fps — 1 frame ≈ 16.7 ms.
 *
 * DPAD_INITIAL_DELAY: frames the user must hold a direction before
 *                     auto-repeat kicks in.
 * dpad_repeat_period(phase): frames between successive emissions.
 *                            Lower = faster.  Currently a 3-step ramp.
 */
#define DPAD_INITIAL_DELAY 25

static int dpad_repeat_period(int phase) {
    if (phase <  60) return 10;   /* first 1.0 s: ~6/sec */
    if (phase < 150) return 5;    /* next  1.5 s: ~12/sec */
    return 3;                     /* after 2.5 s: ~20/sec */
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

    /* R → toggle IME mode (no byte sent) */
    if (keys_down & KEY_R) {
        kbd->mode = (kbd->mode == MODE_EN) ? MODE_CN : MODE_EN;
        mark_event(kbd, kbd->mode == MODE_CN ? "R→C" : "R→E");
        return NULL;
    }

    /* Circle Pad scroll — same as M3 polish:
     *   deadband 80, sustained-push 6 frames before first event,
     *   then one event per 5 frames.  See M3 commit history for rationale. */
    int scroll_active = (circle_dy > 80 || circle_dy < -80);
    if (scroll_active) {
        kbd->scroll_timer++;
        int armed = (kbd->scroll_timer == 6) ||
                    (kbd->scroll_timer > 6 && (kbd->scroll_timer - 6) % 5 == 0);
        if (armed) {
            if (term && term->mouse_proto && term->mouse_sgr) {
                return emit_seq(kbd, circle_dy > 0
                                ? "\x1b[<64;1;1M"
                                : "\x1b[<65;1;1M");
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
            const char *seq;
            if      (dpad_active == KEY_DUP)    seq = "\x1b[A";
            else if (dpad_active == KEY_DDOWN)  seq = "\x1b[B";
            else if (dpad_active == KEY_DRIGHT) seq = "\x1b[C";
            else                                 seq = "\x1b[D";
            return emit_seq(kbd, seq);
        }
    }

    /* A → Enter */
    if (keys_down & KEY_A) {
        mark_event(kbd, "ENT");
        if (term && term->sb_offset) terminal_scroll_view(term, -term->sb_offset);
        return emit_byte(kbd, '\r');
    }
    /* B → Backspace, with the same auto-repeat ramp as D-pad so holding
     * B for a couple seconds rapid-deletes (super useful in Claude Code's
     * input editor). */
    if (keys_held & KEY_B) {
        if (keys_down & KEY_B) {
            kbd->b_held_frames = 0;
            mark_event(kbd, "BSP");
            return emit_byte(kbd, 0x7f);
        }
        kbd->b_held_frames++;
        if (kbd->b_held_frames >= DPAD_INITIAL_DELAY) {
            int phase = kbd->b_held_frames - DPAD_INITIAL_DELAY;
            int period = dpad_repeat_period(phase);
            if (period > 0 && phase % period == 0) {
                return emit_byte(kbd, 0x7f);
            }
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
