# HealthMonitorWakeTest — tablet screen wake/lock app

Android app for the RMMS tablet. It couples radar presence to the tablet screen:
the MagicMirror² display behind the two-way acrylic lights up when someone is in
front of the mirror and locks (blanks) when nobody is there.

The app itself is deliberately dumb: it subscribes to one local MQTT topic and
turns the screen on or off. All presence *policy* (debounce, ghost filtering,
stale-radar handling) lives in `scripts/tablet_presence_screen.py`, which runs
on the same tablet in Termux. The authoritative design doc for the whole
coupling is **`docs/presence_screen_coupling.md`** — read that first.

## Role in the system

```
firmware radar ──rmms/<uuid>/radar (mTLS :8883)──► tablet_presence_screen.py
                                                        │ debounce / state machine
                                                        ▼
                                        `display` topic, plain MQTT 127.0.0.1:1883
                                                        │
                                                        ▼
                                              this app (MQTTService)
                                     "ON"  → WakeService full wakelock (screen on)
                                     "OFF" → DevicePolicyManager.lockNow() (screen off)
```

The firmware never talks to this app directly; it only publishes raw topics on
the mTLS listener. The bridge script is the adapter between the two worlds.

## MQTT contract (what the code actually does)

- Broker: `tcp://127.0.0.1:1883` — the **localhost-only plain listener** of the
  tablet's Mosquitto. This is deliberate on-device IPC, not a network-facing
  plaintext broker; see `docs/adr/0004-localhost-plain-listener-app-ipc.md` for
  why this does not violate the "mTLS-only" rule (the `:1883` listener is bound
  to `127.0.0.1` and carries no firmware traffic).
- Client: Eclipse Paho `org.eclipse.paho.client.mqttv3` 1.2.5, random client
  id, clean session, automatic reconnect, plus its own retry-every-5 s loop
  until the first connect succeeds.
- Subscription: topic **`display`** (no wildcard, no `rmms/` prefix).
- Payloads: plain strings, trimmed and uppercased before matching:
  - `ON` — starts `WakeService`, which acquires a
    `FULL_WAKE_LOCK | ACQUIRE_CAUSES_WAKEUP` wakelock for 5 s to turn the
    screen on.
  - `OFF` — calls `DevicePolicyManager.lockNow()` via the
    `MyDeviceAdminReceiver` device admin (policy `<force-lock/>`). If Device
    Admin is not active it logs an error and does nothing.
  - Anything else is logged and ignored.
- The app publishes nothing to the broker. It broadcasts status strings on an
  in-app `MQTT_MESSAGE` intent, shown in `MainActivity`'s status text.

`MainActivity` is a minimal test UI: manual SCREEN ON / SCREEN OFF buttons and
the last-MQTT-message text. Opening it also starts `MQTTService`.

## Build

The Android Studio project is **nested one level down**: open
`HealthMonitorWakeTest/HealthMonitorWakeTest/` (the folder containing
`settings.gradle.kts`), not this folder.

Command line:

```bash
cd HealthMonitorWakeTest/HealthMonitorWakeTest
./gradlew assembleDebug
# APK: app/build/outputs/apk/debug/app-debug.apk
```

Targets SDK 36, minSdk 26. A prebuilt APK is checked in at the repo root as
`apk/appScreenControl.apk`.

## Install + first run

1. Install the APK (`adb install` or sideload).
2. Open the app once so `MQTTService` starts.
3. **Grant Device Admin** — required for `lockNow()`; without it `OFF` does
   nothing. The app declares the admin receiver but does not prompt for it, so
   enable it manually: Settings → Security → Device admin apps →
   HealthMonitorWakeTest (or via adb:
   `adb shell dpm set-active-admin com.example.healthmonitorwaketest/.MyDeviceAdminReceiver`).
4. **Disable battery optimization** for the app (Settings → Battery), otherwise
   Android may kill the foreground MQTT service. The app does not request this
   exemption itself.
5. Start-on-boot works via `BootReceiver` (`BOOT_COMPLETED` /
   `QUICKBOOT_POWERON` → `startForegroundService(MQTTService)`). Note that
   Android only delivers `BOOT_COMPLETED` to apps the user has launched at
   least once.
6. Smoke test without radar, from Termux on the tablet:
   `mosquitto_pub -h 127.0.0.1 -p 1883 -t display -m ON` (screen wakes),
   `-m OFF` (screen locks).

The other half of the coupling — Mosquitto with both listeners (`:8883` mTLS +
`:1883` loopback plain), the mirror reader cert, and
`tablet_presence_screen.py` — is deployed by `scripts/demo_start.sh`; see the
design doc.

## Known operational gotchas (project findings)

- **Keep the tablet awake enough for the broker.** The screen-off part of this
  coupling works (`lockNow()` blanks the display), but if the tablet is allowed
  to actually sleep, Android Doze kills Mosquitto and sshd in Termux and the
  hotspot IP can drift — taking down the whole system, not just the mirror. In
  practice: battery optimization off for Termux and this app, and keep the
  device on power.
- **Radar ghost presence from walls.** A wall ~2.5 m in the MR60BHA2's cone can
  produce sustained `presence:true` with `distance_mm:null` and `q:2`. The
  gating lives in `tablet_presence_screen.py`, which currently keys on raw
  `v.presence` and only drops `q==3` samples — so a `q:2` wall ghost keeps the
  screen on forever. If this bites, either clear the radar's field of view
  (moving the wall/repointing the sensor fixed it on the bench) or change the
  bridge to require `distance_mm != null` before counting presence. Do the fix
  in the bridge script, not in this app and not in the firmware.
- The `display` payloads are compared after `.trim().uppercase()`, so `on`,
  `ON `, etc. all work — but only the literal strings `ON`/`OFF` do anything.
- `MQTTService` uses a foreground notification ("MQTT Service Running"); if
  that notification is missing, the service is not running — reopen the app or
  reboot.

## Pointers

- `docs/presence_screen_coupling.md` — authoritative design doc (chain,
  debounce parameters, deployment, test procedure).
- `docs/adr/0004-localhost-plain-listener-app-ipc.md` — why the plain `:1883`
  loopback listener exists and its security boundary.
- `scripts/tablet_presence_screen.py` — the presence→ON/OFF bridge (the policy
  half of this coupling).
