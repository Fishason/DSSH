#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = 0;
}

/* Remove an inline comment while preserving # inside quoted values. */
static void strip_comment(char *s) {
    char quote = 0;
    int escaped = 0;
    for (; *s; s++) {
        if (quote) {
            if (escaped) {
                escaped = 0;
            } else if (*s == '\\') {
                escaped = 1;
            } else if (*s == quote) {
                quote = 0;
            }
        } else if (*s == '\'' || *s == '"') {
            quote = *s;
        } else if (*s == '#') {
            *s = 0;
            return;
        }
    }
}

/* Strip matching quotes and decode only escaped quote/backslash pairs.
 * Other backslashes remain literal so existing paths/passwords do not change. */
static void unquote(char *s) {
    size_t len = strlen(s);
    if (len < 2 || (s[0] != '\'' && s[0] != '"') || s[len - 1] != s[0])
        return;

    char quote = s[0];
    char *src = s + 1;
    char *end = s + len - 1;
    char *dst = s;
    while (src < end) {
        if (*src == '\\' && src + 1 < end &&
            (src[1] == quote || src[1] == '\\')) {
            src++;
        }
        *dst++ = *src++;
    }
    *dst = 0;
}

static void set_str(char *dst, const char *src) {
    snprintf(dst, CONFIG_STR_MAX, "%s", src);
}

int config_load(ssh_config_t *cfg, const char *path) {
    /* defaults — placeholder values; user must override via SD config or
     * the binary will fail to authenticate. */
    set_str(cfg->host,       "your-server.example.com");
    cfg->port = 22;
    set_str(cfg->user,       "ubuntu");
    set_str(cfg->key_path,   "sdmc:/3ds/3dssh/id_rsa");
    cfg->passphrase[0] = 0;
    cfg->macos_keychain_password[0] = 0;

    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char line[CONFIG_STR_MAX * 2];
    while (fgets(line, sizeof(line), fp)) {
        strip_comment(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);
        unquote(val);
        if (!*key) continue;

        if      (!strcmp(key, "host"))       set_str(cfg->host,       val);
        else if (!strcmp(key, "port"))       cfg->port = atoi(val);
        else if (!strcmp(key, "user"))       set_str(cfg->user,       val);
        else if (!strcmp(key, "key_path"))   set_str(cfg->key_path,   val);
        else if (!strcmp(key, "passphrase")) set_str(cfg->passphrase, val);
        else if (!strcmp(key, "macos_keychain_password"))
            set_str(cfg->macos_keychain_password, val);
    }
    fclose(fp);
    return 1;
}
