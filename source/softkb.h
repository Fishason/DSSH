#pragma once
#include "renderer.h"
#include "keyboard.h"

/*
 * On-screen touch keyboard for the 3DS bottom screen (M4).
 *
 * Two pages:
 *   PAGE_LETTERS   q w e r t y u i o p / a s d f g h j k l ' / z x c v b n m , . /
 *                   + page switch [123] + wide SPACE on the bottom row.
 *   PAGE_SYMBOLS   1 2 3 4 5 6 7 8 9 0 / ! @ # $ % ^ & * ( ) / - + = [ ] ; : ' " /
 *                   + page switch [abc] + ` < > | SPACE on the bottom row.
 *
 * Modifier keys (Shift / Ctrl / Alt) live on the *physical* shoulder/face
 * buttons (L=Shift, Y=Ctrl, X=Alt) — see keyboard.c.  The on-screen layout
 * therefore only carries character keys and a page-switch.
 *
 * Top row of the bottom screen is reserved for:
 *   [STA]                  candidate-area (IME, M7)              [EN/CN]
 *    └ keyboard_status_label()                                    └ mode
 */

typedef enum {
    PAGE_LETTERS = 0,
    PAGE_SYMBOLS,
} softkb_page_t;

typedef struct softkb_t softkb_t;

softkb_t *softkb_init(void);
void      softkb_free(softkb_t *kb);

/* Render the current page + status row to the bottom screen.  Must be
 * called inside C2D_SceneBegin(bottom_target). */
void softkb_draw(softkb_t *kb, renderer_t *r, const keyboard_t *kbd);

/* Process a touch event for the current frame.
 *   pressed=1 means the touch has just gone down (keys_down & KEY_TOUCH).
 *   pressed=0 means the touch is held or just released — we use this only
 *   to clear the visual "pressed" highlight; we don't fire on release.
 *
 * If a printable key was tapped, returns the byte sequence to send to
 * SSH (already passed through keyboard_emit_for so modifiers apply).
 * If a control key was tapped (page switch, etc.), returns NULL but the
 * internal state has been updated and the next softkb_draw will reflect
 * the change.
 *
 * Pass tx=ty=-1 when there is no current touch.  The function should be
 * called on every frame so the visual highlight follows touch state. */
const char *softkb_touch(softkb_t *kb,
                         keyboard_t *kbd,
                         int tx, int ty,
                         int pressed);

/* Currently displayed page (for diagnostic / layout testing). */
softkb_page_t softkb_current_page(const softkb_t *kb);

/* ── Debug page ─────────────────────────────────────────────────────
 *
 * Double-tapping the right-side ENG/CHN mode badge toggles into a full-
 * screen debug overlay that mirrors the M3 byte-tracing panel.  It
 * shows the last 32 received bytes in hex, every physical-key binding,
 * and a button to disable the bottom-row mascot.  Tap the badge once
 * to leave debug mode.
 */

/* Append received SSH bytes to the debug recv ring (last 32 bytes
 * shown).  Safe to call every frame; bytes < 1 are no-ops. */
void softkb_record_recv(softkb_t *kb, const char *bytes, int n);

/* True iff the keyboard is currently rendering the debug overlay
 * instead of the key layout.  When this returns 1, main.c should also
 * suppress the clock + mascot in the bottom row — the debug page
 * occupies the full bottom screen. */
int  softkb_in_debug(const softkb_t *kb);

/* Mascot enable flag — toggled from the debug page button.  Defaults
 * to 1 (mascot visible).  When 0, main.c should skip mascot_update
 * and mascot_draw so the crab stops moving and disappears. */
int  softkb_mascot_enabled(const softkb_t *kb);
