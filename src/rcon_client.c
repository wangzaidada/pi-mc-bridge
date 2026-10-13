/* rcon_client.c - see rcon_client.h */
#include "rcon_client.h"
#include "parser.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---------- framing ---------- */

static void put_i32le(DynBuf *out, int32_t v)
{
    unsigned char b[4];
    uint32_t u = (uint32_t)v;
    b[0] = (unsigned char)(u & 0xFF);
    b[1] = (unsigned char)((u >> 8) & 0xFF);
    b[2] = (unsigned char)((u >> 16) & 0xFF);
    b[3] = (unsigned char)((u >> 24) & 0xFF);
    dynbuf_append(out, (char *)b, 4);
}

static int32_t get_i32le(const unsigned char *b)
{
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

void rcon_pack(DynBuf *out, int id, int type, const char *payload)
{
    size_t plen = payload ? strlen(payload) : 0;
    int32_t len = (int32_t)(4 + 4 + plen + 2); /* id + type + payload + 2 NULs */
    put_i32le(out, len);
    put_i32le(out, id);
    put_i32le(out, type);
    if (plen)
        dynbuf_append(out, payload, plen);
    dynbuf_append_char(out, '\0');
    dynbuf_append_char(out, '\0');
}

int rcon_unpack(const char *body, size_t body_len, int *id, int *type,
                DynBuf *payload_out)
{
    if (body_len < 10) /* id + type + at least the 2 NULs */
        return RCON_ERR_PROTO;
    const unsigned char *b = (const unsigned char *)body;
    if (id)
        *id = get_i32le(b);
    if (type)
        *type = get_i32le(b + 4);
    size_t plen = body_len - 10;
    if (payload_out && plen)
        dynbuf_append(payload_out, body + 8, plen);
    return RCON_ERR_NONE;
}

bool rcon_valid_player(const char *name)
{
    return is_valid_player_name(name);
}

/* ---------- socket I/O ---------- */

static int send_all(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, buf + off, n - off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return RCON_ERR_TIMEOUT;
            return RCON_ERR_SOCKET;
        }
        off += (size_t)w;
    }
    return RCON_ERR_NONE;
}

static int recv_all(int fd, char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, buf + off, n - off, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return RCON_ERR_TIMEOUT;
            return RCON_ERR_SOCKET;
        }
        if (r == 0)
            return RCON_ERR_SOCKET; /* peer closed */
        off += (size_t)r;
    }
    return RCON_ERR_NONE;
}

/* Read one full packet. Returns 0 and fills id/type/payload. */
static int recv_packet(int fd, int32_t *id, int32_t *type, DynBuf *payload)
{
    char lenbuf[4];
    int rc = recv_all(fd, lenbuf, 4);
    if (rc != RCON_ERR_NONE)
        return rc;

    int32_t len = get_i32le((const unsigned char *)lenbuf);
    if (len < 10 || len > 4 * 1024 * 1024)
        return RCON_ERR_PROTO;

    char *body = malloc((size_t)len);
    if (!body)
        return RCON_ERR_SOCKET;
    rc = recv_all(fd, body, (size_t)len);
    if (rc != RCON_ERR_NONE) {
        free(body);
        return rc;
    }

    int i, t;
    rc = rcon_unpack(body, (size_t)len, &i, &t, payload);
    free(body);
    if (rc != RCON_ERR_NONE)
        return rc;
    *id = i;
    *type = t;
    return RCON_ERR_NONE;
}

/* Discard a pending empty trailing packet, if one is immediately readable. */
static void drain_trailing(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 30) <= 0)
        return;
    int32_t id, type;
    DynBuf tmp;
    dynbuf_init(&tmp);
    if (recv_packet(fd, &id, &type, &tmp) == RCON_ERR_NONE)
        LOG_DEBUGF("rcon: discarded %zu trailing byte(s)", tmp.len);
    dynbuf_free(&tmp);
}

/* ---------- connect / auth ---------- */

