/*
 * 3dssh — Nintendo 3DS SSH client (M4).
 *
 * M4 changes from M3:
 *   - All modifiers (Shift / Ctrl / Alt) on physical shoulder/face buttons
 *     (L=Shift, Y=Ctrl, X=Alt — see source/keyboard.c).  No more
 *     SELECT-driven sticky-Ctrl state machine.
 *   - SELECT now emits Esc, R now toggles IME mode (EN ↔ CN).
 *     SELECT is overloaded: when the SSH session is dead (hard disconnect
 *     after lid-close sleep), pressing SELECT reconnects instead of emit-
 *     ting Esc — so the user can recover without exiting DSSH (M13).
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
#include <ts3ds.h>

#include "ssh_client.h"
#include "config.h"
#include "keychain_protocol.h"
#include "terminal.h"
#include "renderer.h"
#include "keyboard.h"
#include "softkb.h"
#include "mascot.h"

#ifndef DSSH_TAILSCALE_PATH
#define DSSH_TAILSCALE_PATH "auto"
#endif
#ifndef DSSH_TAILSCALE_VERBOSE
#define DSSH_TAILSCALE_VERBOSE 0
#endif
#include "ime_pinyin.h"
#include "voice.h"
#include "ai_modal.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

#define CONFIG_PATH     "sdmc:/3ds/3dssh/config.ini"
#define READ_BUFSZ      2048

/* Interactive shell startup can be noticeably slower over Tailscale/DERP,
 * especially when fish/zsh startup hooks perform network or filesystem I/O. */
#define SHELL_READY_TIMEOUT_MS      60000
#define KEYCHAIN_PROMPT_TIMEOUT_MS  30000
#define KEYCHAIN_RESULT_TIMEOUT_MS  30000

#define COLOR_OK        0xa6e3a1ff
#define COLOR_WARN      0xfab387ff
#define COLOR_ERR       0xf38ba8ff
#define COLOR_FG        0xcdd6f4ff
#define COLOR_DIM       0x6c7086ff
#define COLOR_ACCENT    0x89b4faff

static u32 *soc_buf = NULL;

#define TS_DEBUG_LINE_COUNT 24
#define TS_DEBUG_LINE_SIZE  224

typedef struct tailscale_debug_log {
    LightLock lock;
    char lines[TS_DEBUG_LINE_COUNT][TS_DEBUG_LINE_SIZE];
    unsigned char levels[TS_DEBUG_LINE_COUNT];
    unsigned head;
    unsigned count;
    unsigned dropped;
    int startup_verbose;
} tailscale_debug_log;

static void tailscale_debug_init(tailscale_debug_log *debug) {
    memset(debug, 0, sizeof(*debug));
    LightLock_Init(&debug->lock);
    debug->startup_verbose = DSSH_TAILSCALE_VERBOSE ? 1 : 0;
}

/* libts3ds may log from its DERP worker. Keep that callback independent of
 * terminal rendering, then drain it from DSSH's main thread. */
static void tailscale_debug_capture(void *userdata, int level,
                                    const char *message) {
    tailscale_debug_log *debug = (tailscale_debug_log *)userdata;
    if (!debug || !message) return;
    LightLock_Lock(&debug->lock);
    /* Keep rich diagnostics while Tailscale and SSH are connecting. Once an
     * interactive shell exists, control/DERP maintenance is independent of
     * the established WireGuard data path and must never corrupt the user's
     * command line. SSH failure still has its own visible connection state. */
    if (level >= 3 || !debug->startup_verbose) {
        LightLock_Unlock(&debug->lock);
        return;
    }
    if (debug->count == TS_DEBUG_LINE_COUNT) {
        debug->head = (debug->head + 1) % TS_DEBUG_LINE_COUNT;
        debug->count--;
        debug->dropped++;
    }
    unsigned slot = (debug->head + debug->count) % TS_DEBUG_LINE_COUNT;
    snprintf(debug->lines[slot], TS_DEBUG_LINE_SIZE, "%s", message);
    debug->levels[slot] = (unsigned char)level;
    debug->count++;
    LightLock_Unlock(&debug->lock);
}

static void tailscale_debug_set_runtime(tailscale_debug_log *debug) {
    if (!debug) return;
    LightLock_Lock(&debug->lock);
    debug->startup_verbose = 0;
    LightLock_Unlock(&debug->lock);
}

static void tailscale_debug_flush(tailscale_debug_log *debug,
                                  terminal_t *term) {
    if (!debug || !term) return;
    for (;;) {
        char message[TS_DEBUG_LINE_SIZE];
        unsigned level;
        unsigned dropped = 0;
        LightLock_Lock(&debug->lock);
        if (debug->count == 0) {
            dropped = debug->dropped;
            debug->dropped = 0;
            LightLock_Unlock(&debug->lock);
            if (dropped) {
                char line[96];
                snprintf(line, sizeof(line),
                         "\x1b[33m[ts3ds] %u earlier log lines dropped"
                         "\x1b[0m\r\n", dropped);
                terminal_write(term, line);
            }
            return;
        }
        unsigned slot = debug->head;
        snprintf(message, sizeof(message), "%s", debug->lines[slot]);
        level = debug->levels[slot];
        debug->head = (debug->head + 1) % TS_DEBUG_LINE_COUNT;
        debug->count--;
        LightLock_Unlock(&debug->lock);

        char line[TS_DEBUG_LINE_SIZE + 40];
        const char *color = level == 0 ? "\x1b[31m"
                            : level == 1 ? "\x1b[33m" : "\x1b[90m";
        snprintf(line, sizeof(line), "%s[ts3ds] %s\x1b[0m\r\n",
                 color, message);
        terminal_write(term, line);
    }
}

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

