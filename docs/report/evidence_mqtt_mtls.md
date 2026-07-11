# Evidence Appendix — MQTT Message Protocol & mTLS

This appendix collects reproducible exhibits backing the communication-and-security
claims. Every exhibit is either a committed source/config file or the verbatim
output of a command you can re-run. File paths are clickable in the repo.

---

## A. Transport & message protocol (what the firmware speaks)

- **MQTT v3.1.1** client (lwIP `pico_lwip_mqtt`) running **over `altcp_tls`
  (mbedTLS)** — there is no plaintext MQTT path off the device. Single owner:
  `components/transport_mqtt/transport_mqtt.c`.
- **Per-device topic tree** `rmms/<uuid>/...`; the device UUID is also the MQTT
  `client_id`:
  - `transport_mqtt.c:508` — `s_ci.client_id = s_id->uuid;`
- **Publish QoS 1** for every sensor sample (retry-until-PUBACK from the spool):
  - `transport_mqtt.c:693` — `mqtt_publish(..., /*qos=*/1, /*retain=*/0, ...)`
- **Retained status + Last-Will-and-Testament**, both QoS 1:
  - `transport_mqtt.c:395` — `"online"` published `qos=1, retain=1` on CONNACK
  - `transport_mqtt.c:511-514` — LWT `rmms/<uuid>/status = "offline"`, `qos=1, retain=1`
- **Subscribes only to its own downlinks** — `rmms/<uuid>/cmd` and
  `rmms/<uuid>/time/set` (`transport_mqtt.c:400`). It never subscribes to other
  devices' trees.
- **Payload = compact JSON** per the §9.2 envelope (`ts_us`, `wall_ms`, `seq`,
  `q`, `v{...}`); schema in [docs/mqtt_topics.md](../mqtt_topics.md).

> Claim you can defend: *"The device speaks MQTT 3.1.1 exclusively over mutual
> TLS, publishes each vitals sample at QoS 1 into its own `rmms/<uuid>/...`
> subtree, advertises liveness with a retained online/offline LWT, and
> subscribes only to its own command/time topics."*

---

## B. Mutual TLS (both directions, static factory certs)

### B.1 The firmware does *two-way* auth, not server-only TLS
`components/transport_mqtt/transport_mqtt.c:482`:

```c
s_tls_cfg = altcp_tls_create_config_client_2wayauth(
    /* CA */ ca, ca_len,        /* validate the broker against the project CA */
    /* key */ key, key_len,     /* present the device private key ... */
    /* cert */ crt, crt_len);   /* ... and the device cert (client auth) */
```

`..._2wayauth` = the device **presents its own cert** *and* **validates the
broker's cert against the same project CA**. Mutual authentication, single trust
anchor.

### B.2 TLS profile is locked to ECDHE-ECDSA P-256 / AES-GCM
[mbedtls_config.h](../../mbedtls_config.h) — `MBEDTLS_SSL_PROTO_TLS1_2` only;
ECDHE-ECDSA key exchange; P-256 only; AES-128/256-GCM; SHA-256/384.
RSA, DHE, CBC, RC4, DES, SHA-1 are all `#undef`-ed.

> **Calibration — important for the defense:** the **device negotiates TLS 1.2**,
> not 1.3. pico-sdk 2.x does not compile the mbedTLS 1.3 handshake sources, so
> 1.3 is deliberately disabled (comment at `mbedtls_config.h:21-27`). Any
> "TLS 1.3" seen in broker logs is a **desktop** client (mirror/operator/
> `mosquitto_pub`, which default to 1.3), **not the Pico**. State the device path
> as **"TLS 1.2, ECDHE-ECDSA-AES128/256-GCM-SHA256/384, mutual auth, P-256."**

### B.3 The certificates are real ECDSA P-256, CN = UUID, signed by the project CA
Verbatim `openssl` output (re-run: `openssl x509 -in <cert> -noout -text`):

```
### CA cert
subject=CN = RMMS-Project-CA
issuer=CN = RMMS-Project-CA           <- self-signed root
ASN1 OID: prime256v1 / NIST CURVE: P-256
Signature Algorithm: ecdsa-with-SHA256
CA:TRUE, pathlen:0

### Device cert
subject=CN = 7b3728e2-e682-4d08-90ca-48757d9ecc5a   <- CN *is* the device UUID
issuer=CN = RMMS-Project-CA                          <- signed by project CA
ASN1 OID: prime256v1 / NIST CURVE: P-256
Signature Algorithm: ecdsa-with-SHA256
X509v3 Extended Key Usage: TLS Web Client Authentication
SAN: URI:urn:rmms:device:7b3728e2-e682-4d08-90ca-48757d9ecc5a

### Chain verification
out/device-7b3728e2-.../dev.crt.pem: OK              <- chains to the CA
```

Provisioning script: [scripts/provision_ca.sh](../../scripts/provision_ca.sh)
(generates CA + device/mirror/operator certs, all P-256, off-device).

---

## C. Broker maps cert CN → identity → per-device authorization

Broker config [out/broker/mosquitto.conf](../../out/broker/mosquitto.conf) —
the only network-facing listener:

