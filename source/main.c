/*
 * 3dssh — Nintendo 3DS SSH client (M3).
 *
 * M3 brings:
 *   - citro2d-based top-screen ANSI terminal (replaces libctru consoleInit)
 *   - bitmap font atlas with full ANSI 256/TrueColor + box-drawing
 *   - bottom-screen status panel (citro2d) with modifier-state indicators
 *   - sticky Ctrl modifier state machine driven by SELECT
 *   - Circle Pad scrollback navigation (Y toggles scroll mode)
 *   - X still pops system swkbd applet for typing (M4 will replace with
 *     a custom on-screen keyboard)
 *
 * Single-threaded polling main loop. Memory budget for terminal+scrollback
 * is ~512KB, plus ~580KB for font_data, plus libssh2/mbedtls — total well
 * under 64MB default heap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <3ds.h>
#include <citro2d.h>

#include "ssh_client.h"
#include "config.h"
#include "terminal.h"
#include "renderer.h"
#include "keyboard.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

#define CONFIG_PATH     "sdmc:/3ds/3dssh/config.ini"
#define READ_BUFSZ      2048

#define BG_TOP          0x1e1e2eff
#define BG_BOT          0x181825ff
#define COLOR_OK        0xa6e3a1ff   /* green */
#define COLOR_WARN      0xfab387ff   /* peach */
#define COLOR_ERR       0xf38ba8ff   /* red */
#define COLOR_FG        0xcdd6f4ff   /* default text */
#define COLOR_DIM       0x6c7086ff   /* dim grey */
#define COLOR_ACCENT    0x89b4faff   /* blue accent */

static u32 *soc_buf = NULL;

static int net_init(char *err, int err_sz) {
    soc_buf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!soc_buf) { snprintf(err, err_sz, "memalign failed"); return -1; }
    int rc = socInit(soc_buf, SOC_BUFFERSIZE);
    if (rc != 0) { snprintf(err, err_sz, "socInit 0x%08lX", (unsigned long)rc); return -1; }
    return 0;
}

static void net_fini(void) {
    socExit();
    if (soc_buf) { free(soc_buf); soc_buf = NULL; }
}

/* Pop the system soft keyboard, return UTF-8 input or empty string on cancel. */
static int prompt_swkbd(const char *hint, char *out, int out_sz) {
    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, hint);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT,  "Cancel", false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "Send",   true);
    SwkbdButton btn = swkbdInputText(&swkbd, out, out_sz);
    if (btn != SWKBD_BUTTON_RIGHT) { out[0] = 0; return 0; }
    return 1;
}

/* UTF-8 fragment buffer: SSH packet boundaries can split a multibyte char.
 * Stash up to 3 trailing bytes for the next read. */
static char utf8_frag[4];
static int  utf8_frag_len = 0;

/* Diagnostic: ring of last bytes sent / received so the user can verify on
 * the bottom panel what's actually going over the wire when keys don't
 * seem to be doing what they should. */
#define DBG_LOG_LEN 8
static unsigned char dbg_sent[DBG_LOG_LEN];
static int dbg_sent_count = 0;
static unsigned char dbg_recv[DBG_LOG_LEN];
static int dbg_recv_count = 0;

static void dbg_log_sent(const char *buf, int n) {
    for (int i = 0; i < n && dbg_sent_count < 1<<30; i++) {
        dbg_sent[dbg_sent_count % DBG_LOG_LEN] = (unsigned char)buf[i];
        dbg_sent_count++;
    }
}

static void dbg_log_recv(const char *buf, int n) {
    for (int i = 0; i < n; i++) {
        dbg_recv[dbg_recv_count % DBG_LOG_LEN] = (unsigned char)buf[i];
        dbg_recv_count++;
    }
}

/* Format the ring as a hex string of the most recent DBG_LOG_LEN bytes,
 * oldest-first. */
static void dbg_fmt(char *out, int out_sz,
                    const unsigned char *ring, int total) {
    int n = total < DBG_LOG_LEN ? total : DBG_LOG_LEN;
    int start = (total - n + DBG_LOG_LEN) % DBG_LOG_LEN;
    if (total < DBG_LOG_LEN) start = 0;
    int p = 0;
    for (int i = 0; i < n && p < out_sz - 4; i++) {
        int idx = (start + i) % DBG_LOG_LEN;
        p += snprintf(out + p, out_sz - p, "%02x ", ring[idx]);
    }
    out[p] = 0;
}

