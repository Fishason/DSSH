/*
 * 3dssh — Nintendo 3DS SSH client (M4).
 *
 * M4 changes from M3:
 *   - All modifiers (Shift / Ctrl / Alt) on physical shoulder/face buttons
 *     (L=Shift, Y=Ctrl, X=Alt — see source/keyboard.c).  No more
 *     SELECT-driven sticky-Ctrl state machine.
 *   - SELECT now emits Esc, R now toggles IME mode (EN ↔ CN).
 *   - Bottom screen is the full custom soft keyboard (source/softkb.c)
 *     — no more system swkbd applet popup.  X key is for Alt now.
 *   - Touch screen drives the soft keyboard.  Tapped letter goes through
 *     keyboard_emit_for() so any held modifier (L/Y/X) is applied.
 *
 * Single-threaded polling main loop.  60 fps render cadence.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <3ds.h>
#include <citro2d.h>

#include "ssh_client.h"
#include "config.h"
#include "terminal.h"
#include "renderer.h"
#include "keyboard.h"
#include "softkb.h"
#include "mascot.h"
#include "ime_pinyin.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

#define CONFIG_PATH     "sdmc:/3ds/3dssh/config.ini"
#define READ_BUFSZ      2048

#define COLOR_OK        0xa6e3a1ff
#define COLOR_WARN      0xfab387ff
#define COLOR_ERR       0xf38ba8ff
#define COLOR_FG        0xcdd6f4ff
#define COLOR_DIM       0x6c7086ff
#define COLOR_ACCENT    0x89b4faff

static u32 *soc_buf = NULL;

static int net_init(char *err, int err_sz) {
    soc_buf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!soc_buf) { snprintf(err, err_sz, "memalign failed"); return -1; }
    int rc = socInit(soc_buf, SOC_BUFFERSIZE);
    if (rc != 0) {
        snprintf(err, err_sz, "socInit 0x%08lX", (unsigned long)rc);
        return -1;
    }
    return 0;
}

static void net_fini(void) {
    socExit();
    if (soc_buf) { free(soc_buf); soc_buf = NULL; }
}

/* UTF-8 fragment buffer: SSH packet boundaries can split a multibyte char.
 * Stash up to 3 trailing bytes for the next read. */
static char utf8_frag[4];
static int  utf8_frag_len = 0;

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
            break;
        }
    }
    terminal_write_n(term, buf, valid_end);
}

/* Wall-clock of the last successful ssh_write.  Compared against
 * last_rx_at by the main loop's interactivity-stall detector — if we
 * sent input recently but haven't received anything back, the network
 * is unresponsive even when libssh2 hasn't yet declared the socket
 * dead.  Updated only by send_to_ssh below. */
static time_t g_last_tx_at = 0;

/* Snap the local terminal view to the bottom (canceling any user-side
 * scrollback peek) right before sending a key.  This way the user always
 * sees what they just typed, even if they were glancing at history. */
static void send_to_ssh(ssh_client_t *ssh, terminal_t *term,
                        const char *bytes, int n) {
    if (!ssh || !ssh_is_connected(ssh) || n <= 0) return;
    if (term && term->sb_offset != 0) terminal_scroll_view(term, -term->sb_offset);
    ssh_write(ssh, bytes, n);
    g_last_tx_at = time(NULL);
}