static int tcp_connect(const char *host, int port, int timeout_ms)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0) {
        LOG_ERRORF("rcon: cannot resolve %s:%d: %s", host, port,
                   gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        LOG_ERRORF("rcon: cannot connect to %s:%d: %s", host, port,
                   strerror(errno));
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static int rcon_auth(RconClient *c)
{
    /* Salt the auth id with the counter so a stale packet can't be mistaken. */
    int id = c->next_id++;

    DynBuf req;
    dynbuf_init(&req);
    rcon_pack(&req, id, RCON_TYPE_AUTH, c->password);
    int rc = send_all(c->fd, req.buf, req.len);
    dynbuf_free(&req);
    if (rc != RCON_ERR_NONE)
        return rc;

    int32_t rid = 0, rtype = 0;
    DynBuf resp;
    dynbuf_init(&resp);
    rc = recv_packet(c->fd, &rid, &rtype, &resp);
    dynbuf_free(&resp);
    if (rc != RCON_ERR_NONE)
        return rc;

    if (rid == -1) {
        LOG_ERRORF("rcon: authentication failed (wrong password)");
        return RCON_ERR_AUTH;
    }
    if (rid != id) {
        LOG_ERRORF("rcon: auth response id %d != requested %d", rid, id);
        return RCON_ERR_PROTO;
    }
    return RCON_ERR_NONE;
}

int rcon_connect(RconClient *c, const char *host, int port,
                 const char *password, int timeout_ms)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->host = xstrdup(host);
    c->port = port;
    c->password = xstrdup(password);
    c->timeout_ms = timeout_ms;
    c->next_id = 1;
    pthread_mutex_init(&c->lock, NULL);

    c->fd = tcp_connect(host, port, timeout_ms);
    if (c->fd < 0)
        return RCON_ERR_SOCKET;

    int rc = rcon_auth(c);
    if (rc != RCON_ERR_NONE) {
        close(c->fd);
        c->fd = -1;
        return rc;
    }
    LOG_INFOF("rcon: connected to %s:%d", host, port);
    return RCON_ERR_NONE;
}

void rcon_disconnect(RconClient *c)
{
    if (c && c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

void rcon_free(RconClient *c)
{
    if (!c)
        return;
    rcon_disconnect(c);
    free(c->host);
    free(c->password);
    c->host = c->password = NULL;
    pthread_mutex_destroy(&c->lock);
}

/* ---------- command ---------- */

static int rcon_command_once(RconClient *c, const char *cmd, DynBuf *out)
{
    int id = c->next_id++;

    DynBuf req;
    dynbuf_init(&req);
    rcon_pack(&req, id, RCON_TYPE_COMMAND, cmd);
    int rc = send_all(c->fd, req.buf, req.len);
    dynbuf_free(&req);
    if (rc != RCON_ERR_NONE)
        return rc;

    dynbuf_reset(out);

    /* Vanilla sends the body in one packet, or splits it into 4096-byte chunks
     * with a final empty packet. Read until a matching packet shorter than the
     * split size arrives. */
    for (int guard = 0; guard < 1024; guard++) {
        int32_t rid = 0, rtype = 0;
        size_t before = out->len;
        rc = recv_packet(c->fd, &rid, &rtype, out);
        if (rc != RCON_ERR_NONE)
            return rc;
        if (rid != id) {
            LOG_DEBUGF("rcon: ignoring packet with id %d (want %d)", rid, id);
            out->len = before;
            if (out->buf)
                out->buf[out->len] = '\0';
            continue;
        }
        if (rtype != RCON_TYPE_RESPONSE) {
            LOG_DEBUGF("rcon: unexpected packet type %d", rtype);
            out->len = before;
            if (out->buf)
                out->buf[out->len] = '\0';
            continue;
        }
        size_t got = out->len - before;
        if (got < 4096) {
            /* Last chunk. A trailing empty packet may still be pending for
             * some servers; don't block the next command on it. */
            drain_trailing(c->fd);
            return RCON_ERR_NONE;
        }
    }
    return RCON_ERR_PROTO;
}

int rcon_command(RconClient *c, const char *cmd, DynBuf *out)
{
    /* One connection, possibly shared by several workers: serialise the whole
     * send/read round trip. */
    pthread_mutex_lock(&c->lock);

    if (c->fd < 0) {
        c->fd = tcp_connect(c->host, c->port, c->timeout_ms);
        if (c->fd < 0) {
            pthread_mutex_unlock(&c->lock);
            return RCON_ERR_SOCKET;
        }
        if (rcon_auth(c) != RCON_ERR_NONE) {
            close(c->fd);
            c->fd = -1;
            pthread_mutex_unlock(&c->lock);
            return RCON_ERR_AUTH;
        }
    }

    int rc = rcon_command_once(c, cmd, out);
    if (rc == RCON_ERR_SOCKET || rc == RCON_ERR_TIMEOUT) {
        LOG_WARNF("rcon: connection lost (%d), reconnecting and retrying once",
                  rc);
        rcon_disconnect(c);
        c->fd = tcp_connect(c->host, c->port, c->timeout_ms);
        if (c->fd < 0) {
            pthread_mutex_unlock(&c->lock);
            return RCON_ERR_SOCKET;
        }
        if (rcon_auth(c) != RCON_ERR_NONE) {
            close(c->fd);
            c->fd = -1;
            pthread_mutex_unlock(&c->lock);
            return RCON_ERR_AUTH;
        }
        rc = rcon_command_once(c, cmd, out);
    }

    pthread_mutex_unlock(&c->lock);
    return rc;
}