static void feed_terminal(terminal_t *term, const char *raw, int raw_len) {
    char buf[4 + READ_BUFSZ];
    int total;
    if (utf8_frag_len > 0) {
        memcpy(buf, utf8_frag, utf8_frag_len);
        memcpy(buf + utf8_frag_len, raw, raw_len);
        total = utf8_frag_len + raw_len;
        utf8_frag_len = 0;
    } else {
        memcpy(buf, raw, raw_len);
        total = raw_len;
    }
    /* Detect a trailing incomplete UTF-8 sequence and stash it. */
    int valid_end = total;
    for (int j = total - 1; j >= total - 3 && j >= 0; j--) {
        unsigned char b = (unsigned char)buf[j];
        if (b >= 0xc0) {
            int seq_len = (b < 0xe0) ? 2 : (b < 0xf0) ? 3 : 4;
            if (total - j < seq_len) {
                utf8_frag_len = total - j;
                memcpy(utf8_frag, buf + j, utf8_frag_len);
                valid_end = j;
            }
            break;
        } else if (b < 0x80) {
            break;  /* ASCII – no trailing fragment */
        }
    }
    terminal_write_n(term, buf, valid_end);
}

/* Render the bottom-screen status panel. Must be called inside
 * C2D_SceneBegin(bot). */
static void draw_status(renderer_t *r, const ssh_config_t *cfg,
                        const keyboard_t *kbd, terminal_t *term,
                        const char *status_text, uint32_t status_color) {
    char line[64];

    /* Title bar */
    renderer_draw_rect_cells(r, 0, 0, R_BOT_COLS, 1, 0x313244ff);
    renderer_draw_text(r, 1, 0, "3dssh M3", COLOR_ACCENT);
    snprintf(line, sizeof(line), "%s@%s", cfg->user, cfg->host);
    renderer_draw_text(r, 12, 0, line, COLOR_FG);

    /* Status line */
    renderer_draw_text(r, 1, 2, status_text, status_color);

    /* Modifier state — only line that animates so the user can confirm
     * SELECT did something. */
    snprintf(line, sizeof(line), "Ctrl: %s", keyboard_mod_label(kbd));
    renderer_draw_text(r, 1, 4, line,
                       (kbd && kbd->sticky_ctrl == MOD_ARMED)
                           ? COLOR_ACCENT : COLOR_DIM);

    /* Key legend — minimal, "real terminal" experience: scrolling and
     * history are tmux's job, not ours. */
    renderer_draw_text(r, 1,  7, "KEY LEGEND", COLOR_ACCENT);
    renderer_draw_text(r, 1,  8, "  A    Enter         D-pad arrows", COLOR_FG);
    renderer_draw_text(r, 1,  9, "  B    Backspace     L+key Ctrl-key", COLOR_FG);
    renderer_draw_text(r, 1, 10, "  X    type a line   SELECT once-Ctrl", COLOR_FG);
    renderer_draw_text(r, 1, 11, "  STRT quit          Stick: glance back", COLOR_FG);

    renderer_draw_text(r, 1, 13, "TIPS", COLOR_ACCENT);
    renderer_draw_text(r, 1, 14, "  Ctrl-C: SELECT then X+'c'", COLOR_FG);
    renderer_draw_text(r, 1, 15, "  tmux history: prefix [ (Ctrl-B then [)", COLOR_FG);

    /* Diagnostic readout — proves what bytes hit the wire when the user
     * presses a key.  Press B and look for "7f" in 'sent'.  Watch how
     * cursor moves in the term struct vs what you see on the top screen. */
    renderer_draw_text(r, 1, 17, "DEBUG", COLOR_ACCENT);
    if (term) {
        snprintf(line, sizeof(line),
                 "cur=(%d,%d) mouse=%d/%d alt=%d",
                 term->cur_x, term->cur_y,
                 term->mouse_proto, term->mouse_sgr, term->use_alt);
        renderer_draw_text(r, 1, 18, line, COLOR_DIM);
    }
    char hex[64];
    dbg_fmt(hex, sizeof(hex), dbg_sent, dbg_sent_count);
    snprintf(line, sizeof(line), "sent: %s", hex);
    renderer_draw_text(r, 1, 19, line, COLOR_DIM);
    dbg_fmt(hex, sizeof(hex), dbg_recv, dbg_recv_count);
    snprintf(line, sizeof(line), "recv: %s", hex);
    renderer_draw_text(r, 1, 20, line, COLOR_DIM);
}

