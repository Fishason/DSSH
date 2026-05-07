#include "ssh_client.h"
#include <libssh2.h>
#include <mbedtls/pk.h>
#include <mbedtls/error.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct ssh_client_t {
    int              sock;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    int              connected;
};

static void copy_err(char *dst, int dst_sz, const char *src) {
    if (dst && dst_sz > 0) {
        snprintf(dst, dst_sz, "%s", src ? src : "(unknown)");
    }
}

/* Capture both the function's own return value and libssh2 session's last
 * recorded error. They can disagree, especially in mbedTLS backend paths
 * where some failures don't get logged into the session. */
static void copy_libssh2_err(char *dst, int dst_sz, LIBSSH2_SESSION *sess,
                             const char *prefix, int fn_rc) {
    if (!dst || dst_sz <= 0) return;
    char *msg = NULL;
    int msg_len = 0;
    int sess_errnum = libssh2_session_last_error(sess, &msg, &msg_len, 0);
    const char *msg_disp = (msg && msg_len > 0) ? msg : "(empty)";
    int eff_len = (msg && msg_len > 0) ? msg_len : 7;
    snprintf(dst, dst_sz, "%s fn=%d sess=%d \"%.*s\"",
             prefix, fn_rc, sess_errnum, eff_len, msg_disp);
}

static int tcp_connect(const char *host, int port, char *err, int err_sz) {
    struct hostent *he = gethostbyname(host);
    if (!he) {
        copy_err(err, err_sz, "DNS lookup failed");
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        copy_err(err, err_sz, "socket() failed");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        copy_err(err, err_sz, "connect() refused / unreachable");
        closesocket(sock);
        return -1;
    }
    return sock;
}

