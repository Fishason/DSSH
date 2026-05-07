#ifndef AI_MODAL_H
#define AI_MODAL_H

/* Rounded-corner bottom-screen modal that displays the (question, answer)
 * returned by the M12 voice AI-ask path.  Sits over the soft keyboard,
 * 6 px gap from the screen edges, dims everything behind it with a 60 %
 * black overlay, and fades in over 0.5 s when ai_modal_open() is called.
 *
 * Inputs (q + a) are *copied*; the caller can free them afterwards.
 *
 * Interaction is handled in main.c:
 *   A      → caller calls ai_modal_close() + voice_ai_close_keep()
 *   B      → caller calls ai_modal_close() + voice_ai_close_clear()
 *   touch  → same as B
 *
 * The modal itself does not consume input — main.c routes those events.
 */

typedef struct ai_modal_t ai_modal_t;

ai_modal_t *ai_modal_init(void);
void        ai_modal_free(ai_modal_t *m);

/* Begin showing the modal (resets to start of fade-in animation).
 * Strings are copied internally up to a fixed cap; long answers are
 * truncated with a trailing "…". */
void ai_modal_open(ai_modal_t *m, const char *question, const char *answer);

/* Begin fade-out animation (~0.2 s).  Once it finishes, ai_modal_visible
 * will start returning 0 and the soft keyboard regains its surface.
 * Idempotent. */
void ai_modal_close(ai_modal_t *m);

/* Advance per-frame state (animation timers).  Call once per frame. */
void ai_modal_tick(ai_modal_t *m);

/* Render the modal — must be called inside C2D_SceneBegin(bottom_target),
 * AFTER the soft keyboard / clock / mascot have already drawn so this
 * lays over them. */
void ai_modal_draw(ai_modal_t *m);

/* True iff the modal currently occupies the bottom screen (including
 * fade-in / fade-out animation frames).  main.c uses this to suppress
 * key-routing and touch routing during the modal lifecycle. */
int  ai_modal_visible(const ai_modal_t *m);

#endif /* AI_MODAL_H */
