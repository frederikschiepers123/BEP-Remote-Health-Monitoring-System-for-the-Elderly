"""Outbound MQTT — the tablet-clock time-sync publisher (firmware §9.2.5 / §16-Q6).

When a device announces itself (retained `status="online"`), the SBC publishes
the current wall clock to ``rmms/<uuid>/time/set`` so the firmware can stamp real
timestamps (firmware ADR-0003). The SBC is a good clock source: it has the system
clock / network time the firmware lacks.

Broker ACL note: the SBC's cert must be permitted to publish ``rmms/+/time/set``
(in addition to its subscribe grant). Document this in the tablet broker config.
"""
from __future__ import annotations

import json
import logging
import time

log = logging.getLogger(__name__)


def publish_time_set(mqtt_client, device_uuid: str) -> None:
    epoch_ms = int(time.time() * 1000)
    payload = json.dumps({"epoch_ms": epoch_ms})
    # Retained: firmware §9.2.5 expects the per-device time/set to be retained so
    # a device that (re)connects gets the clock immediately, without waiting for
    # the next online event. (The epoch is re-read against the device's monotonic
    # clock at receipt, so a slightly stale retained value is corrected on sync.)
    mqtt_client.publish(f"rmms/{device_uuid}/time/set", payload, qos=1, retain=True)
    log.info("published time/set to %s (epoch_ms=%d)", device_uuid, epoch_ms)
