#include "ime_pinyin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * On-device pinyin IME engine.
 *
 * The dict binary (tools/gen_pinyin_dict.py output) is loaded fully
 * into heap at startup — ~9 MB for 300k entries, well within the 3DS
 * 64 MB heap.  We keep direct pointers into the blob; nothing is
 * copied.  Engine state is just the input buffer plus a candidate list.
 *
 * Lookup flow per keystroke:
 *
 *   1. Binary-search the entry table for the lower-bound of the
 *      current buffer (entries are pre-sorted by pinyin).
 *   2. Walk forward from there while pinyin starts with buffer.
 *   3. qsort the gathered entries by frequency descending.
 *   4. Take the top 256 (most common) as the candidate list, then
 *      paginate 7 at a time.
 *
 * Worst case (single-letter prefix like "n") gathers ~16k entries and
 * qsort takes ~2 ms on the 268 MHz ARM11.  Typical (2-3 letter buffer)
 * gathers under 500 entries, refreshes in <0.2 ms.
 */

#define IME_GATHER_MAX  16384  /* cap per-prefix gather before qsort */

struct __attribute__((packed)) dict_header {
    char     magic[4];
    uint32_t version;
    uint32_t n_entries;
    uint32_t pinyin_pool_off;
    uint32_t pinyin_pool_len;
    uint32_t word_pool_off;
    uint32_t word_pool_len;
};

struct __attribute__((packed)) dict_entry {
    uint32_t pinyin_off;
    uint32_t word_off;
    uint32_t freq;
};

struct ime_t {
    /* Loaded dictionary blob — owned, freed in ime_free. */
    uint8_t *blob;
    size_t   blob_size;
    /* Pointers into the blob — not owned. */
    const struct dict_header *hdr;
    const struct dict_entry  *entries;
    const char *pinyin_pool;
    const char *word_pool;

    /* Current pinyin input. */
    char buffer[IME_BUFFER_MAX + 1];
    int  buffer_len;

    /* Candidates for the current buffer.  We gather up to 16k matching
     * entries, qsort by freq desc, then keep the top 256 as
     * `candidates` (pointers into word_pool).  Pagination is purely a
     * view onto this fixed array. */
    const struct dict_entry *gathered[IME_GATHER_MAX];
    int n_gathered;

    const char *candidates[IME_MAX_CANDIDATES];
    int  n_candidates;
    int  page;
};

/* ── helpers ─────────────────────────────────────────────────────── */

static const char *entry_pinyin(const ime_t *ime, int idx) {
    return &ime->pinyin_pool[ime->entries[idx].pinyin_off];
}

static int compare_freq_desc(const void *a, const void *b) {
    const struct dict_entry *ea = *(const struct dict_entry *const *)a;
    const struct dict_entry *eb = *(const struct dict_entry *const *)b;
    /* Higher freq first.  Don't subtract — uint32 wraparound bites. */
    if (ea->freq > eb->freq) return -1;
    if (ea->freq < eb->freq) return 1;
    return 0;
}

/* lower_bound for pinyin string — returns first index whose pinyin
 * compares >= prefix lexicographically.  All matching-prefix entries
 * sit in [lo, walk_end_until_no_match). */
static int lower_bound_pinyin(const ime_t *ime, const char *prefix) {
    int lo = 0, hi = (int)ime->hdr->n_entries;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (strcmp(entry_pinyin(ime, mid), prefix) < 0) lo = mid + 1;
        else                                            hi = mid;
    }
    return lo;
}

static void refresh_candidates(ime_t *ime) {
    ime->n_gathered   = 0;
    ime->n_candidates = 0;
    ime->page         = 0;
    if (ime->buffer_len == 0) return;

    int total = (int)ime->hdr->n_entries;
    int lo    = lower_bound_pinyin(ime, ime->buffer);
    for (int i = lo;
         i < total && ime->n_gathered < IME_GATHER_MAX;
         i++) {
        const char *py = entry_pinyin(ime, i);
        if (strncmp(py, ime->buffer, (size_t)ime->buffer_len) != 0) break;
        ime->gathered[ime->n_gathered++] = &ime->entries[i];
    }
    if (ime->n_gathered == 0) return;

    qsort(ime->gathered, (size_t)ime->n_gathered,
          sizeof(ime->gathered[0]), compare_freq_desc);

    int take = ime->n_gathered < IME_MAX_CANDIDATES
             ? ime->n_gathered : IME_MAX_CANDIDATES;
    for (int i = 0; i < take; i++) {
        ime->candidates[i] = &ime->word_pool[ime->gathered[i]->word_off];
    }
    ime->n_candidates = take;
}

