# MQTT + mTLS communication — captured proof

Captured 2026-06-21. These logs demonstrate the firmware's MQTT contract
(CLAUDE.md §8.3, §9) and security model (§10) working end to end at the
**protocol level**: mutual-TLS handshake, cert-CN identity, QoS-1 publish/PUBACK,
message delivery to a subscriber, and broker-side ACL enforcement.

## Scope of this capture (read first)

This bundle was produced on the **development host** by running the **real
production broker config** (`out/broker/mosquitto.conf`) with the **real
CA-issued certificates** (`out/`) on the loopback interface, and connecting with
the `mosquitto`/`openssl` clients using the device, mirror and operator certs.

- ✅ Proves: the mTLS trust model, the restricted cipher suite (§10.2),
  `require_certificate`, `use_identity_as_username`, the per-cert ACL, and the
  QoS-1 MQTT round-trip — all with the exact certs and broker policy that ship
  on the tablet.
- ⚠️ Does **not** prove (here): the Pico firmware binary talking to the physical
  tablet over Wi-Fi. That has been demonstrated separately on hardware
  (2026-06-02 `bringup_mqtt.uf2`; 2026-06-19 full PCB e2e on the iPhone hotspot —
  Wi-Fi join, mTLS `CONNACK status=0`, radar vitals publishing). To re-capture
  that live, see "Re-capturing the hardware path" below.

The only differences between `mosquitto-local.conf` and the tablet's
`out/broker/mosquitto.conf` are the cert **paths** (repo `out/` vs Termux home),
the **bind address** (`127.0.0.1` vs `0.0.0.0`), and one extra `log_type
information` line. Auth/TLS/ACL settings are byte-identical.

## Artifacts

| File | What it proves |
|---|---|
| `01_mtls_handshake.log` | Full mutual-TLS handshake as the **device** identity. TLS 1.2, cipher `ECDHE-ECDSA-AES256-GCM-SHA384` (§10.2), `Verify return code: 0 (ok)`, broker chain `tablet.local ← RMMS-Project-CA`, ECDSA peer signature, broker **requested** a client cert (mutual auth). |
| `02_nocert_refused.log` / `02b_nocert_mosquitto.log` | Negative control — a client with **no certificate** is refused (`sslv3 alert handshake failure` / `tlsv13 alert certificate required`). `require_certificate true` enforced. |
| `03_device_publish.log` | Device side of a QoS-1 publish: `CONNECT → CONNACK(0) → PUBLISH(q1) → PUBACK(RC:0)`. |
| `03_mirror_received.log` | Mirror side: the exact §9.2 env JSON delivered over the mirror's own mTLS session. |
| `04_acl_harness.log` | `scripts/test_broker_acl.sh` — **7/7** ACL+mTLS assertions (own-subtree write allowed, cross-device write/read denied, operator info-only, device reads own /cmd, anonymous refused). CLAUDE.md §16-Q9 / bring-up step 12. |
| `broker.stdout.log` | Broker side: each client's MQTT username is its **verified cert CN** (`u'52295a51-…'` = device UUID, `u'mirror-de4ed19a'`, `u'operator-c4c60d23'`) via `use_identity_as_username`; no-cert probes logged as `<unknown> disconnected: Protocol error`. |
| `mosquitto-local.conf` | The localized broker config used for the capture. |

## Reproduce this capture

```bash
# 1. start the broker with the real config + certs on loopback
mosquitto -c docs/report/mqtt_mtls_proof/mosquitto-local.conf -v &

# 2. mTLS handshake as the device
DEV=out/device-52295a51-1a2d-4b2f-bedd-dacbbc1685f0
openssl s_client -connect 127.0.0.1:8883 -CAfile out/broker/ca.crt \
  -cert $DEV/dev.crt.pem -key $DEV/dev.key.pem \
  -verify_ip 127.0.0.1 -verify_return_error \
  -tls1_2 -cipher ECDHE-ECDSA-AES256-GCM-SHA384 </dev/null

# 3. full ACL + mTLS harness
scripts/test_broker_acl.sh 127.0.0.1
```

## Re-capturing the hardware path (Pico → tablet)

When the Pico and tablet are on the same network (phone hotspot — see
project memory; eduroam blocks device↔tablet P2P):

```bash
# device console (TLS handshake → CONNACK → first publish)
minicom -D /dev/ttyACM1 -b 115200          # or: cat /dev/ttyACM1

# on the tablet (or a host with the mirror cert), watch real samples land:
mosquitto_sub --cafile out/broker/ca.crt -h tablet.local -p 8883 \
  --cert out/mirror-*/cert.pem --key out/mirror-*/key.pem \
  -t 'rmms/#' -v
```
