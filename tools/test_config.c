#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "../source/config.h"

static int failures = 0;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

int main(void) {
    ssh_config_t cfg;
    CHECK(config_load(&cfg, "/definitely/not/a/dssh/config") == 0,
          "missing config should use defaults");
    CHECK(cfg.macos_keychain_password[0] == 0,
          "keychain password should default empty");
    CHECK(cfg.tailscale_auth_key[0] == 0,
          "Tailscale auth key should default empty");
    CHECK(strcmp(cfg.tailscale_hostname, "dssh-3ds") == 0,
          "Tailscale hostname default");
    CHECK(strcmp(cfg.tailscale_state,
                 "sdmc:/3ds/3dssh/tailscale.state") == 0,
          "Tailscale state path default");
    CHECK(config_tailscale_should_start(&cfg) == 0,
          "Tailscale should stay off without key or state");

    char path[] = "/tmp/dssh-config-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp failed");
    if (fd < 0) return 1;

    FILE *fp = fdopen(fd, "w");
    CHECK(fp != NULL, "fdopen failed");
    if (!fp) {
        close(fd);
        unlink(path);
        return 1;
    }
    fputs("host = mac.example # comment\n"
          "user = alice\n"
          "macos_keychain_password = \"  p#ass\\\"word  \" # comment\n"
          "tailscale_auth_key = test-key\n"
          "tailscale_hostname =\n"
          "tailscale_state =\n"
          "tailscale_control_url =\n",
          fp);
    fclose(fp);

    CHECK(config_load(&cfg, path) == 1, "temporary config should load");
    unlink(path);
    CHECK(strcmp(cfg.host, "mac.example") == 0, "inline comment parsing");
    CHECK(strcmp(cfg.macos_keychain_password, "  p#ass\"word  ") == 0,
          "quoted password should preserve spaces, #, and escaped quote");
    CHECK(config_tailscale_should_start(&cfg) == 1,
          "auth key should enable Tailscale automatically");
    CHECK(strcmp(cfg.tailscale_hostname, "dssh-3ds") == 0,
          "empty Tailscale hostname should restore default");
    CHECK(strcmp(cfg.tailscale_state,
                 "sdmc:/3ds/3dssh/tailscale.state") == 0,
          "empty Tailscale state path should restore default");
    CHECK(strcmp(cfg.tailscale_control_url,
                 "https://controlplane.tailscale.com") == 0,
          "empty control URL should restore default");

    cfg.tailscale_auth_key[0] = 0;
    snprintf(cfg.tailscale_state, sizeof(cfg.tailscale_state), "%s", path);
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    CHECK(fd >= 0, "Tailscale state fixture create failed");
    if (fd >= 0) close(fd);
    CHECK(config_tailscale_should_start(&cfg) == 1,
          "persisted state should enable Tailscale without an auth key");
    unlink(path);
    CHECK(config_tailscale_should_start(&cfg) == 0,
          "missing state and empty auth key should disable Tailscale");

    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    CHECK(fd >= 0, "legacy-key config create failed");
    if (fd >= 0) {
        fp = fdopen(fd, "w");
        CHECK(fp != NULL, "legacy-key fdopen failed");
        if (fp) {
            fputs("keychain_password = should-not-load\n", fp);
            fclose(fp);
            CHECK(config_load(&cfg, path) == 1,
                  "legacy-key config should still be readable");
            CHECK(cfg.macos_keychain_password[0] == 0,
                  "unreleased legacy key should not be accepted");
        } else {
            close(fd);
        }
        unlink(path);
    }

    if (failures) {
        fprintf(stderr, "%d config test(s) failed\n", failures);
        return 1;
    }
    puts("config tests passed");
    return 0;
}
