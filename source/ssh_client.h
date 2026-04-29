#ifndef SSH_CLIENT_H
#define SSH_CLIENT_H

/*
 * Thin wrapper around libssh2 for the 3DS.
 * RSA-4096 public-key authentication (mbedTLS backend doesn't support ed25519).
 * Single shell channel per connection, non-blocking I/O after handshake.
 */

typedef struct ssh_client_t ssh_client_t;

/* Connect, authenticate, open shell with PTY. Returns NULL on any failure.
 *   key_path        — path to PEM-format RSA private key (e.g. "sdmc:/3ds/3dssh/id_rsa").
 *   pubkey_path     — public key path or NULL to let libssh2 derive from private.
 *   passphrase      — passphrase for encrypted key, or NULL for unencrypted keys.
 *   err_buf, err_sz — optional human-readable error captured on failure.
 */
ssh_client_t *ssh_connect_pubkey(const char *host, int port,
                                 const char *user,
                                 const char *key_path,
                                 const char *pubkey_path,
                                 const char *passphrase,
                                 char *err_buf, int err_sz);

void ssh_disconnect(ssh_client_t *ssh);
int  ssh_is_connected(ssh_client_t *ssh);

/* Returns: bytes read (>=0), or -1 on disconnect. 0 means EAGAIN (try later). */
int  ssh_read(ssh_client_t *ssh, char *buf, int len);

/* Returns: bytes written, or -1 on disconnect. May write fewer than len bytes. */
int  ssh_write(ssh_client_t *ssh, const char *buf, int len);

void ssh_set_pty_size(ssh_client_t *ssh, int cols, int rows);

#endif /* SSH_CLIENT_H */
