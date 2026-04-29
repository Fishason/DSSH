#ifndef CONFIG_H
#define CONFIG_H

/*
 * Loads SSH connection config from sdmc:/3ds/3dssh/config.ini at startup.
 * Falls back to compiled-in defaults if the file is missing or unparseable.
 *
 * Format (simple key=value, # for comments):
 *   host = 52.76.104.33
 *   port = 22
 *   user = ubuntu
 *   key_path = sdmc:/3ds/3dssh/id_rsa
 *   passphrase =                  # leave empty for unencrypted keys
 */

#define CONFIG_STR_MAX 256

typedef struct {
    char host[CONFIG_STR_MAX];
    int  port;
    char user[CONFIG_STR_MAX];
    char key_path[CONFIG_STR_MAX];
    char passphrase[CONFIG_STR_MAX];
} ssh_config_t;

/* Fills cfg with defaults then overlays values from config file (if present).
 * Returns 1 if the file was loaded successfully, 0 if defaults only. */
int config_load(ssh_config_t *cfg, const char *path);

#endif /* CONFIG_H */
