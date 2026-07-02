#ifndef VOICE_H
#define VOICE_H

#include <stdint.h>

/* Voice-input subsystem.  Records 16 kHz PCM16 mono via the 3DS mic into
 * an aligned heap buffer, then streams the bytes over an auxiliary
 * libssh2 channel to `dssh-whisper-shim` on the server side, reads back
 * the transcribed Chinese UTF-8 text, and forwards it to the main SSH
 * shell channel as if the user had typed it.
 *
 * State machine, driven by physical KEY_START presses:
 *   IDLE   ──► RECORDING  (mic init + start sampling)
 *   RECORDING ──► TRANSCRIBING (stop mic, open aux channel, exec shim)
 *   TRANSCRIBING ──► IDLE (response received, text forwarded to ssh_write)
 *   ANY ──► ERROR ──► IDLE (auto-decay after ~2 s)
 *
 * Recording auto-stops after ~7 s (frame counter) before the 8 s mic
 * buffer overruns; user can also press START a second time to end
 * recording at any point.
 */

typedef struct ssh_client_t ssh_client_t;
typedef struct voice_t      voice_t;

typedef enum {
    VOICE_IDLE = 0,
    VOICE_RECORDING,
    VOICE_TRANSCRIBING,
    /* Non-AI transcription received; the reply text is being streamed
     * back to the SSH shell one UTF-8 character at a time (typewriter
     * effect) rather than dumped all at once.  Per-char delay scales
     * inversely with total length so long utterances don't crawl. */
    VOICE_TYPING,
    /* M12: AI-ask modal is showing.  voice_state_frame() drives the
     * 0.5 s fade-in and the modal renderer.  Stays in this state until
     * voice_ai_close_keep() (A) or voice_ai_close_clear() (B / touch). */
    VOICE_AI_SHOWING,
    VOICE_ERROR,
} voice_state_t;

voice_t *voice_init(void);
void     voice_free(voice_t *v);

/* Triggered by physical KEY_START.
 *   IDLE         → RECORDING
 *   RECORDING    → TRANSCRIBING (or IDLE if too short)
 *   TRANSCRIBING → IDLE (cancel)
 *   ERROR        → IDLE (clear error)                                  */
void voice_toggle(voice_t *v, ssh_client_t *ssh);

/* M12: triggered by physical L + KEY_START.  Same shape as voice_toggle
 * but flips an internal `ai_mode` flag — the recording is destined for
 * the AI-ask modal (DeepSeek via dssh-whisper-shim --ask) instead of
 * being typed into the SSH shell. */
void voice_ai_toggle(voice_t *v, ssh_client_t *ssh);

/* Modal dismiss while in VOICE_AI_SHOWING.
 *   close_keep  — A pressed.  Append (Q,A) to history; next L+START
 *                 sends accumulated history.
 *   close_clear — B pressed or touch anywhere on bottom screen.
 *                 Discard history; next L+START is a fresh chat.    */
void voice_ai_close_keep(voice_t *v);
void voice_ai_close_clear(voice_t *v);

/* Modal content accessors (read-only). */
const char *voice_ai_question(const voice_t *v);
const char *voice_ai_answer(const voice_t *v);
int         voice_ai_history_count(const voice_t *v);

/* True iff the current cycle is destined for AI ask (used by softkb to
 * pick a different status badge). */
int  voice_in_ai_cycle(const voice_t *v);

/* Per-frame: drives the recording-length cap, the non-blocking aux
 * channel write/eof/read sequence, and the error decay timer. */
void voice_tick(voice_t *v, ssh_client_t *ssh);

voice_state_t voice_state(const voice_t *v);
int           voice_state_frame(const voice_t *v);

/* Returns a 3-character UTF-8 status label for the softkb top-left
 * status slot, or NULL when IDLE (caller falls back to keyboard's
 * modifier label). */
const char *voice_status_label(const voice_t *v);

/* True iff the typewriter streamed at least one glyph since the last
 * call to this function (latches + clears).  main.c uses it to kick the
 * mascot's typing pose in sync with voice TYPING output, which bypasses
 * send_to_ssh (voice.c talks to ssh_write directly). */
int voice_consume_typed(voice_t *v);

/* Background tint for the status slot during voice activity (RGBA);
 * 0 = no tint (use the caller's default). */
uint32_t   voice_status_bg(const voice_t *v);

/* Foreground colour for the status label (RGBA). */
uint32_t   voice_status_fg(const voice_t *v);

#endif /* VOICE_H */
