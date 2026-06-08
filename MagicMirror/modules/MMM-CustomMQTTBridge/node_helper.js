/*
 * MMM-CustomMQTTBridge / node_helper
 *
 * Adapter between two namespaces the mirror uses (CLAUDE.md §9.5.1):
 *   • broker MQTT contract  — rmms/<uuid>/<kind> with the §9.2 JSON envelope
 *   • mirror notification bus — sensors/<field> strings via MQTT_SENSOR_UPDATE
 *
 * THIS FILE IS THE CANONICAL MAPPING TABLE referenced by CLAUDE.md §9.5.1.
 * If a new sensor field is added, edit it here in exactly one place:
 *
 *   Broker topic + field                  Mirror notification     Value (string)
 *   ───────────────────────────────────── ─────────────────────── ──────────────
 *   rmms/<uuid>/env    v.temp_c           sensors/temperature     float, .toFixed(1)
 *                      v.hum_pct          sensors/humidity        int, rounded
 *   rmms/<uuid>/air    v.aqi (1..5)       sensors/airquality      UBA label
 *                      v.co2_ppm          sensors/co2             int
 *                      v.tvoc_ppb         sensors/tvoc            int
 *   rmms/<uuid>/radar  v.heart_bpm        sensors/heartrate       int, rounded
 *                      v.breath_bpm       sensors/respiratoryrate int, rounded
 *   rmms/<uuid>/info   v.text             sensors/infomessage     text
 *   rmms/<uuid>/status (raw string)       sensors/status          "online"/"offline"
 *   rmms/<uuid>/light  ignored in v1
 *   rmms/<uuid>/log    ignored (not sensor data)
 *
 * Quality-flag handling and JSON envelope parsing live here too — they do
 * NOT belong in MMM-SensorUI, which only consumes the sensors/* events.
 *
 * Yasmina's original plaintext mqtt://localhost:1883 flow stays usable by
 * leaving caFile/certFile/keyFile empty in the module config — the bridge
 * falls back to plain MQTT and forwards unknown topics verbatim.
 */
const NodeHelper = require("node_helper");
const mqtt       = require("mqtt");
const fs         = require("fs");

const AQI_LABELS = {
    1: "EXCELLENT",
    2: "GOOD",
    3: "MODERATE",
    4: "POOR",
    5: "UNHEALTHY",
};

function readFileIfSet(path) {
    if (!path) return null;
    try {
        return fs.readFileSync(path);
    } catch (e) {
        console.error("MQTT bridge: failed to read " + path + ": " + e.message);
        return null;
    }
}

module.exports = NodeHelper.create({

    start: function () {
        console.log("MMM-CustomMQTTBridge helper started");
    },

    socketNotificationReceived: function (notification, payload) {
        if (notification !== "CONNECT_MQTT") return;
        const self = this;

        const ca   = readFileIfSet(payload.caFile);
        const cert = readFileIfSet(payload.certFile);
        const key  = readFileIfSet(payload.keyFile);
        const useTls = ca && cert && key && payload.broker.startsWith("mqtts://");

        const opts = {
            clientId: payload.clientId ||
                      ("mirror-" + Math.random().toString(16).slice(2, 10)),
            reconnectPeriod: 5000,
            connectTimeout: 10 * 1000,
        };
        if (useTls) {
            opts.ca   = ca;
            opts.cert = cert;
            opts.key  = key;
            opts.rejectUnauthorized = true;
        }

        console.log("MMM-CustomMQTTBridge connecting:",
                    payload.broker, "TLS=" + useTls, "clientId=" + opts.clientId);

        const client = mqtt.connect(payload.broker, opts);

        client.on("connect", function () {
            console.log("MQTT connected — subscribing to:", payload.topics);
            self.sendSocketNotification("MQTT_BRIDGE_STATUS", "connected");
            client.subscribe(payload.topics, function (err) {
                if (err) {
                    console.log("Subscribe error:", err);
                } else {
                    console.log("Subscribed.");
                }
            });
        });

        client.on("reconnect", function () {
            console.log("MQTT reconnecting…");
            self.sendSocketNotification("MQTT_BRIDGE_STATUS", "reconnecting");
        });

        client.on("error", function (err) {
            console.log("MQTT error:", err.message || err);
            self.sendSocketNotification("MQTT_BRIDGE_STATUS",
                                        "error: " + (err.message || err));
        });

        client.on("close", function () {
            console.log("MQTT connection closed.");
            self.sendSocketNotification("MQTT_BRIDGE_STATUS", "closed");
        });

        client.on("message", function (topic, raw) {
            const text  = raw.toString();
            const parts = topic.split("/");
            const kind  = parts.length >= 3 && parts[0] === "rmms" ? parts[2] : null;

            // Topic shape we don't recognise — relay verbatim so legacy
            // sensors/... topics keep working with Yasmina's test setup.
            if (!kind) {
                self.sendSocketNotification("MQTT_SENSOR_UPDATE", {
                    topic: topic,
                    message: text,
                });
                return;
            }

            // status is a plain string ("online" / "offline"); kept for
            // bridge introspection, not displayed by Yasmina's MMM-SensorUI.
            if (kind === "status") {
                self.sendSocketNotification("MQTT_SENSOR_UPDATE", {
                    topic: "sensors/status",
                    message: text,
                });
                return;
            }

            let env;
            try { env = JSON.parse(text); }
            catch (e) {
                console.log("MQTT bridge: non-JSON on " + topic + ": " + text);
                return;
            }
            const v = env.v || env;   // tolerant of "v" envelope or flat

            function send(name, value) {
                self.sendSocketNotification("MQTT_SENSOR_UPDATE", {
                    topic: "sensors/" + name,
                    message: String(value),
                });
            }

            if (kind === "env") {
                if (v.temp_c  != null) send("temperature", Number(v.temp_c).toFixed(1));
                if (v.hum_pct != null) send("humidity",    Math.round(Number(v.hum_pct)).toString());
            } else if (kind === "air") {
                // AQI 1..5 is a valid UBA index; aqi 0 means the ENS160 has no
                // usable reading yet (first seconds after power-on). Show the
                // real label as soon as aqi>=1 — the chip flags data as
                // INITIAL_STARTUP (envelope q=2) for up to ~1 h while it keeps
                // stabilising, but those readings are already meaningful, so we
                // do NOT hide them behind "WARMUP". Only aqi==0 shows "WARMUP".
                if (v.aqi != null) {
                    const label = Number(v.aqi) >= 1
                        ? (AQI_LABELS[v.aqi] || ("AQI " + v.aqi))
                        : "WARMUP";
                    send("airquality", label);
                }
                if (v.co2_ppm  != null) send("co2",        String(v.co2_ppm));
                if (v.tvoc_ppb != null) send("tvoc",       String(v.tvoc_ppb));
            } else if (kind === "radar") {
                if (v.heart_bpm  != null) send("heartrate",       Math.round(Number(v.heart_bpm)).toString());
                if (v.breath_bpm != null) send("respiratoryrate", Math.round(Number(v.breath_bpm)).toString());
            } else if (kind === "info") {
                if (v.text != null) send("infomessage", v.text);
            }
            // log / light are intentionally ignored.
        });
    }
});