int main(int argc, char *argv[]) {
    char err[256] = {0};
    char status_buf[80] = "starting...";
    uint32_t status_color = COLOR_WARN;
    ssh_client_t *ssh = NULL;       /* declared up here so goto idle_loop is safe */

    /* Graphics + citro2d setup. */
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(32768);
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    /* Load config from SD. */
    ssh_config_t cfg;
    int loaded = config_load(&cfg, CONFIG_PATH);
    snprintf(status_buf, sizeof(status_buf),
             loaded ? "config: SD" : "config: defaults (no SD config)");

    /* Sub-systems. */
    terminal_t *term = terminal_init(R_TOP_COLS, R_TOP_ROWS);
    renderer_t *r    = renderer_init(top, bot);
    keyboard_t *kbd  = keyboard_init();
    if (!term || !r || !kbd) goto cleanup;

    if (net_init(err, sizeof(err)) != 0) {
        snprintf(status_buf, sizeof(status_buf), "net err: %s", err);
        status_color = COLOR_ERR;
        goto idle_loop;
    }

    /* Banner inside the terminal. */
    {
        char banner[160];
        snprintf(banner, sizeof(banner),
                 "\x1b[36m3dssh M3\x1b[0m connecting to "
                 "\x1b[33m%s@%s:%d\x1b[0m...\r\n",
                 cfg.user, cfg.host, cfg.port);
        terminal_write(term, banner);
    }
    snprintf(status_buf, sizeof(status_buf), "connecting...");

    /* Pump one frame so the user sees the banner before the (synchronous,
     * 5-10s) RSA handshake blocks the main loop. */
    {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, C2D_Color32(0x1e, 0x1e, 0x2e, 0xff));
        C2D_SceneBegin(top);
        renderer_draw_terminal(r, term);
        C2D_TargetClear(bot, C2D_Color32(0x18, 0x18, 0x25, 0xff));
        C2D_SceneBegin(bot);
        draw_status(r, &cfg, kbd, term, status_buf, status_color);
        C3D_FrameEnd(0);
    }

    ssh = ssh_connect_pubkey(
        cfg.host, cfg.port, cfg.user,
        cfg.key_path, NULL,
        cfg.passphrase[0] ? cfg.passphrase : NULL,
        err, sizeof(err));

    if (!ssh) {
        char line[256];
        snprintf(line, sizeof(line), "\x1b[31mSSH error:\x1b[0m %s\r\n", err);
        terminal_write(term, line);
        snprintf(status_buf, sizeof(status_buf), "ssh err");
        status_color = COLOR_ERR;
    } else {
        terminal_write(term, "\x1b[32mconnected.\x1b[0m\r\n");
        ssh_set_pty_size(ssh, R_TOP_COLS, R_TOP_ROWS);
        snprintf(status_buf, sizeof(status_buf), "connected %s:%d",
                 cfg.host, cfg.port);
        status_color = COLOR_OK;
    }

idle_loop:
    {
        char rbuf[READ_BUFSZ];
        char ibuf[256];

        while (aptMainLoop()) {
            hidScanInput();
            u32 down = hidKeysDown();
            u32 held = hidKeysHeld();
            circlePosition cpad;
            hidCircleRead(&cpad);

            if (down & KEY_START) break;

            /* SSH receive */
            if (ssh && ssh_is_connected(ssh)) {
                int n = ssh_read(ssh, rbuf, sizeof(rbuf));
                if (n > 0) {
                    dbg_log_recv(rbuf, n);
                    feed_terminal(term, rbuf, n);
                } else if (n < 0) {
                    terminal_write(term, "\r\n\x1b[31m[disconnected]\x1b[0m\r\n");
                    ssh_disconnect(ssh);
                    ssh = NULL;
                    snprintf(status_buf, sizeof(status_buf), "disconnected");
                    status_color = COLOR_WARN;
                }
            }

            /* Physical key input. Any produced byte snaps the local view
             * back to live output (so the user always sees their command
             * echo even if they were peeking at scrollback). */
            const char *out = keyboard_handle_input(kbd, term, down, held, cpad.dy);
            if (out && ssh && ssh_is_connected(ssh)) {
                int olen = (int)strlen(out);
                if (term->sb_offset != 0) terminal_scroll_view(term, -term->sb_offset);
                dbg_log_sent(out, olen);
                ssh_write(ssh, out, olen);
            }

            /* X = pop swkbd for free-form text entry (replaced in M4). The
             * sticky Ctrl modifier set via SELECT is applied here too: ARMED
             * Ctrl-transforms the first typed char (so SELECT -> X -> 'c'
             * sends Ctrl-C). */
            if ((down & KEY_X) && ssh && ssh_is_connected(ssh)) {
                if (prompt_swkbd("type a line", ibuf, sizeof(ibuf))) {
                    int n = (int)strlen(ibuf);
                    if (n > 0) {
                        n = keyboard_apply_modifiers(kbd, ibuf, n);
                        terminal_scroll_view(term, -term->sb_offset);
                        dbg_log_sent(ibuf, n);
                        ssh_write(ssh, ibuf, n);
                    }
                }
            }

            /* Render */
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TargetClear(top, C2D_Color32(0x1e, 0x1e, 0x2e, 0xff));
            C2D_SceneBegin(top);
            renderer_draw_terminal(r, term);
            C2D_TargetClear(bot, C2D_Color32(0x18, 0x18, 0x25, 0xff));
            C2D_SceneBegin(bot);
            draw_status(r, &cfg, kbd, term, status_buf, status_color);
            C3D_FrameEnd(0);
        }

        if (ssh) ssh_disconnect(ssh);
    }
    net_fini();

cleanup:
    if (kbd)  keyboard_free(kbd);
    if (r)    renderer_free(r);
    if (term) terminal_free(term);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
