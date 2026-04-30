/*
 * Host-side smoke test for source/ime_pinyin.c.
 *
 * Compile and run via tools/test_ime.sh.  Catches regressions in the
 * dict format, lookup logic, and pagination without needing a 3DS push.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/ime_pinyin.h"

#define DICT_PATH_DEFAULT "romfs/pinyin_dict.bin"

static int failures = 0;

#define FAIL(msg, ...) do { \
    fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
    failures++; \
} while (0)

static int has_candidate(const ime_t *ime, const char *expected) {
    int n = ime_candidate_count(ime);
    for (int i = 0; i < n; i++) {
        if (strcmp(ime_candidate(ime, i), expected) == 0) return i;
    }
    return -1;
}

static void print_candidates(const ime_t *ime, const char *label) {
    int n = ime_candidate_count(ime);
    printf("  %-12s →", label);
    for (int i = 0; i < n; i++) printf(" %s", ime_candidate(ime, i));
    printf("   (page %d/%d, total %d)\n",
           ime_page(ime) + 1, ime_page_count(ime),
           ime_total_candidates(ime));
}

static void type(ime_t *ime, const char *s) {
    for (; *s; s++) ime_input_letter(ime, *s);
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : DICT_PATH_DEFAULT;
    ime_t *ime = ime_init(path);
    if (!ime) {
        fprintf(stderr,
            "FATAL: ime_init(\"%s\") failed.\n"
            "  Run `python3 tools/gen_pinyin_dict.py` first to build it.\n",
            path);
        return 1;
    }
    printf("Loaded dict: %s\n", path);

    /* ── 1. ni → 你 in candidates ───────────────────────────── */
    ime_clear(ime);
    type(ime, "ni");
    print_candidates(ime, "ni");
    if (has_candidate(ime, "你") < 0) FAIL("'你' not in 'ni' candidates");

    /* ── 2. nihao → 你好 in top results ──────────────────────── */
    ime_clear(ime);
    type(ime, "nihao");
    print_candidates(ime, "nihao");
    int idx = has_candidate(ime, "你好");
    if (idx < 0) FAIL("'你好' not in 'nihao' candidates");
    if (idx > 3) FAIL("'你好' should be in top 4 for nihao, got idx %d", idx);

    /* ── 3. shijie → 世界 reachable ──────────────────────────── */
    ime_clear(ime);
    type(ime, "shijie");
    print_candidates(ime, "shijie");
    if (has_candidate(ime, "世界") < 0) FAIL("'世界' not in 'shijie' candidates");

    /* ── 4. wo → 我 should be top candidate (very high freq) ── */
    ime_clear(ime);
    type(ime, "wo");
    print_candidates(ime, "wo");
    if (has_candidate(ime, "我") < 0) FAIL("'我' not in 'wo' candidates");

    /* ── 5. ime_select returns committed string + clears buffer ── */
    ime_clear(ime);
    type(ime, "nihao");
    const char *got = ime_select(ime, 0);
    if (!got) {
        FAIL("ime_select(0) returned NULL");
    } else {
        printf("  selected from nihao → %s\n", got);
        if (ime_buffer_len(ime) != 0) FAIL("buffer not cleared after select");
    }

    /* ── 6. backspace narrows / restores prefix ───────────────── */
    ime_clear(ime);
    type(ime, "wome");
    int n_wome = ime_candidate_count(ime);
    ime_backspace(ime);
    if (strcmp(ime_buffer(ime), "wom") != 0) {
        FAIL("buffer should be 'wom' after bs, got '%s'", ime_buffer(ime));
    }
    int n_wom = ime_candidate_count(ime);
    /* Sanity: a shorter prefix usually has more candidates (not strictly
     * true but is here).  Just print for inspection. */
    printf("  wome n=%d, after bs wom n=%d\n", n_wome, n_wom);

    /* ── 7. 'n' has many pages ────────────────────────────────── */
    ime_clear(ime);
    type(ime, "n");
    if (ime_page_count(ime) < 2)
        FAIL("'n' should have multiple pages, got %d", ime_page_count(ime));
    print_candidates(ime, "n p0");
    ime_page_next(ime);
    print_candidates(ime, "n p1");
    if (ime_page(ime) != 1) FAIL("page should be 1 after page_next");
    ime_page_prev(ime);
    if (ime_page(ime) != 0) FAIL("page should be 0 after page_prev");

    /* ── 8. nonsense prefix gives empty list, not crash ───────── */
    ime_clear(ime);
    type(ime, "zzzzz");
    if (ime_candidate_count(ime) != 0)
        FAIL("'zzzzz' should yield no candidates");

    /* ── 9. several common everyday words ─────────────────────── */
    const char *cases[][2] = {
        { "zhongwen",  "中文" },
        { "yingwen",   "英文" },
        { "diannao",   "电脑" },
        { "shoufuji",  "首富级" },     /* may or may not be in top */
        { "shouji",    "手机" },
        { "xiexie",    "谢谢" },
        { "huanying",  "欢迎" },
        { "yemian",    "页面" },
        { NULL, NULL },
    };
    int hits = 0, hit_top = 0;
    for (int i = 0; cases[i][0]; i++) {
        ime_clear(ime);
        type(ime, cases[i][0]);
        int found = has_candidate(ime, cases[i][1]);
        if (found >= 0) {
            hits++;
            if (found < 3) hit_top++;
            printf("  %s → %s (idx=%d)\n",
                   cases[i][0], cases[i][1], found);
        } else {
            printf("  %s → ✗ '%s' not found in candidates:",
                   cases[i][0], cases[i][1]);
            int n = ime_candidate_count(ime);
            for (int j = 0; j < n; j++) printf(" %s", ime_candidate(ime, j));
            printf("\n");
        }
    }
    printf("  %d/8 expected words found, %d in top-3.\n", hits, hit_top);

    ime_free(ime);

    if (failures > 0) {
        fprintf(stderr, "\n%d failures\n", failures);
        return 1;
    }
    printf("\nAll tests passed.\n");
    return 0;
}
