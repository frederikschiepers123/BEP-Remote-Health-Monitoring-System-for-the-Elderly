"""Paho MQTT client with mTLS to the tablet broker (CLAUDE.md §6.1).

Same project CA / cert chain as the firmware (firmware §10). Persistent session
(clean_session=False), QoS-1 subscriptions, exponential reconnect 1→60 s.
"""
from __future__ import annotations

import logging
import socket
import ssl

import paho.mqtt.client as mqtt

from ..config import Settings
from ..domain.sample import SENSORS

log = logging.getLogger(__name__)


class MqttClient:
    def __init__(self, settings: Settings,
                 on_message,            # callable(topic: str, payload: bytes)
                 on_connected=None) -> None:
        self._settings = settings
        self._on_message = on_message
        self._on_connected = on_connected
        self.connected = False

        cid = f"radxa-{socket.gethostname()}"
        self._c = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=cid, protocol=mqtt.MQTTv311, clean_session=False,
        )

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.load_verify_locations(cafile=settings.broker_ca_path)
        ctx.load_cert_chain(certfile=settings.broker_cert_path,
                            keyfile=settings.broker_key_path)
        self._c.tls_set_context(ctx)

        self._c.reconnect_delay_set(min_delay=1, max_delay=60)
        self._c.on_connect = self._handle_connect
        self._c.on_disconnect = self._handle_disconnect
        self._c.on_message = self._handle_message

    # ── lifecycle ──────────────────────────────────────────────────────────
    def start(self) -> None:
        self._c.connect_async(self._settings.broker_host, self._settings.broker_port,
                              keepalive=60)
        self._c.loop_start()

    def stop(self) -> None:
        self._c.loop_stop()
        try:
            self._c.disconnect()
        except Exception:
            pass

    def publish(self, topic: str, payload: bytes | str, qos: int = 1,
                retain: bool = False) -> None:
        self._c.publish(topic, payload, qos=qos, retain=retain)

    # ── callbacks (paho v2) ────────────────────────────────────────────────
    def _handle_connect(self, client, userdata, flags, reason_code, properties):  # noqa: ANN001
        if reason_code != 0:
            log.error("MQTT connect refused: %s", reason_code)
            return
        self.connected = True
        for sensor in SENSORS:
            client.subscribe(f"rmms/+/{sensor}", qos=1)
        client.subscribe("rmms/+/status", qos=1)
        log.info("MQTT connected; subscribed to rmms/+/{%s,status}", ",".join(SENSORS))
        if self._on_connected:
            self._on_connected()

    def _handle_disconnect(self, client, userdata, flags, reason_code, properties):  # noqa: ANN001
        self.connected = False
        log.warning("MQTT disconnected: %s (paho will reconnect)", reason_code)

    def _handle_message(self, client, userdata, msg):  # noqa: ANN001
        try:
            self._on_message(msg.topic, msg.payload)
        except Exception:   # never let one bad message kill the network loop
            log.exception("error handling message on %s", msg.topic)
