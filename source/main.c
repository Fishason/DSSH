/*
 * 3dssh — Nintendo 3DS SSH client (M1 milestone).
 *
 * Scope of this milestone:
 *   - libssh2 + mbedTLS over 3DS BSD sockets
 *   - RSA public-key authentication (key from SD card)
 *   - sdmc:/3ds/3dssh/config.ini for host/user/key path
 *   - Console-mode display (built-in libctru ANSI subset)
 *   - System swkbd applet for input (real keyboard comes in M4)
 *
 * Not yet:
 *   - citro2d rendering, custom ANSI parser, soft keyboard, IME
 *
 * Controls:
 *   X        — open soft keyboard, type a line, press OK to send
 *   A        — send Enter (\r)
 *   B        — send Backspace (\x7f)
 *   D-pad    — arrow keys
 *   L        — Ctrl-C (interrupt)
 *   R        — Ctrl-D (EOF / logout)
 *   START    — disconnect and exit to HBL
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <3ds.h>

#include "ssh_client.h"
#include "config.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000  /* 1 MB context for SOC service */

#define CONFIG_PATH     "sdmc:/3ds/3dssh/config.ini"
#define READ_BUFSZ      2048
#define INPUT_BUFSZ     512

static u32 *soc_buf = NULL;

static int net_init(char *err, int err_sz) {
    soc_buf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!soc_buf) {
        snprintf(err, err_sz, "memalign failed");
        return -1;
    }
    int rc = socInit(soc_buf, SOC_BUFFERSIZE);
    if (rc != 0) {
        snprintf(err, err_sz, "socInit failed: 0x%08lX", (unsigned long)rc);
        return -1;
    }
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

int main(int argc, char *argv[]) {
    PrintConsole topScreen, bottomScreen;
    gfxInitDefault();
    consoleInit(GFX_TOP,    &topScreen);
    consoleInit(GFX_BOTTOM, &bottomScreen);

    consoleSelect(&bottomScreen);
    printf("\x1b[2J\x1b[H3dssh M1\n");
    printf("--------\n");
    printf("X: type   A: Enter   B: BS\n");
    printf("D-pad: arrows  L: ^C  R: ^D\n");
    printf("START: quit\n\n");

    char err[256] = {0};

    /* Load config from SD. */
    ssh_config_t cfg;
    int loaded = config_load(&cfg, CONFIG_PATH);
    printf("config: %s\n", loaded ? "loaded from SD" : "DEFAULTS (no SD config)");
    printf("  host: %s:%d\n", cfg.host, cfg.port);
    printf("  user: %s\n", cfg.user);
    printf("  key:  %s\n", cfg.key_path);

    /* Init network. */
    if (net_init(err, sizeof(err)) != 0) {
        printf("\nNET ERROR: %s\n", err);
        printf("Press START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        gfxExit();
        return 1;
    }
    printf("\nnet: OK\n");

    /* Connect. */
    printf("connecting (this can take 5-10 sec)...\n");
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();

    ssh_client_t *ssh = ssh_connect_pubkey(
        cfg.host, cfg.port, cfg.user,
        cfg.key_path, NULL,
        cfg.passphrase[0] ? cfg.passphrase : NULL,
        err, sizeof(err));

    if (!ssh) {
        printf("\nSSH ERROR:\n  %s\n", err);
        printf("\nCheck:\n");
        printf(" - SD has %s\n", cfg.key_path);
        printf(" - server allows pubkey for %s\n", cfg.user);
        printf(" - WiFi reachable\n");
        printf("\nPress START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        net_fini();
        gfxExit();
        return 2;
    }

    printf("connected!\n");
    /* Top screen now shows the SSH session. Initial cols/rows match top
     * screen console: 50 cols x 30 rows. */
    ssh_set_pty_size(ssh, 50, 30);
    consoleSelect(&topScreen);
    printf("\x1b[2J\x1b[H");

    char rbuf[READ_BUFSZ];
    char ibuf[INPUT_BUFSZ];

    while (aptMainLoop() && ssh_is_connected(ssh)) {
        /* Pump SSH read into top-screen console. */
        int n = ssh_read(ssh, rbuf, sizeof(rbuf) - 1);
        if (n > 0) {
            rbuf[n] = 0;
            /* fwrite would be cleaner but PrintConsole's fputs/printf both work. */
            fwrite(rbuf, 1, (size_t)n, stdout);
            fflush(stdout);
        } else if (n < 0) {
            break;
        }

        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) break;
        if (down & KEY_A)     ssh_write(ssh, "\r",     1);
        if (down & KEY_B)     ssh_write(ssh, "\x7f",   1);
        if (down & KEY_DUP)   ssh_write(ssh, "\x1b[A", 3);
        if (down & KEY_DDOWN) ssh_write(ssh, "\x1b[B", 3);
        if (down & KEY_DRIGHT)ssh_write(ssh, "\x1b[C", 3);
        if (down & KEY_DLEFT) ssh_write(ssh, "\x1b[D", 3);
        if (down & KEY_L)     ssh_write(ssh, "\x03",   1); /* Ctrl-C */
        if (down & KEY_R)     ssh_write(ssh, "\x04",   1); /* Ctrl-D */
        if (down & KEY_X) {
            consoleSelect(&bottomScreen);
            if (prompt_swkbd("type a line", ibuf, sizeof(ibuf))) {
                int ilen = (int)strlen(ibuf);
                if (ilen > 0) ssh_write(ssh, ibuf, ilen);
            }
            consoleSelect(&topScreen);
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    consoleSelect(&bottomScreen);
    printf("\ndisconnecting...\n");
    ssh_disconnect(ssh);
    net_fini();
    printf("press START to exit.\n");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
        gfxSwapBuffers();
    }

    gfxExit();
    return 0;
}
