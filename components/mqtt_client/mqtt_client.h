#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

/* MQTT v3.1.1 client over an abstract stream_t (post-TLS, from tls_context.h).
 *
 * Supports QoS 0 and QoS 1 publish, QoS 0/1 subscribe.
 * LWT is set unconditionally on connect to rmms/<uuid>/status = "offline".
 *
 * Thread safety: MqttClient is NOT thread-safe.  Only the transport_task
 * may call these functions.  All concurrency protection lives at the caller.
 *
 * Lock order: no internal mutex — caller must serialise access.
 */

#include "err.h"
#include "tls_context.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── QoS levels ─────────────────────────────────────────────────────────── */

typedef enum {
    MQTT_QOS_0 = 0,
    MQTT_QOS_1 = 1,
} MqttQos;

/* ── Incoming-message callback ──────────────────────────────────────────── */

/**
 * Called by mqtt_client_poll() for every complete incoming PUBLISH received.
 *
 * @param topic       NUL-terminated topic string.
 * @param payload     Raw payload bytes (not NUL-terminated).
 * @param payload_len Number of payload bytes.
 * @param user        The on_message_user pointer from MqttClient.
 */
typedef void (*mqtt_message_cb_t)(const char    *topic,
                                  const uint8_t *payload,
                                  size_t         payload_len,
                                  void          *user);

/* ── Client configuration & state ───────────────────────────────────────── */

typedef struct {
    /* Configured by caller before mqtt_client_connect(). */
    stream_t         *stream;              /* post-TLS stream from tls_context */
    char              client_id[64];       /* device UUID (from identity) */
    uint16_t          keepalive_s;         /* 30 for USB, 60 for Wi-Fi */
    mqtt_message_cb_t on_message;          /* may be NULL */
    void             *on_message_user;

    /* Internal state — do not access outside mqtt_client.c. */
    uint16_t          _next_packet_id;     /* QoS 1 packet counter */
    uint32_t          _last_activity_ms;   /* wall-clock ms of last send/recv */
    bool              _connected;          /* true after successful CONNACK */
} MqttClient;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Send MQTT CONNECT and wait for CONNACK.
 *
 * Sets up a Last-Will-and-Testament on rmms/<client_id>/status with payload
 * "offline", QoS 1, retained, before the CONNACK is accepted.
 *
 * The stream must already be open and the TLS handshake complete.
 *
 * @return ERR_OK         Connected successfully.
 *         ERR_TIMEOUT    No CONNACK within 5 s.
 *         ERR_MQTT       Broker returned a non-zero return code.
 *         ERR_IO         Underlying stream error.
 */
err_t mqtt_client_connect(MqttClient *c);

/**
 * Publish a message.
 *
 * For QoS 1, blocks until the corresponding PUBACK is received (or timeout).
 *
 * @param topic    NUL-terminated topic string.
 * @param payload  Payload bytes (may be binary).
 * @param len      Number of payload bytes.
 * @param qos      MQTT_QOS_0 or MQTT_QOS_1.
 * @param retain   Whether the broker should retain the message.
 *
 * @return ERR_OK on success, ERR_TIMEOUT, ERR_IO, or ERR_MQTT on failure.
 */
err_t mqtt_client_publish(MqttClient    *c,
                          const char    *topic,
                          const uint8_t *payload,
                          size_t         len,
                          MqttQos        qos,
                          bool           retain);

/**
 * Subscribe to a topic.
 *
 * Blocks until SUBACK is received (or timeout).
 *
 * @return ERR_OK on success, ERR_TIMEOUT or ERR_IO on failure.
 */
err_t mqtt_client_subscribe(MqttClient *c, const char *topic, MqttQos qos);

/**
 * Process one incoming message or keepalive tick.
 *
 * Should be called periodically by the transport task.  Handles:
 *   - Incoming PUBLISH (calls on_message callback).
 *   - PUBACK for pending QoS 1 publishes.
 *   - PINGRESP from a previous PINGREQ.
 *   - Sending PINGREQ when keepalive interval is about to expire.
 *
 * @param timeout_ms  Time to wait for data before returning ERR_TIMEOUT.
 *                    Pass 0 for non-blocking (returns immediately if no data).
 *
 * @return ERR_OK      A packet was processed (or no data in non-blocking mode).
 *         ERR_TIMEOUT No data within timeout_ms (not fatal — caller keeps going).
 *         ERR_IO      Stream error (fatal — caller should reconnect).
 */
err_t mqtt_client_poll(MqttClient *c, uint32_t timeout_ms);

/**
 * Send MQTT DISCONNECT and close the underlying stream.
 *
 * Best-effort: if the stream is already broken the function returns silently.
 * After this call c->_connected is false and the stream pointer is invalid.
 */
void mqtt_client_disconnect(MqttClient *c);

#endif /* MQTT_CLIENT_H */