```
listener 8883 0.0.0.0
allow_anonymous false
require_certificate true          # no cert -> no connection
use_identity_as_username true     # MQTT username := cert CN
acl_file .../acl
```

ACL [out/broker/acl](../../out/broker/acl) — `%u` is the CN-derived username:

```
pattern write rmms/%u/#           # a device may publish ONLY under its own UUID
pattern read  rmms/%u/cmd         # ... and read ONLY its own /cmd
user mirror-de4ed19a
  topic read  rmms/#              # mirror: read-only across the whole tree
user operator-c4c60d23
  topic write rmms/+/info         # operator: write ONLY info/screen
  topic write rmms/+/screen
```

> Claim you can defend: *"The broker requires a client certificate, derives the
> MQTT username from the certificate CN, and a pattern ACL confines each device
> to publishing within its own `rmms/<uuid>/#` subtree and reading only its own
> `/cmd`."*

---

## D. ACL-enforcement test harness (positive + partial negative)

[scripts/test_broker_acl.sh](../../scripts/test_broker_acl.sh) makes **7
assertions** against the live broker. Because Mosquitto silently drops
ACL-denied publishes, it asserts via a privileged mirror observer (it checks
whether a publish actually *lands*, not the subscribe ACK):

| # | Assertion | Type |
|---|-----------|------|
| A | device → own subtree **allowed** | positive |
| B | device → *other* device subtree **denied** | **negative (cross-device write isolation)** |
| C | operator → `/info` **allowed** | positive |
| D | operator → `/env` **denied** (operator is info/screen-only) | **negative (role confinement)** |
| E | device reads its own `/cmd` **allowed** | positive |
| F | device → read *other* device's topics **denied** | **negative (cross-device read isolation)** |
| G | anonymous / **no client cert** connection **refused** | **negative (require_certificate)** |

> **Calibration vs. the current report draft:** the harness already covers
> cert-less refusal (G) and cross-device read+write isolation (B, F). So you can
> claim *more* negative testing than "not completed." The one negative case
> genuinely **missing** is **rogue-CA rejection** (a cert signed by a *different*
> CA) — see §F to close it.

---

## E. Live captures to attach (run against the running tablet broker)

These need the tablet broker up (`scripts/demo_start.sh`). Each produces a
paste-ready artifact for the report.

1. **ACL harness result (7/7):**
   ```
   scripts/test_broker_acl.sh <tablet-ip>
   ```
   Attach the `=== N passed, 0 failed ===` block.

2. **Broker log showing mTLS + CN-as-username + per-client connect** (Termux):
   ```
   mosquitto -c .../mosquitto.conf -v   # already verbose in demo_start
   ```
   Capture the lines: `New client connected ... as <uuid>` and the TLS/cipher
   notice. This is the "confirmed from broker logs" evidence.

3. **Independent handshake proof (cipher + peer chain), device profile:**
   ```
   openssl s_client -connect <tablet-ip>:8883 -tls1_2 \
       -CAfile ~/rmms-ca/ca.crt \
       -cert out/device-<uuid>/dev.crt.pem -key out/device-<uuid>/dev.key.pem 2>/dev/null \
       | grep -E 'Protocol|Cipher|subject=|issuer='
   ```
   Expect `Protocol: TLSv1.2`, an `ECDHE-ECDSA-AES*-GCM-SHA*` cipher, and the
   broker cert chaining to `RMMS-Project-CA`.

4. **Live message capture (the actual protocol on the wire):**
   ```
   mosquitto_sub --cafile ~/rmms-ca/ca.crt -h <tablet-ip> -p 8883 \
       --cert out/mirror-de4ed19a/cert.pem --key out/mirror-de4ed19a/key.pem \
       -t 'rmms/#' -v
   ```
   Attach a few `rmms/<uuid>/env|radar|... {json}` lines + the retained
   `rmms/<uuid>/status online`.

---

## F. Optional: close the rogue-CA gap in ~5 minutes

This makes the negative-testing claim complete. Generates a throwaway CA + cert
and proves the broker rejects it:

```bash
TMP=$(mktemp -d)
openssl ecparam -name prime256v1 -genkey -noout -out $TMP/rogue-ca.key
openssl req -x509 -new -key $TMP/rogue-ca.key -days 1 -subj /CN=ROGUE-CA -out $TMP/rogue-ca.crt
openssl ecparam -name prime256v1 -genkey -noout -out $TMP/rogue.key
openssl req -new -key $TMP/rogue.key -subj /CN=11111111-1111-1111-1111-111111111111 -out $TMP/rogue.csr
openssl x509 -req -in $TMP/rogue.csr -CA $TMP/rogue-ca.crt -CAkey $TMP/rogue-ca.key \
    -days 1 -out $TMP/rogue.crt
# Present the rogue cert to the real broker — expect connection REFUSED:
mosquitto_pub --cafile ~/rmms-ca/ca.crt -h <tablet-ip> -p 8883 --insecure \
    --cert $TMP/rogue.crt --key $TMP/rogue.key \
    -t rmms/11111111-1111-1111-1111-111111111111/x -m nope \
  && echo "BREACH: rogue cert accepted" || echo "PASS: broker rejected rogue-CA cert"
```
