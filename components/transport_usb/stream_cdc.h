#ifndef STREAM_CDC_H
#define STREAM_CDC_H

/* USB-CDC stream_t backend — TinyUSB CDC interface 0.
 *
 * CDC0 carries the mTLS-encrypted MQTT byte stream (binary, no line
 * discipline).  CDC1 is the log console; writes to CDC1 are done by the log
 * component and never by this module.
 *
 * Lock order: none (no shared mutable state; ring buffers are owned by
 * TinyUSB and accessed only from the transport task).
 */

#include "err.h"
#include "tls_context.h"   /* for stream_t */
#include <stdbool.h>

/* Initialise TinyUSB CDC interface 0 and fill *out with the stream_t vtable.
 * Must be called after tud_init() has been invoked (typically in app_main).
 * Returns ERR_OK on success, ERR_NOT_INIT if TinyUSB is not yet initialised. */
err_t stream_cdc_init(stream_t *out);

/* Returns true if CDC0 is currently enumerated by the host (tud_cdc_connected
 * on interface 0).  Does NOT imply TLS is established — use the transport
 * selector's state for that. */
bool stream_cdc_connected(void);

#endif /* STREAM_CDC_H */