typedef struct {
    int unlock_status;
    int verify_status;
} keychain_report_t;

static int startup_write_all(ssh_client_t *ssh, const char *data, int len,
                             int timeout_ms);

static void flush_terminal_responses(ssh_client_t *ssh, terminal_t *term) {
    char reply[TERM_RESPONSE_MAX];
    int n;
    while ((n = terminal_take_response(term, reply, sizeof(reply))) > 0)
        if (startup_write_all(ssh, reply, n, 1000) != 0) break;
}

static int startup_write_all(ssh_client_t *ssh, const char *data, int len,
                             int timeout_ms) {
    int sent = 0;
    u64 deadline = osGetTime() + (u64)timeout_ms;
    while (sent < len && osGetTime() < deadline) {
        ssh_poll_transport(ssh);
        int n = ssh_write(ssh, data + sent, len - sent);
        if (n < 0) return -1;
        if (n == 0) {
            svcSleepThread(10 * 1000 * 1000LL);
        } else {
            sent += n;
        }
    }
    return sent == len ? 0 : -1;
}

static void append_startup_tail(char *tail, int cap, int *tail_len,
                                const char *data, int len) {
    if (len >= cap - 1) {
        data += len - (cap - 1);
        len = cap - 1;
        *tail_len = 0;
    } else if (*tail_len + len >= cap) {
        int drop = *tail_len + len - (cap - 1);
        memmove(tail, tail + drop, (size_t)(*tail_len - drop));
        *tail_len -= drop;
    }
    memcpy(tail + *tail_len, data, (size_t)len);
    *tail_len += len;
    tail[*tail_len] = 0;
}

/* Pump the main interactive PTY until either marker/prompt appears. Feeding
 * the terminal while waiting is essential for fish: its startup asks CSI 6n
 * and will not finish drawing the prompt until DSSH sends the queued CPR
 * reply. The alternate marker is optional; a return value of 1 means needle,
 * 2 means alternate, 0 means timeout, and -1 means SSH disconnected. */
static int wait_for_remote_text_any(ssh_client_t *ssh, terminal_t *term,
                                    const char *needle,
                                    const char *alternate,
                                    int timeout_ms,
                                    char *capture, int capture_sz) {
    char raw[READ_BUFSZ];
    char tail[768] = {0};
    int tail_len = 0;
    u64 deadline = osGetTime() + (u64)timeout_ms;

    while (osGetTime() < deadline) {
        /* The keychain bootstrap runs before the main loop, so the UI is
         * frozen while we wait (up to 60s if the remote auto-starts tmux,
         * which swallows the OSC readiness marker).  Give the user a way
         * out: START aborts the wait and hands the shell over as-is. */
        hidScanInput();
        if (hidKeysDown() & KEY_START) return -2;
        /* A Tailscale-backed SSH socket lives in libts3ds's private lwIP
         * stack. It cannot progress while this synchronous bootstrap loop
         * sleeps unless its transport is explicitly pumped. */
        ssh_poll_transport(ssh);
        int n = ssh_read(ssh, raw, sizeof(raw));
        if (n < 0) return -1;
        if (n == 0) {
            svcSleepThread(10 * 1000 * 1000LL);
            continue;
        }
        append_startup_tail(tail, sizeof(tail), &tail_len, raw, n);
        feed_terminal(term, raw, n);
        flush_terminal_responses(ssh, term);
        char *found = strstr(tail, needle);
        char *found_alternate = alternate ? strstr(tail, alternate) : NULL;
        if (found || found_alternate) {
            if (capture && capture_sz > 0)
                snprintf(capture, (size_t)capture_sz, "%s", tail);
            if (!found) return 2;
            if (!found_alternate) return 1;
            return found <= found_alternate ? 1 : 2;
        }
    }
    if (capture && capture_sz > 0)
        snprintf(capture, (size_t)capture_sz, "%s", tail);
    return 0;
}

static int wait_for_remote_text(ssh_client_t *ssh, terminal_t *term,
                                const char *needle, int timeout_ms,
                                char *capture, int capture_sz) {
    return wait_for_remote_text_any(ssh, term, needle, NULL, timeout_ms,
                                    capture, capture_sz);
}

static int parse_keychain_result(const char *capture,
                                 keychain_report_t *report,
                                 char *err, int err_sz) {
    /* Match the raw OSC form (ESC ] 777 ; ...) — the shell's echo of the
     * unlock command contains the same words as printable text and would
     * shadow the real result if we matched the bare marker name. */
    const char *result = strstr(capture, DSSH_KEYCHAIN_RESULT_MARKER);
    if (!result || sscanf(result + 6, /* skip ESC ] 7 7 7 ; */
            "DSSH_KEYCHAIN_RESULT unlock=%d verify=%d",
            &report->unlock_status, &report->verify_status) != 2) {
        snprintf(err, (size_t)err_sz, "invalid keychain result marker");
        return -1;
    }
    if (report->unlock_status != 0) {
        snprintf(err, (size_t)err_sz,
                 "security unlock-keychain failed (status=%d)",
                 report->unlock_status);
        return -1;
    }
    if (report->verify_status != 0) {
        snprintf(err, (size_t)err_sz,
                 "keychain verification failed (status=%d)",
                 report->verify_status);
        return -1;
    }
    err[0] = 0;
    return 0;
}

