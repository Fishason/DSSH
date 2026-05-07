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

/* Drive libssh2 keepalive — call once per main-loop iteration.  Internal
 * timer means actual SSH_MSG_GLOBAL_REQUEST packets only transmit at
 * the configured interval (10s by default; see ssh_client.c).  Each
 * successful round-trip produces ssh_read traffic the caller can use
 * to detect a stalled connection. */
void ssh_keepalive_tick(ssh_client_t *ssh);

/* ── Auxiliary channel API ──────────────────────────────────────────
 * Opens a SECOND channel on the same SSH session (reusing auth and
 * encryption) to exec a remote command — used by the voice-input
 * subsystem to stream PCM audio to dssh-whisper-shim and read back the
 * transcribed Chinese UTF-8 text.  The main shell channel is unaffected.
 *
 * Lifecycle: ssh_aux_exec → ssh_aux_write*N → ssh_aux_send_eof →
 *            ssh_aux_read*N (until ssh_aux_eof) → ssh_aux_close.
 * All read/write/send_eof calls are non-blocking and may return 0
 * meaning "try again next frame".
 */
typedef struct ssh_aux_channel_t ssh_aux_channel_t;

/* Open the aux channel and exec `cmd` on the remote.  This call briefly
 * flips the session to blocking mode while it round-trips, then returns
 * a non-blocking aux handle for the caller to pump per-frame.
 * Returns NULL on failure (err_buf populated when non-NULL). */
ssh_aux_channel_t *ssh_aux_exec(ssh_client_t *ssh, const char *cmd,
                                char *err_buf, int err_sz);

/* Non-blocking write.  Returns bytes written (>=0), 0 = EAGAIN, -1 = err. */
int  ssh_aux_write(ssh_aux_channel_t *aux, const char *buf, int len);

/* Tell the remote no more bytes coming.  Returns 0 done, 1 EAGAIN, -1 err. */
int  ssh_aux_send_eof(ssh_aux_channel_t *aux);

/* Non-blocking read.  Returns bytes read (>0), 0 = EAGAIN/no-data-yet,
 * -1 = error.  Use ssh_aux_eof() to distinguish "more later" from EOF. */
int  ssh_aux_read(ssh_aux_channel_t *aux, char *buf, int len);

/* Returns 1 if the remote process has closed its stdout (response over). */
int  ssh_aux_eof(const ssh_aux_channel_t *aux);

/* Close + free.  Idempotent. */
void ssh_aux_close(ssh_aux_channel_t *aux);

#endif /* SSH_CLIENT_H */