int main(int argc, char *argv[]) {
    char err[256] = {0};
    char status_buf[80] = "starting...";
    uint32_t status_color = COLOR_WARN;
    ssh_client_t *ssh = NULL;

    /* ── Graphics init (audio disabled — see audio.{c,h} kept for future) ── */
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(32768);
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    /* ── romfs (carries pinyin_dict.bin for M7 IME) ── */
    Result romfs_rc = romfsInit();
    int romfs_ok   = R_SUCCEEDED(romfs_rc);

    /* ── Config ── */
    ssh_config_t cfg;
    int loaded = config_load(&cfg, CONFIG_PATH);
    snprintf(status_buf, sizeof(status_buf),
             loaded ? "config: SD" : "config: defaults");

    /* Seed RNG so the mascot's idle/walk transitions don't repeat across
     * runs.  time(NULL) is fine — we don't need cryptographic randomness. */
    srand((unsigned)time(NULL));

    /* ── Sub-systems ── */
    terminal_t *term = terminal_init(R_TOP_COLS, R_TOP_ROWS);
    renderer_t *r    = renderer_init(top, bot);
    keyboard_t *kbd  = keyboard_init();
    /* Mascot lives in the bottom row (y=214..239, 26 px tall).  Clock
     * occupies x=2..67 on the left; mascot scampers in x=72..302 on
     * the right.  Crab is 12 px tall so y_top = 214 + (26-12)/2 = 221
     * vertically centers it in the row. */
    mascot_t   *mc   = mascot_init(72, 302, 221);
    /* IME loads later (after the M7 banner pumps); softkb tolerates
     * a NULL ime by falling back to passthrough in CN mode. */
    ime_t      *ime  = NULL;
    softkb_t   *kb   = softkb_init(NULL);
    if (!term || !r || !kbd || !kb || !mc) goto cleanup;

    if (net_init(err, sizeof(err)) != 0) {
        snprintf(status_buf, sizeof(status_buf), "net err: %s", err);
        status_color = COLOR_ERR;
        goto idle_loop;
    }

    /* Banner inside the terminal. */
    terminal_write(term, "\x1b[36m3dssh M7\x1b[0m\r\n");
    if (romfs_ok) {
        terminal_write(term, "loading pinyin dictionary...\r\n");
    } else {
        terminal_write(term, "\x1b[33mromfs init failed — IME unavailable\x1b[0m\r\n");
    }

    /* Pump one frame so the user sees the loading banner during the
     * (synchronous, ~5s) dict read.  The bottom screen still has the
     * keyboard rendered — the badge and mascot work normally. */
    {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, C2D_Color32(0x1e, 0x1e, 0x2e, 0xff));
        C2D_SceneBegin(top);
        renderer_draw_terminal(r, term);
        C2D_TargetClear(bot, C2D_Color32(0x18, 0x18, 0x25, 0xff));
        C2D_SceneBegin(bot);
        softkb_draw(kb, r, kbd);
        C3D_FrameEnd(0);
    }

    /* Load the pinyin dict (~9 MB).  Failure here is non-fatal — we
     * just leave ime NULL and softkb degrades CN mode to passthrough. */
    if (romfs_ok) {
        ime = ime_init("romfs:/pinyin_dict.bin");
        if (ime) {
            softkb_set_ime(kb, ime);
            keyboard_set_ime(kbd, ime);
            terminal_write(term, "\x1b[32mdictionary loaded.\x1b[0m\r\n");
        } else {
            terminal_write(term, "\x1b[31mdictionary load failed — IME disabled\x1b[0m\r\n");
        }
    }

    {
        char banner[160];
        snprintf(banner, sizeof(banner),
                 "connecting to \x1b[33m%s@%s:%d\x1b[0m...\r\n",
                 cfg.user, cfg.host, cfg.port);
        terminal_write(term, banner);
    }

    /* Pump again so the user sees the loaded/connecting banners before
     * the SSH handshake blocks the main loop. */
    {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, C2D_Color32(0x1e, 0x1e, 0x2e, 0xff));
        C2D_SceneBegin(top);
        renderer_draw_terminal(r, term);
        C2D_TargetClear(bot, C2D_Color32(0x18, 0x18, 0x25, 0xff));
        C2D_SceneBegin(bot);
        softkb_draw(kb, r, kbd);
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
        (void)status_buf; (void)status_color;  /* not currently rendered */

        /* Interactive-stall detection (5 s): after the user sends bytes
         * (g_last_tx_at), if no response (echo, output, keepalive ack)
         * comes back within 5 s, the network is broken in the way the
         * user actually notices.  The alert clears the instant any
         * byte arrives.
         *
         * No idle-stall rule.  When the user isn't typing they aren't
         * waiting for anything, so a silently broken link during an
         * idle period is not worth flagging — it would just produce
         * false positives when the keepalive ack happens to drop. */
        const int STALL_TX_NORX_S = 5;
        time_t last_rx_at = time(NULL);
        g_last_tx_at      = last_rx_at;
        int    stall_alert = 0;

        while (aptMainLoop()) {
            hidScanInput();
            u32 down = hidKeysDown();
            u32 held = hidKeysHeld();
            circlePosition cpad;
            hidCircleRead(&cpad);

            if (down & KEY_START) break;

            /* ── SSH receive ── */
            if (ssh && ssh_is_connected(ssh)) {
                ssh_keepalive_tick(ssh);
                int n = ssh_read(ssh, rbuf, sizeof(rbuf));
                if (n > 0) {
                    /* Capture for the debug page's recv-ring before the
                     * UTF-8 reassembler can chop the buffer up. */
                    softkb_record_recv(kb, rbuf, n);
                    feed_terminal(term, rbuf, n);
                    last_rx_at = time(NULL);
                } else if (n < 0) {
                    terminal_write(term, "\r\n\x1b[31m[disconnected]\x1b[0m\r\n");
                    ssh_disconnect(ssh);
                    ssh = NULL;
                }
            }

            /* Interactive-stall: only flag when the user has typed
             * something more recently than the last reply, and that
             * typed-but-unacked window has been open longer than
             * STALL_TX_NORX_S.  Idle periods can never trigger this. */
            time_t now_t = time(NULL);
            int want_alert = ssh && ssh_is_connected(ssh) &&
                             g_last_tx_at > last_rx_at &&
                             (now_t - g_last_tx_at) > STALL_TX_NORX_S;
            if (want_alert != stall_alert) {
                stall_alert = want_alert;
                mascot_set_alert(mc, stall_alert);
            }

            /* ── Physical keys (Esc / Enter / BS / D-pad / R / scroll) ── */
            const char *out = keyboard_handle_input(kbd, term, down, held, cpad.dy);
            if (out) send_to_ssh(ssh, term, out, (int)strlen(out));

            /* ── Touch / soft keyboard ── */
            int touch_pressed = (held & KEY_TOUCH) ? 1 : 0;
            int tx = -1, ty = -1;
            if (touch_pressed) {
                touchPosition tp;
                hidTouchRead(&tp);
                tx = tp.px; ty = tp.py;
            }
            int touch_down = (down & KEY_TOUCH) ? 1 : 0;

            /* Bottom row (y >= 214) belongs to mascot — taps there don't
             * reach softkb.  On the down-edge we hit-test the crab; if
             * it's where the finger lands the crab flees.
             *
             * The mascot is suppressed entirely when the debug overlay
             * is up or the user has toggled it off, so taps in that
             * region fall through to softkb_touch (which no-ops since
             * no key extends below y=213 in normal mode, and the debug
             * page handles its own widgets). */
            int show_mascot = !softkb_in_debug(kb) && softkb_mascot_enabled(kb);
            if (touch_down && ty >= 214 && show_mascot) {
                if (mascot_hit_test(mc, tx, ty))
                    mascot_on_touched(mc, tx);
            } else {
                const char *kt = softkb_touch(kb, kbd, tx, ty, touch_down);
                if (kt) send_to_ssh(ssh, term, kt, (int)strlen(kt));
            }
            /* Always advance the softkb release-fade timer on no-touch frames. */
            if (!touch_pressed) {
                softkb_touch(kb, kbd, -1, -1, 0);
            }

            /* Mascot ticks only when it's actually being shown.  When
             * paused this way it freezes in place; on re-enable it
             * resumes from wherever it stopped. */
            if (show_mascot) mascot_update(mc);

            /* ── Render ── */
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TargetClear(top, C2D_Color32(0x1e, 0x1e, 0x2e, 0xff));
            C2D_SceneBegin(top);
            renderer_draw_terminal(r, term);

            C2D_TargetClear(bot, C2D_Color32(0x18, 0x18, 0x25, 0xff));
            C2D_SceneBegin(bot);
            softkb_draw(kb, r, kbd);

            /* Bottom row: clock on the left, mascot on the right.
             * Suppressed entirely when the debug overlay is up — the
             * debug page draws its own full-screen background. */
            if (!softkb_in_debug(kb)) {
                char clock_buf[24];
                time_t now = time(NULL);
                struct tm lt;
                localtime_r(&now, &lt);
                snprintf(clock_buf, sizeof(clock_buf),
                         "%02d-%02d %02d:%02d",
                         lt.tm_mon + 1, lt.tm_mday,
                         lt.tm_hour, lt.tm_min);
                /* Vertically center 12-px text in the 26-px bottom row:
                 * y = 214 + (26-12)/2 = 221. */
                renderer_draw_text_px(2, 221, clock_buf, COLOR_DIM);
                if (softkb_mascot_enabled(kb)) mascot_draw(mc);
            }

            C3D_FrameEnd(0);
        }

        if (ssh) ssh_disconnect(ssh);
    }
    net_fini();

cleanup:
    if (ime)  ime_free(ime);
    if (mc)   mascot_free(mc);
    if (kb)   softkb_free(kb);
    if (kbd)  keyboard_free(kbd);
    if (r)    renderer_free(r);
    if (term) terminal_free(term);
    if (romfs_ok) romfsExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