/* Unlock through the already-open interactive PTY, mirroring ServerCC's
 * proven macOS flow: wait for a prompt-ready shell, start security, wait for
 * its password prompt, then write the password.  An exec channel without a
 * PTY can fail with "User interaction is not allowed" on macOS. */
static int unlock_macos_keychain(ssh_client_t *ssh, terminal_t *term,
                                 const char *password,
                                 keychain_report_t *report,
                                 char *err, int err_sz) {
    static const char ready_command[] =
        "printf '\\033]777;%s_READY_%s\\007' DSSH SHELL\n";
    static const char ready_marker[] = "DSSH_READY_SHELL";
    static const char unlock_command[] =
        "sh -c '/usr/bin/security unlock-keychain "
        "\"$HOME/Library/Keychains/login.keychain-db\"; u=$?; v=-1; "
        "if [ \"$u\" -eq 0 ]; then /usr/bin/security show-keychain-info "
        "\"$HOME/Library/Keychains/login.keychain-db\" >/dev/null 2>&1; "
        "v=$?; fi; "
        "printf \"\\033]777;DSSH_KEYCHAIN_RESULT unlock=%d verify=%d\\007\" "
        "\"$u\" \"$v\"; "
        "printf \"\\033[2J\\033[H\"; "
        "if [ \"$u\" -ne 0 ] || [ \"$v\" -ne 0 ]; then "
        "printf \"[keychain] unlock failed (unlock=%d verify=%d)\\n\" "
        "\"$u\" \"$v\"; fi'\n";
    static const char result_marker[] = DSSH_KEYCHAIN_RESULT_MARKER;
    char capture[768];

    report->unlock_status = -1;
    report->verify_status = -1;
    if (!password || !*password) {
        snprintf(err, (size_t)err_sz, "keychain password is empty");
        return -1;
    }

    if (startup_write_all(ssh, ready_command,
                          sizeof(ready_command) - 1, 3000) != 0) {
        snprintf(err, (size_t)err_sz, "shell readiness probe write failed");
        return -1;
    }
    int rc = wait_for_remote_text(ssh, term, ready_marker,
                                  SHELL_READY_TIMEOUT_MS,
                                  NULL, 0);
    if (rc <= 0) {
        if (rc == -2) {
            snprintf(err, (size_t)err_sz,
                     "keychain bootstrap aborted (START)");
        } else if (rc < 0) {
            snprintf(err, (size_t)err_sz,
                     "SSH disconnected while waiting for shell");
        } else {
            snprintf(err, (size_t)err_sz,
                     "shell readiness probe timed out after %ds",
                     SHELL_READY_TIMEOUT_MS / 1000);
        }
        return -1;
    }

    if (startup_write_all(ssh, unlock_command,
                          sizeof(unlock_command) - 1, 3000) != 0) {
        snprintf(err, (size_t)err_sz, "unlock command write failed");
        return -1;
    }
    rc = wait_for_remote_text_any(ssh, term, "password to unlock",
                                  result_marker,
                                  KEYCHAIN_PROMPT_TIMEOUT_MS,
                                  capture, sizeof(capture));
    if (rc == 2) {
        /* security can fail before it ever asks for a password (for example,
         * when the keychain path or utility is unavailable). Consume and
         * report that result immediately instead of waiting for a prompt that
         * will never arrive. */
        return parse_keychain_result(capture, report, err, err_sz);
    }
    if (rc <= 0) {
        if (rc == -2) {
            snprintf(err, (size_t)err_sz,
                     "keychain bootstrap aborted (START)");
        } else if (rc < 0) {
            snprintf(err, (size_t)err_sz,
                     "SSH disconnected before keychain password prompt");
        } else {
            snprintf(err, (size_t)err_sz,
                     "keychain password prompt timed out after %ds",
                     KEYCHAIN_PROMPT_TIMEOUT_MS / 1000);
        }
        return -1;
    }

    /* The password is sent only after security owns the foreground PTY and
     * has disabled echo, so it is neither displayed nor stored in history. */
    if (startup_write_all(ssh, password, (int)strlen(password), 3000) != 0 ||
        startup_write_all(ssh, "\n", 1, 3000) != 0) {
        snprintf(err, (size_t)err_sz, "keychain password write failed");
        return -1;
    }

    rc = wait_for_remote_text(ssh, term, result_marker,
                              KEYCHAIN_RESULT_TIMEOUT_MS,
                              capture, sizeof(capture));
    if (rc <= 0) {
        if (rc == -2) {
            snprintf(err, (size_t)err_sz,
                     "keychain bootstrap aborted (START)");
        } else if (rc < 0) {
            snprintf(err, (size_t)err_sz,
                     "SSH disconnected during keychain verification");
        } else {
            snprintf(err, (size_t)err_sz,
                     "keychain result timed out after %ds",
                     KEYCHAIN_RESULT_TIMEOUT_MS / 1000);
        }
        return -1;
    }
    return parse_keychain_result(capture, report, err, err_sz);
}

/* Wall-clock of the last successful ssh_write.  Compared against
 * last_rx_at by the main loop's interactivity-stall detector — if we
 * sent input recently but haven't received anything back, the network
 * is unresponsive even when libssh2 hasn't yet declared the socket
 * dead.  Updated only by send_to_ssh below. */
