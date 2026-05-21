#define LOG_TAG "MQTT"
#include "log.h"

#include "mqtt_client.h"
#include "err.h"

#include "pico/time.h"      /* to_ms_since_boot(), get_absolute_time() */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Internal buffer sizes ──────────────────────────────────────────────── */

#define MQTT_SEND_BUF_SIZE  1024
#define MQTT_RECV_BUF_SIZE  1024

/* Timeouts */
#define CONNACK_TIMEOUT_MS   5000U
#define PUBACK_TIMEOUT_MS    5000U
#define SUBACK_TIMEOUT_MS    5000U
#define PINGRESP_TIMEOUT_MS  5000U

/* ── MQTT packet type nibbles (upper 4 bits of fixed header byte 0) ──────── */
#define PKT_CONNECT     0x10U
#define PKT_CONNACK     0x20U
#define PKT_PUBLISH     0x30U
#define PKT_PUBACK      0x40U
#define PKT_SUBSCRIBE   0x82U
#define PKT_SUBACK      0x90U
#define PKT_PINGREQ     0xC0U
#define PKT_PINGRESP    0xD0U
#define PKT_DISCONNECT  0xE0U

/* ── Static send/receive buffers ─────────────────────────────────────────── */

static uint8_t s_send_buf[MQTT_SEND_BUF_SIZE];
static uint8_t s_recv_buf[MQTT_RECV_BUF_SIZE];

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Encode MQTT remaining-length (variable-length integer) into buf.
 * Returns number of bytes written (1–4). */
static int encode_remaining_length(uint8_t *buf, size_t remaining)
{
    int i = 0;
    do {
        uint8_t byte = (uint8_t)(remaining & 0x7FU);
        remaining >>= 7;
        if (remaining > 0) {
            byte |= 0x80U;
        }
        buf[i++] = byte;
    } while (remaining > 0 && i < 4);
    return i;
}

/* Write a big-endian uint16 into buf. */
static void write_u16_be(uint8_t *buf, uint16_t val)
{
    buf[0] = (uint8_t)((val >> 8) & 0xFFU);
    buf[1] = (uint8_t)(val & 0xFFU);
}

/* Read big-endian uint16 from buf. */
static uint16_t read_u16_be(const uint8_t *buf)
{
    return (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
}

/* Append a MQTT UTF-8 string (2-byte length prefix + bytes) to buf at *pos.
 * Returns ERR_OVERFLOW if it would exceed buf_size. */
static err_t append_mqtt_string(uint8_t *buf, size_t buf_size, size_t *pos,
                                const char *str)
{
    size_t slen = strlen(str);
    if (*pos + 2 + slen > buf_size) {
        return ERR_OVERFLOW;
    }
    write_u16_be(&buf[*pos], (uint16_t)slen);
    *pos += 2;
    memcpy(&buf[*pos], str, slen);
    *pos += slen;
    return ERR_OK;
}

/* Append raw bytes to buf at *pos. */
static err_t append_bytes(uint8_t *buf, size_t buf_size, size_t *pos,
                          const uint8_t *data, size_t len)
{
    if (*pos + len > buf_size) {
        return ERR_OVERFLOW;
    }
    memcpy(&buf[*pos], data, len);
    *pos += len;
    return ERR_OK;
}

/* Write all bytes through stream; returns ERR_OK or ERR_IO. */
static err_t stream_write_all(stream_t *s, const uint8_t *buf, size_t len)
{
    int ret = s->write(s->ctx, buf, len);
    if (ret < 0 || (size_t)ret != len) {
        return ERR_IO;
    }
    return ERR_OK;
}

/* Read exactly `len` bytes from stream with timeout.
 * Returns ERR_OK, ERR_TIMEOUT, or ERR_IO. */
static err_t stream_read_exact(stream_t *s, uint8_t *buf, size_t len,
                               uint32_t timeout_ms)
{
    size_t received = 0;
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + timeout_ms;

    while (received < len) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (timeout_ms > 0 && now >= deadline) {
            return ERR_TIMEOUT;
        }
        uint32_t remaining_ms = (timeout_ms == 0) ? 0 : (deadline - now);
        int r = s->read(s->ctx, buf + received, len - received, remaining_ms);
        if (r < 0) {
            return ERR_IO;
        }
        if (r == 0) {
            if (timeout_ms == 0) {
                return ERR_TIMEOUT;
            }
            continue;
        }
        received += (size_t)r;
    }
    return ERR_OK;
}

