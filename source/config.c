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

static void set_str(char *dst, const char *src) {
    snprintf(dst, CONFIG_STR_MAX, "%s", src);
}

int config_load(ssh_config_t *cfg, const char *path) {
    /* defaults — placeholder values; user must override via SD config or
     * the binary will fail to authenticate. */
    set_str(cfg->host,       "52.76.104.33");
    cfg->port = 22;
    set_str(cfg->user,       "ubuntu");
    set_str(cfg->key_path,   "sdmc:/3ds/3dssh/id_rsa");
    cfg->passphrase[0] = 0;

    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char line[CONFIG_STR_MAX * 2];
    while (fgets(line, sizeof(line), fp)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);
        if (!*key) continue;

        if      (!strcmp(key, "host"))       set_str(cfg->host,       val);
        else if (!strcmp(key, "port"))       cfg->port = atoi(val);
        else if (!strcmp(key, "user"))       set_str(cfg->user,       val);
        else if (!strcmp(key, "key_path"))   set_str(cfg->key_path,   val);
        else if (!strcmp(key, "passphrase")) set_str(cfg->passphrase, val);
    }
    fclose(fp);
    return 1;
}
