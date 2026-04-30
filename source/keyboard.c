#include "keyboard.h"
#include "terminal.h"
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────
 * D-pad auto-repeat tuning (USER CONTRIBUTION OPPORTUNITY)
 * ──────────────────────────────────────────────────────────────────────
 *
 * 3DS runs at 60 fps, so 1 frame = ~16.7 ms.
 *
 * DPAD_INITIAL_DELAY: how many frames the user must hold a direction
 *                     before auto-repeat kicks in.  Too short = a brisk
 *                     single tap fires twice; too long = feels laggy.
 *                     Common values: 20 (333ms) ... 36 (600ms).
 *
 * dpad_repeat_period(phase):
 *     given how many frames into the auto-repeat phase we are, return
 *     the number of frames between successive emissions.  Lower = faster.
 *     Multiple shapes possible:
 *       step:    if (phase < N) return slow; else if (...) return mid; ...
 *       linear:  return max(min, slow - phase / step)
 *       expo:    return max(min, slow >> (phase / decay))
 *
 * Quick reference of "events per second" at 60 fps:
 *     period = 12  →  5 events/sec   (slow scrolling, careful navigation)
 *     period = 6   →  10 events/sec  (medium, comfortable for prose)
 *     period = 4   →  15 events/sec
 *     period = 3   →  20 events/sec  (fast, for scanning long lines)
 *     period = 2   →  30 events/sec
 *
 * The current values are placeholders — try them on hardware and tune.
 */
#define DPAD_INITIAL_DELAY 25   /* ~417 ms hold before auto-repeat starts */

static int dpad_repeat_period(int phase) {
    /* TODO USER: pick the timing curve that feels best in your hands.
     * Below is a 3-step ramp; feel free to add more steps, change the
     * thresholds, or replace with a continuous formula. */
    if (phase <  60) return 10;   /* first 1.0 s: ~6 events/sec */
    if (phase < 150) return 5;    /* next 1.5 s: ~12 events/sec */
    return 3;                     /* after 2.5 s: ~20 events/sec */
}

keyboard_t *keyboard_init(void) {
    keyboard_t *k = calloc(1, sizeof(*k));
    return k;
}

void keyboard_free(keyboard_t *kbd) { free(kbd); }

/* Apply the Ctrl transformation to a printable ASCII char. Letters become
 * 0x01..0x1A; ?/[/\/]/^/_/space become their canonical Ctrl variants.
 * Returns the transformed char, or 0 if the input doesn't have a Ctrl form. */
static char to_ctrl(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 1;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
    if (c == ' ') return 0x00;        /* Ctrl-Space = NUL */
    if (c == '@') return 0x00;
    if (c == '[') return 0x1b;        /* Ctrl-[ = Esc */
    if (c == '\\') return 0x1c;
    if (c == ']') return 0x1d;
    if (c == '^') return 0x1e;
    if (c == '_') return 0x1f;
    if (c == '?') return 0x7f;        /* Ctrl-? = DEL */
    return 0;
}

/* Set out_buf to a literal byte sequence, return out_buf. */
static const char *emit(keyboard_t *k, const char *seq) {
    int n = (int)strlen(seq);
    if (n >= (int)sizeof(k->out_buf)) n = sizeof(k->out_buf) - 1;
    memcpy(k->out_buf, seq, n);
    k->out_buf[n] = 0;
    k->out_len    = n;
    return k->out_buf;
}

/* Set out_buf to a single byte, return out_buf. */
static const char *emit_byte(keyboard_t *k, char c) {
    k->out_buf[0] = c;
    k->out_buf[1] = 0;
    k->out_len    = 1;
    return k->out_buf;
}

/* Decide the byte for a "letter key" press, possibly applying Ctrl based on
 * sticky state or hold-L. Updates sticky state on consume. */
static const char *letter_with_modifiers(keyboard_t *k, char base, int l_held) {
    int apply_ctrl = l_held || k->sticky_ctrl == MOD_ARMED;
    if (apply_ctrl) {
        char c = to_ctrl(base);
        if (k->sticky_ctrl == MOD_ARMED) k->sticky_ctrl = MOD_OFF; /* one-shot */
        if (c == 0) return emit_byte(k, base);  /* fallback: send literal */
        return emit_byte(k, c);
    }
    return emit_byte(k, base);
}