/* Read fixed header + remaining length from stream.
 * Returns ERR_OK; sets *pkt_type and *remaining_len. */
static err_t recv_fixed_header(stream_t *s, uint8_t *pkt_type,
                               size_t *remaining_len, uint32_t timeout_ms)
{
    uint8_t byte0;
    err_t err = stream_read_exact(s, &byte0, 1, timeout_ms);
    if (err != ERR_OK) {
        return err;
    }
    *pkt_type = byte0;

    size_t rl = 0;
    uint32_t multiplier = 1;
    for (int i = 0; i < 4; i++) {
        uint8_t enc_byte;
        err = stream_read_exact(s, &enc_byte, 1, timeout_ms);
        if (err != ERR_OK) {
            return err;
        }
        rl += (size_t)((enc_byte & 0x7FU) * multiplier);
        multiplier *= 128U;
        if ((enc_byte & 0x80U) == 0) {
            break;
        }
    }
    *remaining_len = rl;
    return ERR_OK;
}

/* Receive a complete variable-length packet into s_recv_buf.
 * Returns ERR_OK on success, ERR_OVERFLOW if packet exceeds buffer. */
static err_t recv_packet(stream_t *s, uint8_t *pkt_type, size_t *payload_len,
                         uint32_t timeout_ms)
{
    err_t err = recv_fixed_header(s, pkt_type, payload_len, timeout_ms);
    if (err != ERR_OK) {
        return err;
    }
    if (*payload_len > MQTT_RECV_BUF_SIZE) {
        LOG_E("Incoming MQTT packet too large: %u bytes", (unsigned)*payload_len);
        return ERR_OVERFLOW;
    }
    if (*payload_len > 0) {
        err = stream_read_exact(s, s_recv_buf, *payload_len, timeout_ms);
        if (err != ERR_OK) {
            return err;
        }
    }
    return ERR_OK;
}

/* Allocate the next QoS 1 packet ID (never returns 0). */
static uint16_t next_packet_id(MqttClient *c)
{
    c->_next_packet_id++;
    if (c->_next_packet_id == 0) {
        c->_next_packet_id = 1;
    }
    return c->_next_packet_id;
}

/* ── mqtt_client_connect ─────────────────────────────────────────────────── */