ssh_client_t *ssh_connect_pubkey(const char *host, int port,
                                  const char *user,
                                  const char *key_path,
                                  const char *pubkey_path,
                                  const char *passphrase,
                                  char *err_buf, int err_sz) {
    if (libssh2_init(0) != 0) {
        copy_err(err_buf, err_sz, "libssh2_init failed");
        return NULL;
    }

    int sock = tcp_connect(host, port, err_buf, err_sz);
    if (sock < 0) {
        libssh2_exit();
        return NULL;
    }

    LIBSSH2_SESSION *session = libssh2_session_init();
    if (!session) {
        copy_err(err_buf, err_sz, "session_init failed");
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }
    libssh2_session_set_blocking(session, 1);

    int hs_rc = libssh2_session_handshake(session, sock);
    if (hs_rc != 0) {
        copy_libssh2_err(err_buf, err_sz, session, "handshake", hs_rc);
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    /* Pre-check 1: can we open the key file at all? Capture size + header. */
    long key_size = 0;
    {
        FILE *kf = fopen(key_path, "rb");
        if (!kf) {
            snprintf(err_buf, err_sz,
                     "open key file failed: %s (errno=%d)", key_path, errno);
            libssh2_session_disconnect(session, "no key");
            libssh2_session_free(session);
            closesocket(sock);
            libssh2_exit();
            return NULL;
        }
        fseek(kf, 0, SEEK_END);
        key_size = ftell(kf);
        fseek(kf, 0, SEEK_SET);
        char first[40] = {0};
        size_t n = fread(first, 1, sizeof(first) - 1, kf);
        fclose(kf);
        first[n] = 0;
        if (!strstr(first, "BEGIN") ||
            (!strstr(first, "RSA PRIVATE KEY") &&
             !strstr(first, "PRIVATE KEY"))) {
            snprintf(err_buf, err_sz,
                     "key not PEM (size=%ld). Head: %.30s", key_size, first);
            libssh2_session_disconnect(session, "bad key");
            libssh2_session_free(session);
            closesocket(sock);
            libssh2_exit();
            return NULL;
        }
    }

    /* Pre-check 2: ask server which auth methods it accepts for this user.
     * Always include the methods string in any later auth error for context. */
    char *methods = libssh2_userauth_list(session, user, (unsigned int)strlen(user));
    if (methods && !strstr(methods, "publickey")) {
        snprintf(err_buf, err_sz,
                 "server rejects publickey for %s. Allowed: %s",
                 user, methods);
        libssh2_session_disconnect(session, "no pubkey method");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    /* Pre-check 3: parse key directly with mbedTLS so we get its specific
     * error code (libssh2's mbedTLS backend swallows mbedTLS errors and just
     * returns -1). This is purely diagnostic — we then let libssh2 do its
     * own parse during userauth_publickey_fromfile_ex. */
    {
        mbedtls_pk_context tctx;
        mbedtls_pk_init(&tctx);
        int mb_rc = mbedtls_pk_parse_keyfile(
            &tctx, key_path,
            (passphrase && *passphrase) ? passphrase : NULL);
        if (mb_rc != 0) {
            char mb_msg[80] = {0};
            mbedtls_strerror(mb_rc, mb_msg, sizeof(mb_msg));
            snprintf(err_buf, err_sz,
                     "mbedTLS parse: rc=-0x%04X %s | path=%s",
                     (unsigned)-mb_rc, mb_msg, key_path);
            mbedtls_pk_free(&tctx);
            libssh2_session_disconnect(session, "mbedtls parse");
            libssh2_session_free(session);
            closesocket(sock);
            libssh2_exit();
            return NULL;
        }
        /* Stash success info into err_buf as a side-channel; if any later
         * step fails, the caller will still see the type/bits we saw. */
        snprintf(err_buf, err_sz, "(parse OK: %s %u-bit) ",
                 mbedtls_pk_get_name(&tctx),
                 (unsigned)mbedtls_pk_get_bitlen(&tctx));
        mbedtls_pk_free(&tctx);
    }

    /* RSA pubkey from file. pubkey_path may be NULL — libssh2 derives it. */
    int auth = libssh2_userauth_publickey_fromfile_ex(
        session, user, (unsigned int)strlen(user),
        pubkey_path, key_path,
        passphrase ? passphrase : "");
    if (auth != 0) {
        char inner[160] = {0};
        copy_libssh2_err(inner, sizeof(inner), session, "auth", auth);
        /* Note: err_buf currently holds "(parse OK: RSA 4096-bit) " prefix
         * from the mbedTLS pre-check success path; preserve it so we know
         * mbedTLS itself parsed the key fine and the issue is downstream. */
        char prefix[64] = {0};
        snprintf(prefix, sizeof(prefix), "%.63s", err_buf);
        snprintf(err_buf, err_sz,
                 "%s%s | key_size=%ld user=%s methods=[%s]",
                 prefix, inner, key_size, user, methods ? methods : "?");
        libssh2_session_disconnect(session, "auth failed");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }
    /* Auth succeeded — clear the diagnostic prefix from err_buf. */
    err_buf[0] = 0;

    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(session);
    if (!channel) {
        copy_libssh2_err(err_buf, err_sz, session, "channel_open", -1);
        libssh2_session_disconnect(session, "channel failed");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    libssh2_channel_setenv(channel, "COLORTERM", "truecolor");
    int pty_rc = libssh2_channel_request_pty(channel, "xterm-256color");
    if (pty_rc != 0) {
        copy_libssh2_err(err_buf, err_sz, session, "pty", pty_rc);
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        libssh2_session_disconnect(session, "pty failed");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    int sh_rc = libssh2_channel_shell(channel);
    if (sh_rc != 0) {
        copy_libssh2_err(err_buf, err_sz, session, "shell", sh_rc);
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        libssh2_session_disconnect(session, "shell failed");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    libssh2_session_set_blocking(session, 0);

    /* Enable keepalive so we can tell idle-connection from broken-network.
     * want_reply=1 makes the server SSH_MSG_GLOBAL_REQUEST/keepalive
     * round-trip so any successful round produces ssh_read traffic
     * — main.c's stall detector watches for that. */
    libssh2_keepalive_config(session, 1, 10);

    ssh_client_t *ssh = calloc(1, sizeof(*ssh));
    if (!ssh) {
        copy_err(err_buf, err_sz, "out of memory");
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        libssh2_session_disconnect(session, "oom");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    ssh->sock      = sock;
    ssh->session   = session;
    ssh->channel   = channel;
    ssh->connected = 1;
    return ssh;
}

void ssh_disconnect(ssh_client_t *ssh) {
    if (!ssh) return;
    if (ssh->channel) {
        libssh2_channel_close(ssh->channel);
        libssh2_channel_free(ssh->channel);
    }
    if (ssh->session) {
        libssh2_session_disconnect(ssh->session, "Bye");
        libssh2_session_free(ssh->session);
    }
    if (ssh->sock >= 0) closesocket(ssh->sock);
    libssh2_exit();
    free(ssh);
}

int ssh_is_connected(ssh_client_t *ssh) { return ssh && ssh->connected; }

int ssh_read(ssh_client_t *ssh, char *buf, int len) {
    if (!ssh || !ssh->connected) return -1;
    ssize_t n = libssh2_channel_read(ssh->channel, buf, (size_t)len);
    if (n == LIBSSH2_ERROR_EAGAIN) return 0;
    if (n < 0) { ssh->connected = 0; return -1; }
    if (libssh2_channel_eof(ssh->channel)) ssh->connected = 0;
    return (int)n;
}

int ssh_write(ssh_client_t *ssh, const char *buf, int len) {
    if (!ssh || !ssh->connected || len <= 0) return -1;
    int sent = 0;
    while (sent < len) {
        ssize_t n = libssh2_channel_write(ssh->channel,
                                          buf + sent,
                                          (size_t)(len - sent));
        if (n == LIBSSH2_ERROR_EAGAIN) break;
        if (n < 0) { ssh->connected = 0; return -1; }
        sent += (int)n;
    }
    return sent;
}

void ssh_set_pty_size(ssh_client_t *ssh, int cols, int rows) {
    if (!ssh || !ssh->connected || !ssh->channel) return;
    libssh2_channel_request_pty_size(ssh->channel, cols, rows);
}

void ssh_keepalive_tick(ssh_client_t *ssh) {
    if (!ssh || !ssh->connected || !ssh->session) return;
    /* libssh2 internally tracks the configured interval (10s) and
     * only actually emits a packet when it's due — calling every
     * frame is cheap and lets the library own the timing. */
    int unused;
    libssh2_keepalive_send(ssh->session, &unused);
}

/* ── Auxiliary channel implementation ─────────────────────────────── */

struct ssh_aux_channel_t {
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    int              eof_sent;
};

ssh_aux_channel_t *ssh_aux_exec(ssh_client_t *ssh, const char *cmd,
                                char *err_buf, int err_sz) {
    if (!ssh || !ssh->connected || !ssh->session || !cmd) return NULL;

    /* Briefly flip session to blocking mode so open + exec round-trip
     * inline.  This stalls the main loop for ~one TCP RTT (~50-200 ms),
     * which is invisible alongside the multi-second voice transcription
     * we're about to wait for.  An async open would be cleaner but adds
     * substantial state-machine code for no perceptible UX gain. */
    libssh2_session_set_blocking(ssh->session, 1);

    LIBSSH2_CHANNEL *ch = libssh2_channel_open_session(ssh->session);
    if (!ch) {
        copy_libssh2_err(err_buf, err_sz, ssh->session, "aux open_session", 0);
        libssh2_session_set_blocking(ssh->session, 0);
        return NULL;
    }
    int rc = libssh2_channel_exec(ch, cmd);
    if (rc != 0) {
        copy_libssh2_err(err_buf, err_sz, ssh->session, "aux exec", rc);
        libssh2_channel_free(ch);
        libssh2_session_set_blocking(ssh->session, 0);
        return NULL;
    }

    libssh2_session_set_blocking(ssh->session, 0);

    ssh_aux_channel_t *a = calloc(1, sizeof(*a));
    if (!a) {
        copy_err(err_buf, err_sz, "aux alloc oom");
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
        return NULL;
    }
    a->session = ssh->session;
    a->channel = ch;
    return a;
}

int ssh_aux_write(ssh_aux_channel_t *aux, const char *buf, int len) {
    if (!aux || !aux->channel || len < 0) return -1;
    if (len == 0) return 0;
    ssize_t n = libssh2_channel_write(aux->channel, buf, (size_t)len);
    if (n == LIBSSH2_ERROR_EAGAIN) return 0;
    if (n < 0) return -1;
    return (int)n;
}

int ssh_aux_send_eof(ssh_aux_channel_t *aux) {
    if (!aux || !aux->channel) return -1;
    if (aux->eof_sent) return 0;
    int rc = libssh2_channel_send_eof(aux->channel);
    if (rc == 0) { aux->eof_sent = 1; return 0; }
    if (rc == LIBSSH2_ERROR_EAGAIN) return 1;
    return -1;
}

int ssh_aux_read(ssh_aux_channel_t *aux, char *buf, int len) {
    if (!aux || !aux->channel || len <= 0) return -1;
    ssize_t n = libssh2_channel_read(aux->channel, buf, (size_t)len);
    if (n == LIBSSH2_ERROR_EAGAIN) return 0;
    if (n < 0) return -1;
    return (int)n;
}

int ssh_aux_eof(const ssh_aux_channel_t *aux) {
    if (!aux || !aux->channel) return 1;
    /* libssh2_channel_eof takes a non-const pointer in older libssh2
     * versions; drop const here to match. */
    return libssh2_channel_eof((LIBSSH2_CHANNEL *)aux->channel) ? 1 : 0;
}

void ssh_aux_close(ssh_aux_channel_t *aux) {
    if (!aux) return;
    if (aux->channel) {
        libssh2_channel_close(aux->channel);
        libssh2_channel_free(aux->channel);
    }
    free(aux);
}