static time_t g_last_tx_at = 0;

static void clear_secret(char *s, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)s;
    while (len-- > 0) *p++ = 0;
}

static void render_connecting_frame(C3D_RenderTarget *top,
                                    C3D_RenderTarget *bot,
                                    renderer_t *renderer, terminal_t *term,
                                    softkb_t *keyboard,
                                    keyboard_t *physical_keyboard) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top, C2D_Color32(0x1a, 0x1b, 0x26, 0xff));
    C2D_SceneBegin(top);
    renderer_draw_terminal(renderer, term);
    C2D_TargetClear(bot, C2D_Color32(0x18, 0x18, 0x25, 0xff));
    C2D_SceneBegin(bot);
    softkb_draw(keyboard, renderer, physical_keyboard);
    C3D_FrameEnd(0);
}

/* Run the macOS keychain bootstrap on a freshly connected session, if a
 * keychain password is configured (no-op otherwise).  Shared between the
 * initial connect and the SELECT-key reconnect, so a reconnected macOS
 * session gets its keychain unlocked again.  Returns the session pointer,
 * or NULL if a hard disconnect during bootstrap tore the session down. */
static ssh_client_t *keychain_bootstrap(ssh_client_t *ssh,
                                        const ssh_config_t *cfg,
                                        terminal_t *term, renderer_t *r,
                                        softkb_t *kb, keyboard_t *kbd,
                                        C3D_RenderTarget *top,
                                        C3D_RenderTarget *bot,
                                        char *status_buf, int status_sz,
                                        uint32_t *status_color,
                                        char *err, int err_sz) {
    if (!ssh || !cfg->macos_keychain_password[0]) return ssh;

    /* Keep this local-only progress line visible while bootstrap blocks,
     * then reset again so fish CPR uses remote coordinates. */
    terminal_write(term, "\x1b[36mUnlocking macOS keychain...\x1b[0m\r\n");
    render_connecting_frame(top, bot, r, term, kb, kbd);
    terminal_reset(term);

    keychain_report_t report = { -1, -1 };
    int unlock_rc = unlock_macos_keychain(
        ssh, term, cfg->macos_keychain_password, &report, err, err_sz);

    /* ssh_read()/ssh_write() clear the connected flag on a hard error.
     * Do not leave a non-NULL but unusable session in the idle loop. */
    if (!ssh_is_connected(ssh)) {
        if (unlock_rc == 0)
            snprintf(err, (size_t)err_sz,
                     "SSH disconnected during keychain bootstrap");
        char line[320];
        snprintf(line, sizeof(line), "\x1b[31mSSH error:\x1b[0m %s\r\n", err);
        terminal_write(term, line);
        snprintf(status_buf, (size_t)status_sz, "ssh err");
        *status_color = COLOR_ERR;
        ssh_disconnect(ssh);
        return NULL;
    }
    if (unlock_rc != 0) {
        /* Completed commands print FAILED + exit codes themselves.
         * Only transport/prompt timeouts need a local fallback. */
        if (report.unlock_status < 0 && report.verify_status < 0) {
            /* A prompt/result timeout can leave `security` owning the
             * foreground PTY. Abort it before handing control to the
             * user so keyboard input reaches the normal shell. */
            (void)startup_write_all(ssh, "\x03\n", 2, 2000);
            char line[320];
            snprintf(line, sizeof(line),
                     "\x1b[33mkeychain bootstrap failed:\x1b[0m %s\r\n", err);
            terminal_write(term, line);
        }
    }
    return ssh;
}

/* Snap the local terminal view to the bottom (canceling any user-side
 * scrollback peek) right before sending a key.  This way the user always
 * sees what they just typed, even if they were glancing at history. */
static void send_to_ssh(ssh_client_t *ssh, terminal_t *term,
                        const char *bytes, int n, mascot_t *mc) {
    if (!ssh || !ssh_is_connected(ssh) || n <= 0) return;
    if (term && term->sb_offset != 0) terminal_scroll_view(term, -term->sb_offset);
    ssh_write(ssh, bytes, n);
    g_last_tx_at = time(NULL);
    /* Kick the typing animation once per send so the crab pecks its
     * claws on every keystroke / handwriting commit / voice glyph. */
    if (mc) mascot_type_kick(mc);
}

/* Establish (or re-establish) the SSH session using the loaded config.
 * Used both for the initial connect at startup and for the SELECT-key
 * reconnect after a hard disconnect (lid-close sleep kills the TCP).
 * On success: returns a new ssh_client_t, resets the local terminal
 * (fish CPR needs remote coordinates), sizes the PTY and sets a status
 * string.  On failure: returns NULL, writes the red SSH-error banner +
 * the diagnostic in err. */
