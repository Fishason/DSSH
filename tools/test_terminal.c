#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../source/terminal.h"

/* terminal.c only needs this one font-atlas query.  Protocol tests use ASCII. */
int font_is_wide(uint32_t codepoint) {
    (void)codepoint;
    return 0;
}

static int failures = 0;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

static char cell_char(terminal_t *term, int x, int y) {
    return (char)terminal_get_cell(term, x, y).codepoint;
}

int main(void) {
    terminal_t *term = terminal_init(4, 3);
    CHECK(term != NULL, "terminal_init failed");
    if (!term) return 1;

    /* A fourth line must scroll the active screen, leaving input on the
     * physical bottom row instead of repainting at row 1. */
    terminal_write(term, "1\r\n2\r\n3\r\n4");
    CHECK(cell_char(term, 0, 0) == '2', "scroll row 1 should contain 2");
    CHECK(cell_char(term, 0, 1) == '3', "scroll row 2 should contain 3");
    CHECK(cell_char(term, 0, 2) == '4', "scroll row 3 should contain 4");
    CHECK(term->cur_y == 2 && term->cur_x == 1,
          "cursor should remain after 4 on the bottom row");

    /* fish uses CPR (CSI 6n) to decide where to redraw its input line. */
    terminal_write(term, "\x1b[6n");
    char reply[128] = {0};
    int n = terminal_take_response(term, reply, sizeof(reply));
    CHECK(n == 6 && memcmp(reply, "\x1b[3;2R", 6) == 0,
          "CSI 6n should report row 3, column 2");
    CHECK(terminal_take_response(term, reply, sizeof(reply)) == 0,
          "terminal response should be consumed");

    /* Local startup banners are discarded before the remote PTY begins; CPR
     * must then use the remote screen's clean origin, not the local banner's
     * former cursor row. */
    terminal_reset(term);
    terminal_write(term, "\x1b[6n");
    memset(reply, 0, sizeof(reply));
    n = terminal_take_response(term, reply, sizeof(reply));
    CHECK(n == 6 && memcmp(reply, "\x1b[1;1R", 6) == 0,
          "terminal reset should restore remote CPR origin");

    terminal_write(term, "\x1b[5n\x1b[c\x1b[>c");
    memset(reply, 0, sizeof(reply));
    n = terminal_take_response(term, reply, sizeof(reply));
    CHECK(n == 20 &&
          memcmp(reply, "\x1b[0n\x1b[?1;2c\x1b[>0;0;0c", 20) == 0,
          "status and device-attribute replies should be queued in order");

    /* Fish 4 enables xterm modifyOtherKeys and Kitty keyboard enhancement at
     * every prompt. CSI = 5 u is not CSI u (restore cursor): confusing the
     * two is what made fish repaint all typed input on terminal row 1. */
    terminal_write(term, "prompt\r\n> ");
    int fish_x = term->cur_x;
    int fish_y = term->cur_y;
    uint8_t fish_flags = term->cur_flags;
    terminal_write(term, "\x1b[>4;1m\x1b[=5u");
    CHECK(term->cur_x == fish_x && term->cur_y == fish_y,
          "fish keyboard protocol must not restore cursor to row 1");
    CHECK(term->cur_flags == fish_flags,
          "xterm modifyOtherKeys must not be parsed as SGR");

    /* Startup synchronization markers travel inside OSC so the raw SSH
     * bootstrap can detect them without leaking READY text onto the screen. */
    terminal_write(term, "\x1b]777;DSSH_READY_SHELL\x07");
    CHECK(term->cur_x == fish_x && term->cur_y == fish_y,
          "OSC readiness marker must not move the cursor");
    CHECK(cell_char(term, fish_x, fish_y) == ' ',
          "OSC readiness marker must not render visible text");

    terminal_write(term, "\x1b[s\x1b[1;1H\x1b[u");
    CHECK(term->cur_x == fish_x && term->cur_y == fish_y,
          "parameterless CSI s/u should still save and restore cursor");

    terminal_free(term);
    if (failures) {
        fprintf(stderr, "%d terminal test(s) failed\n", failures);
        return 1;
    }
    puts("terminal tests passed");
    return 0;
}
