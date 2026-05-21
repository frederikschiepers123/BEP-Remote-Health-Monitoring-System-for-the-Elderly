#define LOG_TAG "TCP"
#include "stream_tcp.h"

#include "log.h"
#include "err.h"
#include "tls_context.h"

/* lwIP POSIX-socket API (NO_SYS=0, FreeRTOS integration). */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/errno.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── context stored in stream_t.ctx ─────────────────────────────────── */

typedef struct {
    int fd;   /* lwIP socket file descriptor; -1 when closed */
} TcpCtx;

/* Static pool: we only ever need one TCP stream at a time in this design.
 * A second slot allows the dormant transport to hold a closed context.
 * No malloc in steady state (CLAUDE.md §13.4). */
#define TCP_CTX_POOL_SIZE 2u

static TcpCtx g_tcp_pool[TCP_CTX_POOL_SIZE];
static bool   g_pool_used[TCP_CTX_POOL_SIZE];

static TcpCtx *alloc_ctx(void)
{
    for (size_t i = 0u; i < TCP_CTX_POOL_SIZE; i++) {
        if (!g_pool_used[i]) {
            g_pool_used[i] = true;
            g_tcp_pool[i].fd = -1;
            return &g_tcp_pool[i];
        }
    }
    return NULL;
}

static void free_ctx(TcpCtx *ctx)
{
    if (!ctx) return;
    for (size_t i = 0u; i < TCP_CTX_POOL_SIZE; i++) {
        if (&g_tcp_pool[i] == ctx) {
            g_pool_used[i] = false;
            return;
        }
    }
}

/* ── vtable callbacks ────────────────────────────────────────────────── */

/*
 * read() — receive up to len bytes from the TCP socket.
 *
 * Uses SO_RCVTIMEO to implement the timeout_ms deadline.  Returns the number
 * of bytes received (>0), 0 on timeout (EAGAIN/EWOULDBLOCK), or a negative
 * err_t on hard failure or connection close.
 */
static int tcp_read(void *ctx_v, uint8_t *buf, size_t len,
                    uint32_t timeout_ms)
{
    TcpCtx *ctx = (TcpCtx *)ctx_v;
    if (!ctx || ctx->fd < 0 || !buf || len == 0u) {
        return (int)ERR_INVALID_ARG;
    }

    /* Set receive timeout on each call so it can vary per invocation. */
    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    (void)lwip_setsockopt(ctx->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int rc = (int)lwip_recv(ctx->fd, buf, len, 0);
    if (rc > 0) {
        return rc;
    }
    if (rc == 0) {
        /* Peer closed the connection. */
        LOG_W("TCP: peer closed connection");
        return (int)ERR_IO;
    }

    /* rc < 0 */
    int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK) {
        return 0; /* timeout, no data; caller treats as EAGAIN */
    }
    LOG_W("TCP recv error: %d", err);
    return (int)ERR_IO;
}

/*
 * write() — send exactly len bytes over the TCP socket.
 *
 * Returns len on success or a negative err_t on failure.
 */
static int tcp_write(void *ctx_v, const uint8_t *buf, size_t len)
{
    TcpCtx *ctx = (TcpCtx *)ctx_v;
    if (!ctx || ctx->fd < 0 || !buf || len == 0u) {
        return (int)ERR_INVALID_ARG;
    }

    size_t total = 0u;
    while (total < len) {
        int rc = (int)lwip_send(ctx->fd, buf + total, len - total, 0);
        if (rc <= 0) {
            LOG_W("TCP send error: %d", errno);
            return (int)ERR_IO;
        }
        total += (size_t)rc;
    }
    return (int)len;
}

/*
 * close() — shut down and close the TCP socket.
 */
static void tcp_close(void *ctx_v)
{
    TcpCtx *ctx = (TcpCtx *)ctx_v;
    if (!ctx || ctx->fd < 0) return;

    lwip_shutdown(ctx->fd, SHUT_RDWR);
    lwip_close(ctx->fd);
    ctx->fd = -1;
    free_ctx(ctx);
    LOG_I("TCP socket closed");
}

/* ── public API ─────────────────────────────────────────────────────── */

err_t stream_tcp_connect(const char *host, uint16_t port, stream_t *out)
{
    if (!host || !out) {
        return ERR_INVALID_ARG;
    }

    TcpCtx *ctx = alloc_ctx();
    if (!ctx) {
        LOG_E("TCP ctx pool exhausted");
        return ERR_NO_MEM;
    }

    /* DNS / address resolution. */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    /* Manual itoa — no snprintf to avoid pulling in large formatting code
     * in the steady-state path.  port is at most 5 digits. */
    {
        uint16_t p = port;
        int idx = 6;
        port_str[idx--] = '\0';
        do {
            port_str[idx--] = (char)('0' + (p % 10u));
            p = (uint16_t)(p / 10u);
        } while (p > 0u);
        /* Use snprintf for correctness — the manual version above has an
         * off-by-one risk; replace with proper call. */
        /* (void)idx; — suppress warning; overwrite with snprintf result. */
    }
    /* Use lwip's snprintf-equivalent for the port string. */
    int plen = lwip_itoa ? 0 : 0; /* just to reference lwip; use standard: */
    /* Rely on compiler built-in since we can include string.h */
    {
        /* simple uint16→string conversion, no snprintf needed */
        uint16_t p = port;
        int pos = 0;
        char tmp[6];
        if (p == 0u) {
            port_str[pos++] = '0';
        } else {
            while (p > 0u) {
                tmp[pos++] = (char)('0' + (int)(p % 10u));
                p = (uint16_t)(p / 10u);
            }
            /* reverse */
            for (int i = 0, j = pos - 1; i < j; i++, j--) {
                char t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
            }
            memcpy(port_str, tmp, (size_t)pos);
        }
        port_str[pos] = '\0';
        (void)plen;
    }

    struct addrinfo *res = NULL;
    int gai_err = lwip_getaddrinfo(host, port_str, &hints, &res);
    if (gai_err != 0 || !res) {
        LOG_E("getaddrinfo(%s:%u) failed: %d", host, port, gai_err);
        free_ctx(ctx);
        return ERR_NOT_FOUND;
    }

    int fd = lwip_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        LOG_E("socket() failed: %d", errno);
        lwip_freeaddrinfo(res);
        free_ctx(ctx);
        return ERR_IO;
    }

    if (lwip_connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        LOG_E("connect(%s:%u) failed: %d", host, port, errno);
        lwip_close(fd);
        lwip_freeaddrinfo(res);
        free_ctx(ctx);
        return ERR_IO;
    }

    lwip_freeaddrinfo(res);

    ctx->fd = fd;
    out->read  = tcp_read;
    out->write = tcp_write;
    out->close = tcp_close;
    out->ctx   = ctx;

    LOG_I("TCP connected to %s:%u (fd=%d)", host, port, fd);
    return ERR_OK;
}

void stream_tcp_close(stream_t *s)
{
    if (!s || !s->ctx) return;
    tcp_close(s->ctx);
    s->ctx = NULL;
}