/* ── lifecycle ───────────────────────────────────────────────────── */

ime_t *ime_init(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) < 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= (long)sizeof(struct dict_header)) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    ime_t *ime = calloc(1, sizeof(*ime));
    if (!ime) { fclose(f); return NULL; }
    ime->blob = malloc((size_t)sz);
    if (!ime->blob) { fclose(f); free(ime); return NULL; }
    size_t got = fread(ime->blob, 1, (size_t)sz, f);
    fclose(f);
    if ((long)got != sz) {
        free(ime->blob); free(ime);
        return NULL;
    }
    ime->blob_size = (size_t)sz;
    ime->hdr = (const struct dict_header *)ime->blob;
    if (memcmp(ime->hdr->magic, "PYIN", 4) != 0 || ime->hdr->version != 1) {
        free(ime->blob); free(ime);
        return NULL;
    }
    ime->entries =
        (const struct dict_entry *)(ime->blob + sizeof(struct dict_header));
    ime->pinyin_pool = (const char *)(ime->blob + ime->hdr->pinyin_pool_off);
    ime->word_pool   = (const char *)(ime->blob + ime->hdr->word_pool_off);
    return ime;
}

void ime_free(ime_t *ime) {
    if (!ime) return;
    free(ime->blob);
    free(ime);
}

/* ── buffer ──────────────────────────────────────────────────────── */

void ime_input_letter(ime_t *ime, char c) {
    if (!ime || c < 'a' || c > 'z') return;
    if (ime->buffer_len >= IME_BUFFER_MAX) return;
    ime->buffer[ime->buffer_len++] = c;
    ime->buffer[ime->buffer_len]   = 0;
    refresh_candidates(ime);
}

void ime_backspace(ime_t *ime) {
    if (!ime || ime->buffer_len == 0) return;
    ime->buffer[--ime->buffer_len] = 0;
    refresh_candidates(ime);
}

void ime_clear(ime_t *ime) {
    if (!ime) return;
    ime->buffer[0]    = 0;
    ime->buffer_len   = 0;
    ime->n_gathered   = 0;
    ime->n_candidates = 0;
    ime->page         = 0;
}

const char *ime_buffer(const ime_t *ime) {
    return ime ? ime->buffer : "";
}
int ime_buffer_len(const ime_t *ime) {
    return ime ? ime->buffer_len : 0;
}
int ime_active(const ime_t *ime) {
    return ime && ime->buffer_len > 0;
}

/* ── candidates / pagination ─────────────────────────────────────── */

int ime_total_candidates(const ime_t *ime) {
    return ime ? ime->n_candidates : 0;
}

int ime_candidate_count(const ime_t *ime) {
    if (!ime || ime->n_candidates == 0) return 0;
    int start     = ime->page * IME_PAGE_SIZE;
    int remaining = ime->n_candidates - start;
    if (remaining <= 0)              return 0;
    if (remaining < IME_PAGE_SIZE)   return remaining;
    return IME_PAGE_SIZE;
}

const char *ime_candidate(const ime_t *ime, int idx) {
    if (!ime || idx < 0)                       return NULL;
    int abs = ime->page * IME_PAGE_SIZE + idx;
    if (abs >= ime->n_candidates)              return NULL;
    return ime->candidates[abs];
}

int ime_page(const ime_t *ime) {
    return ime ? ime->page : 0;
}

int ime_page_count(const ime_t *ime) {
    if (!ime || ime->n_candidates == 0) return 0;
    return (ime->n_candidates + IME_PAGE_SIZE - 1) / IME_PAGE_SIZE;
}

void ime_page_next(ime_t *ime) {
    if (!ime) return;
    int max_page = ime_page_count(ime) - 1;
    if (max_page < 0) return;
    if (ime->page < max_page) ime->page++;
}

void ime_page_prev(ime_t *ime) {
    if (!ime) return;
    if (ime->page > 0) ime->page--;
}

const char *ime_select(ime_t *ime, int idx) {
    const char *committed = ime_candidate(ime, idx);
    if (!committed) return NULL;
    ime_clear(ime);
    return committed;
}