static ssh_client_t *reconnect_ssh(const ssh_config_t *cfg,
                                   ts3ds *tailscale,
                                   terminal_t *term,
                                   char *status_buf, int status_sz,
                                   char *err, int err_sz) {
    ssh_client_t *ssh = ssh_connect_pubkey(
        cfg->host, cfg->port, cfg->user,
        cfg->key_path, NULL,
        cfg->passphrase[0] ? cfg->passphrase : NULL,
        R_TOP_COLS, R_TOP_ROWS,
        tailscale,
        err, err_sz);

    if (!ssh) {
        char line[256];
        snprintf(line, sizeof(line), "\x1b[31mSSH error:\x1b[0m %s\r\n", err);
        terminal_write(term, line);
        snprintf(status_buf, status_sz, "ssh err");
        return NULL;
    }

    /* Local banners ("Reconnecting...", startup text) are not part of the
     * remote PTY screen. Reset before parsing shell output so fish's
     * cursor-position queries see the same coordinate system as sshd. */
    terminal_reset(term);
    ssh_set_pty_size(ssh, R_TOP_COLS, R_TOP_ROWS);
    snprintf(status_buf, status_sz, "connected %.56s:%d",
             cfg->host, cfg->port);
    return ssh;
}

int main(int argc, char *argv[]) {
    char err[256] = {0};
    char status_buf[80] = "starting...";
    uint32_t status_color = COLOR_WARN;
    ssh_client_t *ssh = NULL;
    ts3ds *tailscale = NULL;
    static tailscale_debug_log tailscale_debug;
    tailscale_debug_init(&tailscale_debug);

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
     * the right.  Crab is 11 px tall (10-row body + 1-row feet) so
     * y_top = 214 + (26-11)/2 = 221 centers it in the row. */
    mascot_t   *mc   = mascot_init(72, 302, 221);
    /* IME loads later (after the M7 banner pumps); softkb tolerates
     * a NULL ime by falling back to passthrough in CN mode. */
    ime_t      *ime  = NULL;
    softkb_t   *kb   = softkb_init(NULL);
    /* Voice input: physical START toggles record/transcribe.  Server
     * needs `dssh-whisper-shim` on PATH (installed by
     * tools/install_whisper_server.sh).  voice_t is allocated up-front;
     * mic only opens during the RECORDING state. */
    voice_t    *voice = voice_init();
    if (kb && voice) softkb_set_voice(kb, voice);
    /* M12: AI-ask modal — pops over the soft keyboard when the user
     * presses L+START and a Q&A returns from DeepSeek. */
    ai_modal_t *aim   = ai_modal_init();
    if (!term || !r || !kbd || !kb || !mc) goto cleanup;

    if (net_init(err, sizeof(err)) != 0) {
        snprintf(status_buf, sizeof(status_buf), "net err: %s", err);
        status_color = COLOR_ERR;
        goto idle_loop;
    }

    /* Banner inside the terminal. */
    terminal_write(term,
                   "\x1b[36mDSSH\x1b[0m  "
                   "\x1b[2m●\x1b[0m  pinyin IME (R toggle)  "
                   "\x1b[2m●\x1b[0m  voice (START to record)\r\n");
    if (romfs_ok) {
        terminal_write(term, "loading pinyin dictionary...\r\n");
    } else {
        terminal_write(term, "\x1b[33mromfs init failed — IME unavailable\x1b[0m\r\n");
    }

    if (config_tailscale_should_start(&cfg)) {
        ts3ds_config tailscale_config;
        ts3ds_config_init(&tailscale_config);
        tailscale_config.auth_key = cfg.tailscale_auth_key[0]
                                        ? cfg.tailscale_auth_key : NULL;
        tailscale_config.hostname = cfg.tailscale_hostname;
        tailscale_config.state_path = cfg.tailscale_state;
        tailscale_config.control_url = cfg.tailscale_control_url;
        if (ts3ds_path_policy_parse(DSSH_TAILSCALE_PATH,
                                    &tailscale_config.path_policy) !=
            TS3DS_OK) {
            char line[192];
            snprintf(line, sizeof(line),
                     "\x1b[31mTailscale build error:\x1b[0m invalid "
                     "path policy='%.64s' (use auto, direct, "
                     "peer-relay, or derp)\r\n",
                     DSSH_TAILSCALE_PATH);
            terminal_write(term, line);
            snprintf(status_buf, sizeof(status_buf), "tailscale build err");
            status_color = COLOR_ERR;
            goto idle_loop;
        }
        if (DSSH_TAILSCALE_VERBOSE) {
            tailscale_config.log = tailscale_debug_capture;
            tailscale_config.log_userdata = &tailscale_debug;
        }
        terminal_write(term,
                       "\x1b[36mConnecting to Tailscale...\x1b[0m\r\n");
        if (DSSH_TAILSCALE_VERBOSE) {
            char line[256];
            snprintf(line, sizeof(line),
                     "  node=%.63s auth=%s path=%.16s state=%.96s"
                     "\r\n",
                     cfg.tailscale_hostname,
                     cfg.tailscale_auth_key[0] ? "yes" : "no",
                     ts3ds_path_policy_name(tailscale_config.path_policy),
                     cfg.tailscale_state);
            terminal_write(term, line);
        }
        render_connecting_frame(top, bot, r, term, kb, kbd);
        tailscale = ts3ds_new(&tailscale_config);
        int tailscale_result = tailscale ? ts3ds_up(tailscale)
                                         : TS3DS_ERR_ARGUMENT;
        memset(cfg.tailscale_auth_key, 0,
               sizeof(cfg.tailscale_auth_key));
        tailscale_debug_flush(&tailscale_debug, term);
        if (!tailscale || tailscale_result != TS3DS_OK) {
            const char *reason = tailscale
                                     ? ts3ds_last_error(tailscale)
                                     : "invalid Tailscale configuration";
            char line[256];
            snprintf(line, sizeof(line),
                     "\x1b[31mTailscale error rc=%d status=%d:\x1b[0m "
                     "%.170s\r\n", tailscale_result,
                     tailscale ? (int)ts3ds_get_status(tailscale) : -1,
                     reason);
            terminal_write(term, line);
            snprintf(status_buf, sizeof(status_buf), "tailscale err");
            status_color = COLOR_ERR;
            tailscale_debug_set_runtime(&tailscale_debug);
            goto idle_loop;
        }
        {
            uint32_t ip = ts3ds_get_ipv4(tailscale);
            char line[96];
            snprintf(line, sizeof(line),
                     "\x1b[32mTailscale online:\x1b[0m %u.%u.%u.%u\r\n",
                     (unsigned)((ip >> 24) & 0xff),
                     (unsigned)((ip >> 16) & 0xff),
                     (unsigned)((ip >> 8) & 0xff),
                     (unsigned)(ip & 0xff));
            terminal_write(term, line);
        }
    }

    /* Pump one frame so the user sees the loading banner during the
     * (synchronous, ~5s) dict read.  The bottom screen still has the
     * keyboard rendered — the badge and mascot work normally. */
    render_connecting_frame(top, bot, r, term, kb, kbd);

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
                 "connecting to \x1b[33m%.48s@%.64s:%d\x1b[0m...\r\n",
                 cfg.user, cfg.host, cfg.port);
        terminal_write(term, banner);
    }

    /* Pump again so the user sees the loaded/connecting banners before
     * the SSH handshake blocks the main loop. */
    render_connecting_frame(top, bot, r, term, kb, kbd);

    ssh = reconnect_ssh(&cfg, tailscale, term, status_buf, sizeof(status_buf),
                        err, sizeof(err));

    tailscale_debug_set_runtime(&tailscale_debug);
    tailscale_debug_flush(&tailscale_debug, term);
    status_color = ssh ? COLOR_OK : COLOR_ERR;

    ssh = keychain_bootstrap(ssh, &cfg, term, r, kb, kbd, top, bot,
                             status_buf, sizeof(status_buf), &status_color,
                             err, sizeof(err));

    /* NOTE: cfg.passphrase / cfg.macos_keychain_password intentionally stay
     * in memory — the SELECT-key reconnect path re-authenticates with them.
     * Both are wiped by clear_secret() in the cleanup path at exit. */

