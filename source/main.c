#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>

/*
 * 3dssh — M0 toolchain smoke test.
 *
 * Goal: prove that devkitPro can build a .3dsx, that we link against libctru,
 * and that the result loads on a real 3DS / emulator. No SSH yet.
 *
 * Press START to exit cleanly back to Homebrew Launcher.
 */

int main(int argc, char *argv[]) {
    PrintConsole topScreen, bottomScreen;

    gfxInitDefault();
    consoleInit(GFX_TOP, &topScreen);
    consoleInit(GFX_BOTTOM, &bottomScreen);

    consoleSelect(&topScreen);
    printf("\x1b[2J");
    printf("3dssh M0 — toolchain smoke test\n");
    printf("--------------------------------\n");
    printf("\n");
    printf("If you see this, devkitPro toolchain works.\n");
    printf("\n");
    printf("Next milestones:\n");
    printf("  M1 SSH connect to localhost\n");
    printf("  M2 Config + RSA pubkey auth\n");
    printf("  M3 Hardware key remap\n");
    printf("  M4-M5 Soft keyboard\n");
    printf("  M6 Chinese fonts\n");
    printf("  M7 Pinyin IME\n");
    printf("\n");
    printf("Press START to exit.\n");

    consoleSelect(&bottomScreen);
    printf("\x1b[2J");
    printf("3dssh — bottom screen reserved\n");
    printf("for soft keyboard (M4+).\n");

    while (aptMainLoop()) {
        gspWaitForVBlank();
        gfxSwapBuffers();
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) break;
    }

    gfxExit();
    return 0;
}
