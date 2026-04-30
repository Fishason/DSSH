#pragma once
#include <stdint.h>

/*
 * Pinyin IME engine for 3dssh M7.
 *
 * Loads a binary dictionary produced by tools/gen_pinyin_dict.py
 * (rime-ice top-300k entries, sorted by pinyin) and serves candidates
 * for prefix matches as the user types pinyin letters.
 *
 * Lifecycle:
 *   ime_t *ime = ime_init("romfs:/pinyin_dict.bin");
 *   ...
 *   ime_input_letter(ime, 'n');
 *   ime_input_letter(ime, 'i');
 *   const char *first = ime_candidate(ime, 0);   // 你
 *   const char *got   = ime_select(ime, 0);      // commits, clears buffer
 *   send_to_ssh(got, strlen(got));
 *   ime_free(ime);
 *
 * All returned strings are pointers into the dict's word pool — they
 * stay valid for the IME's lifetime.  Callers should NOT free them.
 *
 * The implementation is single-threaded.  Both the dict load (~9 MB
 * into heap) and per-keystroke refresh do happen synchronously; the
 * main loop should still hit 60 fps (refresh worst case ~5 ms).
 */

#define IME_BUFFER_MAX     31     /* max pinyin letters before refusing */
#define IME_PAGE_SIZE       5     /* candidates shown per page (fits the
                                   * 260 px candidate strip even when
                                   * each candidate is 3 CJK chars) */
#define IME_MAX_CANDIDATES 256    /* total kept after sort, paginated */

typedef struct ime_t ime_t;

/* Open the binary dict at `path` and return an opaque IME handle.
 * Returns NULL on file-not-found, bad magic, OOM, etc. */
ime_t *ime_init(const char *path);
void   ime_free(ime_t *ime);

/* Buffer manipulation.  Letters must be ASCII a-z.  The buffer is
 * silently capped at IME_BUFFER_MAX. */
void   ime_input_letter(ime_t *ime, char c);
void   ime_backspace(ime_t *ime);
void   ime_clear(ime_t *ime);

/* Read-only buffer state. */
const char *ime_buffer(const ime_t *ime);     /* current pinyin string */
int   ime_buffer_len(const ime_t *ime);
int   ime_active(const ime_t *ime);            /* buffer non-empty */

/* Length of the buffer prefix that actually produced the candidates.
 * Equals ime_buffer_len() in the normal case; smaller when the user
 * typed past a valid prefix and the engine fell back (e.g. typed
 * "nihaoz" but only "nihao" matches → returns 5).  Returns 0 when
 * no prefix yielded any matches. */
int   ime_matched_prefix_len(const ime_t *ime);

/* Candidate access — current page (paginate via ime_page_next/prev).
 * Indices are 0..IME_PAGE_SIZE-1 within the current page. */
int   ime_candidate_count(const ime_t *ime);          /* visible on this page */
const char *ime_candidate(const ime_t *ime, int idx); /* NUL-terminated UTF-8 */

/* Total candidates and pagination. */
int   ime_total_candidates(const ime_t *ime);
/* Absolute (across-pages) candidate access — useful for tests and any
 * UI that needs to scan beyond the visible page. */
const char *ime_candidate_at(const ime_t *ime, int abs_idx);
int   ime_page(const ime_t *ime);
int   ime_page_count(const ime_t *ime);
void  ime_page_next(ime_t *ime);                /* resets selection to 0 */
void  ime_page_prev(ime_t *ime);                /* resets selection to 0 */

/* Per-page selection cursor.  Points at the candidate that A/Space
 * commits.  Defaults to 0 after each refresh / page change.  Left/Right
 * cursors clamp at the page edges (use page navigation to reach more). */
int   ime_selection_idx(const ime_t *ime);
void  ime_selection_left(ime_t *ime);
void  ime_selection_right(ime_t *ime);

/* Commit the candidate at the given page-relative index `idx`, or at
 * the current selection.  Both clear the buffer.  Returned pointer is
 * stable for the dict's lifetime; NULL on out-of-range. */
const char *ime_select(ime_t *ime, int idx);
const char *ime_select_current(ime_t *ime);
