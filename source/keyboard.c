#include "keyboard.h"
#include "terminal.h"
#include <stdlib.h>
#include <string.h>

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

    /* Circle Pad always scrolls the local scrollback buffer (no mode toggle).
     * Throttled so a sustained push scrolls ~12 lines/sec.  Any subsequent
     * key press will snap the view back to the bottom (handled in main.c
     * via terminal_scroll_view(-sb_offset) after every produced byte). */
    if (term && (circle_dy > 50 || circle_dy < -50)) {
        if (++kbd->scroll_timer >= 5) {
            kbd->scroll_timer = 0;
            terminal_scroll_view(term, circle_dy > 0 ? 1 : -1);
        }
        return NULL;
    } else {
        kbd->scroll_timer = 0;
    }

    int l_held = (keys_held & KEY_L) ? 1 : 0;

    /* D-pad arrows (raw ANSI sequences, no Ctrl). */
    if (keys_down & KEY_DUP)    return emit(kbd, "\x1b[A");
    if (keys_down & KEY_DDOWN)  return emit(kbd, "\x1b[B");
    if (keys_down & KEY_DRIGHT) return emit(kbd, "\x1b[C");
    if (keys_down & KEY_DLEFT)  return emit(kbd, "\x1b[D");

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
