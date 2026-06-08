#ifndef STREAM_TCP_H
#define STREAM_TCP_H

/* lwIP TCP-socket stream_t backend.
 *
 * Provides a blocking stream_t over a standard POSIX-style lwIP socket.
 * Used by the TLS context (tls_context.c) as the byte transport on the
 * Wi-Fi path, exactly as stream_cdc.c serves the USB-CDC path.
 *
 * Lock order: none externally visible.  Each TcpCtx owns one socket fd;
 * concurrent access from multiple tasks to the same stream_t is not
 * permitted and not needed in the current architecture.
 */

#include "err.h"
#include "tls_context.h"   /* for stream_t */
#include <stdint.h>

/* Connect a TCP socket to host:port and fill *out with the stream_t vtable.
 * host may be a dotted-decimal IPv4 address or a hostname (resolved via lwIP
 * DNS if the lwIP DNS module is enabled).
 * Returns ERR_OK on success, ERR_IO on connection failure. */
err_t stream_tcp_connect(const char *host, uint16_t port, stream_t *out);

/* Close the TCP connection represented by *s and release the socket.
 * After this call *s->ctx is NULL and the stream must not be used. */
void stream_tcp_close(stream_t *s);

#endif /* STREAM_TCP_H */