idle_loop:
    {
        char rbuf[READ_BUFSZ];
        (void)status_buf; (void)status_color;  /* not currently rendered */

        /* ALERT triggers (mascot raises the red ✕):
         *
         *  - Interactive-stall (5 s): user sent bytes more recently than
         *    we've received any reply, and no reply has come for 5 s.
         *    Means the user is actively waiting and not getting through.
         *
         *  - Hard disconnect: ssh_read returned < 0 — libssh2 detected
         *    the socket is dead.  Once this fires we never recover in-
         *    session (no auto-reconnect logic), so the alert stays on
         *    until the user exits and relaunches DSSH.  Replaces the
         *    old "[disconnected]" terminal banner. */
        const int STALL_TX_NORX_S = 5;
        time_t last_rx_at = time(NULL);
        g_last_tx_at      = last_rx_at;
        int    stall_alert = 0;
        int    ssh_dead    = ssh ? 0 : 1;

        while (aptMainLoop()) {
            if (tailscale && ts3ds_get_status(tailscale) ==
                                 TS3DS_STATUS_ONLINE)
                ts3ds_poll(tailscale);
            tailscale_debug_flush(&tailscale_debug, term);
            hidScanInput();
            u32 down = hidKeysDown();
            u32 held = hidKeysHeld();
            circlePosition cpad;
            hidCircleRead(&cpad);

            /* M12 — AI-ask modal swallows input.  When the modal is up
             * (or animating in/out), suppress all routing to softkb /
             * keyboard / IME / mascot.  A=keep history, B=clear, touch
             * anywhere on the bottom screen=clear. */
            int modal_open = ai_modal_visible(aim);

            if (modal_open) {
                if (down & KEY_A) {
                    voice_ai_close_keep(voice);
                    ai_modal_close(aim);
                }
                if (down & KEY_B) {
                    voice_ai_close_clear(voice);
                    ai_modal_close(aim);
                }
            } else {
                /* L + START → AI ask mode.  Plain START → voice-IME mode.
                 * 3DS HOME exits any homebrew, so START is fully ours. */
                if (down & KEY_START) {
                    if (held & KEY_L) voice_ai_toggle(voice, ssh);
                    else              voice_toggle(voice, ssh);
                }
            }

            /* Per-frame voice tick — drives recording cap, async aux
             * channel write/eof/read, and forwards the transcribed
             * Chinese text to the main SSH channel via ssh_write.  Also
             * transitions us into VOICE_AI_SHOWING after a successful
             * AI transcribe; we open the modal once that happens. */
            voice_state_t prev_state = voice_state(voice);
            voice_tick(voice, ssh);
            voice_state_t cur_state = voice_state(voice);
            if (prev_state != VOICE_AI_SHOWING &&
                cur_state == VOICE_AI_SHOWING) {
                ai_modal_open(aim,
                              voice_ai_question(voice),
                              voice_ai_answer(voice));
            }
            ai_modal_tick(aim);

            /* Voice ↔ mascot link (level-driven, every frame).  RECORDING
             * → mic pose; TRANSCRIBING → ? pose; TYPING → no static pose
             * (the per-glyph mascot_type_kick below drives the typing
             * peck instead, same animation the soft keyboard uses).  We
             * must drive this every frame rather than on state edges,
             * because voice_toggle() (IDLE→RECORDING) and begin_transcribe()
             * (RECORDING→TRANSCRIBING) run inside the key-handler /
             * voice_tick before we sample prev_state, so an edge-triggered
             * link would sample prev==cur and skip the exact transition
             * we need to catch. */
            int rec  = (cur_state == VOICE_RECORDING);
            int thk  = (cur_state == VOICE_TRANSCRIBING);
            if (rec)      { mascot_set_recording(mc, 1); mascot_set_thinking(mc, 0); }
            else if (thk) { mascot_set_thinking(mc, 1); mascot_set_recording(mc, 0); }
            else          { mascot_set_recording(mc, 0); mascot_set_thinking(mc, 0); }

            /* Voice TYPING streams glyphs straight to ssh_write inside
             * voice.c, bypassing send_to_ssh — so the per-glyph typing
             * kick wired into send_to_ssh wouldn't fire.  Latch on the
             * voice side, consume here and kick manually so the crab
             * pecks in sync with each streamed character. */
            if (cur_state == VOICE_TYPING && voice_consume_typed(voice)) {
                mascot_type_kick(mc);
            }

            /* ── SSH receive ── */
            if (ssh && ssh_is_connected(ssh)) {
                ssh_keepalive_tick(ssh);
                int n = ssh_read(ssh, rbuf, sizeof(rbuf));
                if (n > 0) {
                    /* Capture for the debug page's recv-ring before the
                     * UTF-8 reassembler can chop the buffer up. */
                    softkb_record_recv(kb, rbuf, n);
                    feed_terminal(term, rbuf, n);
                    /* Interactive shells such as fish query cursor/device
                     * state and wait for the terminal emulator to reply.
                     * Return any responses queued by terminal_write_n(). */
                    flush_terminal_responses(ssh, term);
                    last_rx_at = time(NULL);
                } else if (n < 0) {
                    /* Hard disconnect.  Abort any in-flight voice work
                     * (its aux channel dies with the session), tear down
                     * the session, mark it dead so the mascot raises ✕,
                     * and show a one-shot red banner telling the user how
                     * to recover (SELECT reconnect).  Only fires on the
                     * 0→1 transition of ssh_dead — no per-frame spam. */
                    voice_abort(voice);
                    ssh_disconnect(ssh);
                    ssh = NULL;
                    ssh_dead = 1;
                    terminal_write(term,
                        "\r\n\x1b[31mConnection lost\r\n\x1b[0m"
                        "\x1b[33mPress SELECT to reconnect\r\n\x1b[0m");
                }
            }

            /* ALERT = hard disconnect, OR active interactive stall.
             * While voice input is active (RECORDING/TRANSCRIBING/TYPING)
             * the user is talking to the mic, not typing, so the SSH
             * channel going quiet is expected.  Skip the stall detector
             * entirely during voice so it can neither raise nor lower
             * the alert — the crab is owned by the voice-state link
             * above (mic / think poses).  A real hard disconnect is
             * still caught the moment voice goes idle again. */
            time_t now_t = time(NULL);
            voice_state_t vs = voice_state(voice);
            int voice_busy = (vs == VOICE_RECORDING || vs == VOICE_TRANSCRIBING ||
                              vs == VOICE_TYPING);
            if (!voice_busy) {
                int interactive_stall =
                    ssh && ssh_is_connected(ssh) &&
                    g_last_tx_at > last_rx_at &&
                    (now_t - g_last_tx_at) > STALL_TX_NORX_S;
                int want_alert = ssh_dead || interactive_stall;
                if (want_alert != stall_alert) {
                    stall_alert = want_alert;
                    mascot_set_alert(mc, stall_alert);
                }
            }

            /* ── SELECT reconnect (only when the session is dead) ──
             * A hard disconnect (lid-close sleep) leaves ssh_dead=1.
             * Pressing SELECT here reuses the loaded config to open a
             * fresh session, so the user recovers without relaunching.
             * While connected, SELECT falls through to keyboard_handle_input
             * as Esc (see the input_down mask below).
             *
             * select_consumed records that THIS frame's SELECT triggered
             * a reconnect.  It can't be derived from ssh_dead because a
             * successful reconnect clears ssh_dead to 0 in this same
             * frame — without an independent flag, that just-used SELECT
             * would slip through to keyboard_handle_input as a stray Esc
             * into the freshly opened session. */
            int select_consumed = 0;
            if (ssh_dead && !modal_open && (down & KEY_SELECT)) {
                select_consumed = 1;
                terminal_write(term,
                    "\x1b[33mReconnecting...\x1b[0m\r\n");
                /* Put the crab into its "looking up / waiting" pose before
                 * we flush the frame below, so the user sees it react. */
                mascot_set_reconnecting(mc, 1);
                /* ssh_connect_pubkey below is a blocking handshake that
                 * takes a few seconds.  The normal per-frame render at
                 * the bottom of this loop won't run until it returns, so
                 * the line above would stay hidden in the terminal buffer
                 * for the whole wait.  Flush one frame right now so the
                 * user actually sees "Reconnecting..." while the handshake
                 * is in flight, rather than a frozen blank screen. */
                C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
                C2D_TargetClear(top, C2D_Color32(0x1a, 0x1b, 0x26, 0xff));
                C2D_SceneBegin(top);
                renderer_draw_terminal(r, term);
                C2D_TargetClear(bot, C2D_Color32(0x18, 0x18, 0x25, 0xff));
                C2D_SceneBegin(bot);
                softkb_draw(kb, r, kbd);
                /* Bottom row: clock + mascot, mirroring the main render
                 * path so the reconnect frame isn't missing anything. */
                if (!softkb_in_debug(kb)) {
                    char clock_buf[24];
                    time_t now = time(NULL);
                    struct tm lt;
                    localtime_r(&now, &lt);
                    snprintf(clock_buf, sizeof(clock_buf),
                             "%02d-%02d %02d:%02d",
                             lt.tm_mon + 1, lt.tm_mday,
                             lt.tm_hour, lt.tm_min);
                    renderer_draw_text_px(2, 221, clock_buf, COLOR_DIM);
                    if (softkb_mascot_enabled(kb)) mascot_draw(mc);
                }
                C3D_FrameEnd(0);
                ssh = reconnect_ssh(&cfg, tailscale, term,
                                    status_buf, sizeof(status_buf),
                                    err, sizeof(err));
                /* Reconnected to a macOS host: its login keychain locked
                 * again with the old session — unlock it again so Claude
                 * Code keeps finding its credentials. */
                ssh = keychain_bootstrap(ssh, &cfg, term, r, kb, kbd,
                                         top, bot,
                                         status_buf, sizeof(status_buf),
                                         &status_color, err, sizeof(err));
                mascot_set_reconnecting(mc, 0);
                if (ssh) {
                    ssh_dead    = 0;
                    stall_alert = 0;
                    mascot_celebrate(mc);
                    last_rx_at  = time(NULL);
                    g_last_tx_at = last_rx_at;
                } else {
                    mascot_sadden(mc);
                    terminal_write(term,
                        "\x1b[31mReconnect failed; press SELECT to retry.\x1b[0m\r\n");
                }
            }

            /* ── Physical keys (Esc / Enter / BS / D-pad / R / scroll) ──
             * When the AI-ask modal is showing (or animating), suppress
             * the keyboard handler entirely so A=close-keep doesn't
             * also fire Enter to the SSH terminal, B=close-clear
             * doesn't also fire Backspace, etc.  We pass `down=0` so
             * the handler still ticks held-state timers (keeping
             * modifier release detection coherent for the post-modal
             * world) but registers no edge events. */
            u32 input_down = modal_open ? 0u : down;
            /* Mask SELECT when the session is dead or was just consumed by
             * the reconnect above, so it isn't emitted as Esc. */
            if (ssh_dead || select_consumed) input_down &= ~KEY_SELECT;
            const char *out = keyboard_handle_input(kbd, term, input_down, held, cpad.dy);
            if (out && !modal_open) send_to_ssh(ssh, term, out, (int)strlen(out), mc);

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
            if (modal_open) {
                /* While the AI-ask modal is up, any bottom-screen tap
                 * dismisses it (= clear history).  Block all routing
                 * to softkb / mascot. */
                if (touch_down) {
                    voice_ai_close_clear(voice);
                    ai_modal_close(aim);
                }
            } else if (touch_down && ty >= 214 && show_mascot) {
                if (mascot_hit_test(mc, tx, ty))
                    mascot_on_touched(mc, tx);
            } else {
                /* Pass the held flag (not just the down-edge) so softkb
                 * can detect holds for auto-repeat.  softkb derives the
                 * down-edge internally from prev_pressed.  On no-touch
                 * frames this also runs the release-fade path. */
                const char *kt = softkb_touch(kb, kbd, tx, ty, touch_pressed);
                if (kt) send_to_ssh(ssh, term, kt, (int)strlen(kt), mc);
            }

            /* Mascot ticks only when it's actually being shown.  When
             * paused this way it freezes in place; on re-enable it
             * resumes from wherever it stopped. */
            if (show_mascot) mascot_update(mc);

            /* ── Render ── */
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TargetClear(top, C2D_Color32(0x1a, 0x1b, 0x26, 0xff));
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

            /* M12 — modal lays over softkb + clock + mascot.  Drawn
             * last so its dimming overlay correctly covers everything
             * below. */
            ai_modal_draw(aim);

            C3D_FrameEnd(0);
        }

        /* Release voice's aux channel BEFORE freeing the session —
         * voice_free → release_aux would otherwise call libssh2_channel_*
         * on a freed LIBSSH2_SESSION (use-after-free). */
        voice_abort(voice);
        if (ssh) ssh_disconnect(ssh);
    }
    if (tailscale) {
        ts3ds_close(tailscale);
        tailscale = NULL;
        tailscale_debug_flush(&tailscale_debug, term);
    }
    net_fini();

cleanup:
    if (tailscale) {
        ts3ds_close(tailscale);
        tailscale = NULL;
        tailscale_debug_flush(&tailscale_debug, term);
    }
    clear_secret(cfg.passphrase, sizeof(cfg.passphrase));
    clear_secret(cfg.macos_keychain_password,
                 sizeof(cfg.macos_keychain_password));
    clear_secret(cfg.tailscale_auth_key,
                 sizeof(cfg.tailscale_auth_key));
    if (aim)   ai_modal_free(aim);
    if (voice) voice_free(voice);
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
