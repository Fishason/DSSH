#ifndef CONFIG_H
#define CONFIG_H

/*
 * Loads SSH connection config from sdmc:/3ds/3dssh/config.ini at startup.
 * Falls back to compiled-in defaults if the file is missing or unparseable.
 *
 * Format (simple key=value, # for comments):
 *   host = your-server.example.com
 *   port = 22
 *   user = ubuntu
 *   key_path = sdmc:/3ds/3dssh/id_rsa
 *   passphrase =                  # leave empty for unencrypted keys
 *   macos_keychain_password =     # quote values containing # or edge spaces
 *   tailscale_auth_key =          # setting a key enables Tailscale
 *   tailscale_hostname = dssh-3ds
 *   tailscale_state = sdmc:/3ds/3dssh/tailscale.state
 */

#define CONFIG_STR_MAX 256

typedef struct {
    char host[CONFIG_STR_MAX];
    int  port;
    char user[CONFIG_STR_MAX];
    char key_path[CONFIG_STR_MAX];
    char passphrase[CONFIG_STR_MAX];
    char macos_keychain_password[CONFIG_STR_MAX];
    char tailscale_auth_key[CONFIG_STR_MAX];
    char tailscale_hostname[CONFIG_STR_MAX];
    char tailscale_state[CONFIG_STR_MAX];
    char tailscale_control_url[CONFIG_STR_MAX];
} ssh_config_t;

/* Fills cfg with defaults then overlays values from config file (if present).
 * Returns 1 if the file was loaded successfully, 0 if defaults only. */
int config_load(ssh_config_t *cfg, const char *path);

/* Tailscale starts automatically when an auth key is configured or when a
 * persisted node state exists at tailscale_state. */
int config_tailscale_should_start(const ssh_config_t *cfg);

#endif /* CONFIG_H */