const char *keyboard_handle_input(keyboard_t *kbd,
                                  struct terminal_t *term,
                                  u32 keys_down, u32 keys_held,
                                  int circle_dy) {
    if (!kbd) return NULL;
    kbd->out_len = 0;

    /* SELECT toggles ARMED. Press once to arm Ctrl for the next char,
     * press again to cancel. */
    if (keys_down & KEY_SELECT) {
        kbd->sticky_ctrl = (kbd->sticky_ctrl == MOD_OFF) ? MOD_ARMED : MOD_OFF;
        return NULL;
    }

    /* Emergency: L + Y sends 'q' to exit tmux copy mode.  If the user
     * accidentally entered copy mode via stick drift, this gets them
     * back to normal input quickly without having to use swkbd. */
    if ((keys_down & KEY_Y) && (keys_held & KEY_L)) {
        return emit_byte(kbd, 'q');
    }

    /* Circle Pad scrolls.  Two paths:
     *
     *   (a) Server has enabled xterm mouse tracking (tmux's `set -g mouse on`
     *       does this on startup): forward as a SGR wheel event so tmux's
     *       copy-mode kicks in transparently — same UX as PC terminals.
     *
     *   (b) No mouse tracking: scroll our local 500-row scrollback buffer.
     *
     * Two layers of debounce:
     *   - Deadband 80 (was 50): 3DS Circle Pads frequently drift to a static
     *     offset of 50-70.  Above 80 reliably signals an intentional push.
     *   - Startup delay of 6 frames before the first wheel event: a brief
     *     accidental bump (under ~100ms) won't fire any event.  This
     *     prevents stick taps from accidentally putting tmux into copy
     *     mode (where it eats Backspace and looks like "B doesn't delete").
     *
     * After the startup delay, throttle to ~12 ticks/sec for sustained
     * scrolls.
     *
     * Falls through (not return NULL) so other key handlers below still
     * run — earlier the scroll branch was eating B/A/D-pad whenever the
     * stick was past deadband. */
    int scroll_active = (circle_dy > 80 || circle_dy < -80);
    if (scroll_active) {
        kbd->scroll_timer++;
        /* Need a sustained push of >= 6 frames before the first event,
         * then one event every 5 frames after.  This filters out
         * accidental bumps from putting tmux into copy mode. */
        int armed = (kbd->scroll_timer == 6) ||
                    (kbd->scroll_timer > 6 && (kbd->scroll_timer - 6) % 5 == 0);
        if (armed) {
            if (term && term->mouse_proto && term->mouse_sgr) {
                /* Wheel: button 64 = up, 65 = down at fake col/row 1,1. */
                return emit(kbd, circle_dy > 0
                            ? "\x1b[<64;1;1M"
                            : "\x1b[<65;1;1M");
            }
            if (term) terminal_scroll_view(term, circle_dy > 0 ? 1 : -1);
        }
        /* don't `return NULL` — other key handlers below should still run. */
    } else {
        kbd->scroll_timer = 0;
    }

    int l_held = (keys_held & KEY_L) ? 1 : 0;

    /* ── D-pad with auto-repeat ─────────────────────────────────────────
     * Long-pressing a direction should keep firing arrow keys, accelerating
     * over time (slow at first so a quick tap doesn't double-fire,
     * faster after a sustained hold so you can scan through long lines).
     *
     * Algorithm:
     *   - Snapshot which D-pad direction is currently held (single dir).
     *   - If the direction differs from last frame, reset dpad_frames to 0.
     *   - Frame 0:  fire immediately (responsive single tap).
     *   - Frames 1..DPAD_INITIAL_DELAY-1:  do nothing (hold detection).
     *   - From DPAD_INITIAL_DELAY onward: fire every dpad_repeat_period(phase)
     *     frames where phase = frames since the repeat phase started.
     *
     * Tunable user contribution point below — see dpad_repeat_period(). */
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
            return emit(kbd, seq);
        }
        /* Held but not firing this frame — fall through so other keys
         * can still be processed (A/B/X/etc. while D-pad is held). */
    }

    /* Primary inputs */
    if (keys_down & KEY_A) {
        const char *r = letter_with_modifiers(kbd, '\r', l_held);
        if (term) terminal_scroll_view(term, -term->sb_offset);
        return r;
    }
    if (keys_down & KEY_B) {
        /* Backspace; 0x7f is what most terminals expect. With Ctrl, send
         * Ctrl-? (= 0x7f anyway), so unaffected. */
        return emit_byte(kbd, 0x7f);
    }
    if (keys_down & KEY_X) {
        /* M3 reserves X for the swkbd applet; main.c handles that since
         * applet popping needs the SwkbdState struct lifecycle.
         * Signal to main.c via a sentinel: emit single 0x01 here? No, that
         * conflicts with Ctrl-A. Instead, return NULL and let main.c
         * separately check (down & KEY_X). */
        return NULL;
    }
    if (keys_down & KEY_R) {
        /* R is a hold-style "secondary" that, alone, sends nothing; in
         * combination it could be a future modifier. For M3 we leave it
         * unbound to avoid surprise. */
        return NULL;
    }

    return NULL;
}

const char *keyboard_mod_label(const keyboard_t *kbd) {
    if (!kbd) return "    ";
    return (kbd->sticky_ctrl == MOD_ARMED) ? "CTL " : "    ";
}

int keyboard_apply_modifiers(keyboard_t *kbd, char *buf, int len) {
    if (!kbd || !buf || len <= 0) return len;
    if (kbd->sticky_ctrl == MOD_ARMED) {
        char c = to_ctrl(buf[0]);
        if (c) buf[0] = c;
        kbd->sticky_ctrl = MOD_OFF;
    }
    return len;
}