err_t mqtt_client_connect(MqttClient *c)
{
    if (!c || !c->stream) {
        return ERR_INVALID_ARG;
    }

    /* Build will topic: rmms/<client_id>/status */
    char will_topic[128];
    int n = snprintf(will_topic, sizeof(will_topic), "rmms/%s/status",
                     c->client_id);
    if (n < 0 || (size_t)n >= sizeof(will_topic)) {
        return ERR_INVALID_ARG;
    }

    const char *will_payload = "offline";
    const size_t will_payload_len = 7U;

    /*
     * Build CONNECT payload:
     *   client_id  (UTF-8 string)
     *   will_topic (UTF-8 string)
     *   will_msg   (UTF-8 string — treated as binary for payload)
     */

    /* Variable header: protocol name + level + flags + keepalive = 10 bytes */
    /* Payload: client_id + will_topic + will_payload */

    /* Compute payload length */
    size_t payload_len = 2 + strlen(c->client_id)     /* client_id */
                       + 2 + strlen(will_topic)         /* will_topic */
                       + 2 + will_payload_len;          /* will_message */

    /* Variable header (10 bytes) + payload */
    size_t remaining = 10 + payload_len;

    /* Build packet into s_send_buf */
    size_t pos = 0;

    /* Fixed header byte 0 */
    if (pos >= MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    s_send_buf[pos++] = PKT_CONNECT;

    /* Remaining length */
    uint8_t rl_buf[4];
    int rl_bytes = encode_remaining_length(rl_buf, remaining);
    if (pos + (size_t)rl_bytes > MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    memcpy(&s_send_buf[pos], rl_buf, (size_t)rl_bytes);
    pos += (size_t)rl_bytes;

    /* Protocol name "MQTT" */
    err_t err = append_mqtt_string(s_send_buf, MQTT_SEND_BUF_SIZE, &pos, "MQTT");
    if (err != ERR_OK) { return err; }

    /* Protocol level: 4 (MQTT 3.1.1) */
    if (pos >= MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    s_send_buf[pos++] = 0x04U;

    /*
     * Connect flags:
     *   Bit 7: UserNameFlag = 0
     *   Bit 6: PasswordFlag = 0
     *   Bit 5: WillRetain   = 1
     *   Bit 4-3: WillQoS    = 01 (QoS 1)
     *   Bit 2: WillFlag     = 1
     *   Bit 1: CleanSession = 1
     *   Bit 0: reserved     = 0
     */
    if (pos >= MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    s_send_buf[pos++] = 0x2EU; /* 0b00101110 */

    /* Keepalive (big-endian) */
    if (pos + 2 > MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    write_u16_be(&s_send_buf[pos], c->keepalive_s);
    pos += 2;

    /* Payload: client_id */
    err = append_mqtt_string(s_send_buf, MQTT_SEND_BUF_SIZE, &pos, c->client_id);
    if (err != ERR_OK) { return err; }

    /* Payload: will_topic */
    err = append_mqtt_string(s_send_buf, MQTT_SEND_BUF_SIZE, &pos, will_topic);
    if (err != ERR_OK) { return err; }

    /* Payload: will_message (2-byte length + bytes) */
    if (pos + 2 + will_payload_len > MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    write_u16_be(&s_send_buf[pos], (uint16_t)will_payload_len);
    pos += 2;
    memcpy(&s_send_buf[pos], will_payload, will_payload_len);
    pos += will_payload_len;

    /* Send CONNECT */
    err = stream_write_all(c->stream, s_send_buf, pos);
    if (err != ERR_OK) {
        LOG_E("Failed to send CONNECT");
        return err;
    }

    /* Receive CONNACK */
    uint8_t pkt_type;
    size_t pkt_len;
    err = recv_packet(c->stream, &pkt_type, &pkt_len, CONNACK_TIMEOUT_MS);
    if (err != ERR_OK) {
        LOG_E("CONNACK recv failed: %d", err);
        return err;
    }
    if ((pkt_type & 0xF0U) != PKT_CONNACK) {
        LOG_E("Expected CONNACK, got 0x%02X", pkt_type);
        return ERR_MQTT;
    }
    if (pkt_len < 2) {
        LOG_E("CONNACK too short");
        return ERR_MQTT;
    }
    if (s_recv_buf[1] != 0x00U) {
        LOG_E("CONNACK return code: %d", s_recv_buf[1]);
        return ERR_MQTT;
    }

    c->_connected = true;
    c->_last_activity_ms = to_ms_since_boot(get_absolute_time());
    c->_next_packet_id = 0;
    LOG_I("MQTT connected, client_id=%s", c->client_id);
    return ERR_OK;
}

/* ── mqtt_client_publish ─────────────────────────────────────────────────── */

err_t mqtt_client_publish(MqttClient    *c,
                          const char    *topic,
                          const uint8_t *payload,
                          size_t         len,
                          MqttQos        qos,
                          bool           retain)
{
    if (!c || !c->_connected || !topic) {
        return ERR_INVALID_ARG;
    }

    size_t topic_len = strlen(topic);

    /*
     * Fixed header byte 0:
     *   Bits 7-4: type = 0x3 (PUBLISH)
     *   Bit  3:   DUP  = 0
     *   Bits 2-1: QoS
     *   Bit  0:   RETAIN
     */
    uint8_t hdr0 = (uint8_t)(0x30U | ((uint8_t)qos << 1) | (retain ? 0x01U : 0x00U));

    /* Remaining length = 2 + topic_len [+ 2 for packet_id if QoS1] + payload */
    size_t var_hdr_len = 2 + topic_len + ((qos == MQTT_QOS_1) ? 2U : 0U);
    size_t remaining   = var_hdr_len + len;

    size_t pos = 0;
    if (pos >= MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    s_send_buf[pos++] = hdr0;

    uint8_t rl_buf[4];
    int rl_bytes = encode_remaining_length(rl_buf, remaining);
    if (pos + (size_t)rl_bytes > MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    memcpy(&s_send_buf[pos], rl_buf, (size_t)rl_bytes);
    pos += (size_t)rl_bytes;

    /* Topic string */
    err_t err = append_mqtt_string(s_send_buf, MQTT_SEND_BUF_SIZE, &pos, topic);
    if (err != ERR_OK) { return err; }

    uint16_t pkt_id = 0;
    if (qos == MQTT_QOS_1) {
        pkt_id = next_packet_id(c);
        if (pos + 2 > MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
        write_u16_be(&s_send_buf[pos], pkt_id);
        pos += 2;
    }

    /* Payload */
    err = append_bytes(s_send_buf, MQTT_SEND_BUF_SIZE, &pos, payload, len);
    if (err != ERR_OK) { return err; }

    err = stream_write_all(c->stream, s_send_buf, pos);
    if (err != ERR_OK) {
        LOG_E("PUBLISH send failed");
        return err;
    }
    c->_last_activity_ms = to_ms_since_boot(get_absolute_time());

    /* Wait for PUBACK on QoS 1 */
    if (qos == MQTT_QOS_1) {
        uint8_t pkt_type;
        size_t pkt_len;
        err = recv_packet(c->stream, &pkt_type, &pkt_len, PUBACK_TIMEOUT_MS);
        if (err != ERR_OK) {
            LOG_E("PUBACK recv failed: %d", err);
            return err;
        }
        if ((pkt_type & 0xF0U) != PKT_PUBACK) {
            LOG_E("Expected PUBACK, got 0x%02X", pkt_type);
            return ERR_MQTT;
        }
        if (pkt_len >= 2) {
            uint16_t ack_id = read_u16_be(s_recv_buf);
            if (ack_id != pkt_id) {
                LOG_W("PUBACK packet_id mismatch: got %u expected %u",
                      ack_id, pkt_id);
            }
        }
        c->_last_activity_ms = to_ms_since_boot(get_absolute_time());
    }

    return ERR_OK;
}

/* ── mqtt_client_subscribe ───────────────────────────────────────────────── */

err_t mqtt_client_subscribe(MqttClient *c, const char *topic, MqttQos qos)
{
    if (!c || !c->_connected || !topic) {
        return ERR_INVALID_ARG;
    }

    uint16_t pkt_id = next_packet_id(c);
    size_t topic_len = strlen(topic);

    /*
     * SUBSCRIBE fixed header: type=0x8, flags=0x2 → 0x82
     * Remaining: 2 (pkt_id) + 2 (topic_len) + topic_len + 1 (QoS byte)
     */
    size_t remaining = 2 + 2 + topic_len + 1;

    size_t pos = 0;
    if (pos >= MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    s_send_buf[pos++] = PKT_SUBSCRIBE;

    uint8_t rl_buf[4];
    int rl_bytes = encode_remaining_length(rl_buf, remaining);
    if (pos + (size_t)rl_bytes > MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    memcpy(&s_send_buf[pos], rl_buf, (size_t)rl_bytes);
    pos += (size_t)rl_bytes;

    if (pos + 2 > MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    write_u16_be(&s_send_buf[pos], pkt_id);
    pos += 2;

    err_t err = append_mqtt_string(s_send_buf, MQTT_SEND_BUF_SIZE, &pos, topic);
    if (err != ERR_OK) { return err; }

    if (pos >= MQTT_SEND_BUF_SIZE) { return ERR_OVERFLOW; }
    s_send_buf[pos++] = (uint8_t)qos;

    err = stream_write_all(c->stream, s_send_buf, pos);
    if (err != ERR_OK) {
        LOG_E("SUBSCRIBE send failed");
        return err;
    }
    c->_last_activity_ms = to_ms_since_boot(get_absolute_time());

    /* Wait for SUBACK */
    uint8_t pkt_type;
    size_t pkt_len;
    err = recv_packet(c->stream, &pkt_type, &pkt_len, SUBACK_TIMEOUT_MS);
    if (err != ERR_OK) {
        LOG_E("SUBACK recv failed: %d", err);
        return err;
    }
    if ((pkt_type & 0xF0U) != PKT_SUBACK) {
        LOG_E("Expected SUBACK, got 0x%02X", pkt_type);
        return ERR_MQTT;
    }
    /* Return code in bytes 2+ (first 2 bytes are packet ID); 0x80 = failure */
    if (pkt_len >= 3 && s_recv_buf[2] == 0x80U) {
        LOG_E("Broker rejected subscription to %s", topic);
        return ERR_MQTT;
    }
    c->_last_activity_ms = to_ms_since_boot(get_absolute_time());
    LOG_I("Subscribed to %s QoS%d", topic, (int)qos);
    return ERR_OK;
}

/* ── mqtt_client_poll ────────────────────────────────────────────────────── */

err_t mqtt_client_poll(MqttClient *c, uint32_t timeout_ms)
{
    if (!c || !c->_connected) {
        return ERR_INVALID_ARG;
    }

    /* Check if we need to send a PINGREQ */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t keepalive_ms = (uint32_t)c->keepalive_s * 1000U;
    if (keepalive_ms > 0 && (now - c->_last_activity_ms) >= (keepalive_ms * 3U / 4U)) {
        /* Send PINGREQ */
        uint8_t pingreq[2] = { PKT_PINGREQ, 0x00U };
        err_t err = stream_write_all(c->stream, pingreq, 2);
        if (err != ERR_OK) {
            LOG_E("PINGREQ send failed");
            return err;
        }
        c->_last_activity_ms = to_ms_since_boot(get_absolute_time());
        LOG_D("PINGREQ sent");
    }

    /* Try to receive one packet */
    uint8_t pkt_type;
    size_t pkt_len;
    err_t err = recv_packet(c->stream, &pkt_type, &pkt_len, timeout_ms);
    if (err == ERR_TIMEOUT) {
        return ERR_TIMEOUT;
    }
    if (err != ERR_OK) {
        LOG_E("poll recv_packet error: %d", err);
        return err;
    }
    c->_last_activity_ms = to_ms_since_boot(get_absolute_time());

    uint8_t type_nibble = pkt_type & 0xF0U;

    if (type_nibble == PKT_PUBLISH) {
        /*
         * Incoming PUBLISH.
         * Flags bits: DUP(3) QoS(2:1) RETAIN(0)
         */
        uint8_t qos_flags = (pkt_type >> 1) & 0x03U;

        if (pkt_len < 2) {
            LOG_W("PUBLISH too short");
            return ERR_OK;
        }

        /* Topic length */
        uint16_t topic_len = read_u16_be(&s_recv_buf[0]);
        if ((size_t)(2 + topic_len) > pkt_len) {
            LOG_W("PUBLISH topic_len out of range");
            return ERR_OK;
        }

        /* NUL-terminate topic in-place (overwrite first payload byte) */
        char topic_buf[256];
        size_t copy_len = (topic_len < sizeof(topic_buf) - 1) ?
                          topic_len : (sizeof(topic_buf) - 1);
        memcpy(topic_buf, &s_recv_buf[2], copy_len);
        topic_buf[copy_len] = '\0';

        size_t body_offset = 2 + topic_len;
        uint16_t pkt_id = 0;
        if (qos_flags == 1 || qos_flags == 2) {
            if (body_offset + 2 > pkt_len) {
                LOG_W("PUBLISH packet_id out of range");
                return ERR_OK;
            }
            pkt_id = read_u16_be(&s_recv_buf[body_offset]);
            body_offset += 2;
        }

        const uint8_t *payload     = &s_recv_buf[body_offset];
        size_t         payload_len = (body_offset <= pkt_len) ?
                                     (pkt_len - body_offset) : 0;

        /* Invoke callback */
        if (c->on_message) {
            c->on_message(topic_buf, payload, payload_len, c->on_message_user);
        }

        /* Send PUBACK for QoS 1 */
        if (qos_flags == 1) {
            uint8_t puback[4] = {
                PKT_PUBACK, 0x02U,
                (uint8_t)(pkt_id >> 8), (uint8_t)(pkt_id & 0xFFU)
            };
            err_t perr = stream_write_all(c->stream, puback, sizeof(puback));
            if (perr != ERR_OK) {
                LOG_E("PUBACK send failed");
                return perr;
            }
        }

    } else if (type_nibble == PKT_PINGRESP) {
        LOG_D("PINGRESP received");

    } else if (type_nibble == PKT_PUBACK) {
        /* Unsolicited PUBACK (can occur if a previous publish was racing) */
        LOG_D("Unsolicited PUBACK received");

    } else {
        LOG_W("Unexpected packet type 0x%02X in poll", pkt_type);
    }

    return ERR_OK;
}

/* ── mqtt_client_disconnect ──────────────────────────────────────────────── */

void mqtt_client_disconnect(MqttClient *c)
{
    if (!c || !c->_connected) {
        return;
    }

    uint8_t disconnect[2] = { PKT_DISCONNECT, 0x00U };
    /* Best-effort: ignore errors */
    (void)stream_write_all(c->stream, disconnect, sizeof(disconnect));

    if (c->stream && c->stream->close) {
        c->stream->close(c->stream->ctx);
    }

    c->_connected = false;
    LOG_I("MQTT disconnected");
}
