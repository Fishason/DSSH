#include "ssh_client.h"
#include <libssh2.h>
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

static void copy_libssh2_err(char *dst, int dst_sz, LIBSSH2_SESSION *sess,
                             const char *prefix) {
    if (!dst || dst_sz <= 0) return;
    char *msg = NULL;
    int msg_len = 0;
    int errnum = libssh2_session_last_error(sess, &msg, &msg_len, 0);
    snprintf(dst, dst_sz, "%s rc=%d: %.*s",
             prefix, errnum, msg_len, msg ? msg : "(no message)");
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

    if (libssh2_session_handshake(session, sock) != 0) {
        copy_libssh2_err(err_buf, err_sz, session, "handshake");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    /* Pre-check 1: can we open the key file at all? */
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
        char first[40] = {0};
        size_t n = fread(first, 1, sizeof(first) - 1, kf);
        fclose(kf);
        first[n] = 0;
        /* Only validate the start; libssh2 will fully parse later. */
        if (!strstr(first, "BEGIN") ||
            (!strstr(first, "RSA PRIVATE KEY") &&
             !strstr(first, "PRIVATE KEY"))) {
            snprintf(err_buf, err_sz,
                     "key file is not PEM. First bytes: %.30s", first);
            libssh2_session_disconnect(session, "bad key");
            libssh2_session_free(session);
            closesocket(sock);
            libssh2_exit();
            return NULL;
        }
    }

    /* Pre-check 2: ask server which auth methods it accepts for this user. */
    char *methods = libssh2_userauth_list(session, user, (unsigned int)strlen(user));
    if (methods && !strstr(methods, "publickey")) {
        snprintf(err_buf, err_sz,
                 "server does not allow publickey for %s. Allowed: %s",
                 user, methods);
        libssh2_session_disconnect(session, "no pubkey method");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    /* RSA pubkey from file. pubkey_path may be NULL — libssh2 derives it. */
    int auth = libssh2_userauth_publickey_fromfile_ex(
        session, user, (unsigned int)strlen(user),
        pubkey_path, key_path,
        passphrase ? passphrase : "");
    if (auth != 0) {
        copy_libssh2_err(err_buf, err_sz, session, "auth");
        libssh2_session_disconnect(session, "auth failed");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(session);
    if (!channel) {
        copy_libssh2_err(err_buf, err_sz, session, "channel_open");
        libssh2_session_disconnect(session, "channel failed");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    libssh2_channel_setenv(channel, "COLORTERM", "truecolor");
    if (libssh2_channel_request_pty(channel, "xterm-256color") != 0) {
        copy_libssh2_err(err_buf, err_sz, session, "pty");
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        libssh2_session_disconnect(session, "pty failed");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    if (libssh2_channel_shell(channel) != 0) {
        copy_libssh2_err(err_buf, err_sz, session, "shell");
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        libssh2_session_disconnect(session, "shell failed");
        libssh2_session_free(session);
        closesocket(sock);
        libssh2_exit();
        return NULL;
    }

    libssh2_session_set_blocking(session, 0);

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
