/*
 * MMM-CustomMQTTBridge — RMMS firmware ↔ MMM-SensorUI adapter.
 *
 * Subscribes to the firmware's raw rmms/<uuid>/... topics over mTLS, parses
 * the JSON envelope in the node_helper, and re-broadcasts each field as the
 * `sensors/<name>` notification that MMM-SensorUI already listens for.
 * MMM-SensorUI is not changed.
 *
 * Config (set in MagicMirror/config/config.js):
 *   broker:   "mqtts://<broker-ip>:8883"
 *   clientId: "mirror-<short-id>"           — must match the mirror cert CN
 *                                             from scripts/provision_ca.sh
 *   caFile:   absolute path to ca.crt       (PEM, from out/mirror-<id>/)
 *   certFile: absolute path to cert.pem
 *   keyFile:  absolute path to key.pem
 *   topics:   ["rmms/+/+"]                  (wildcard for all devices)
 *
 * If caFile/certFile/keyFile are left empty the bridge falls back to plain
 * mqtt://localhost:1883 (Yasmina's original test path) so the module remains
 * usable for local mock setups.
 */
Module.register("MMM-CustomMQTTBridge", {

    defaults: {
        broker:   "mqtts://172.20.10.10:8883",
        clientId: "mirror-de4ed19a",
        caFile:   "",
        certFile: "",
        keyFile:  "",
        topics:   ["rmms/+/+"],
    },

    start: function () {
        this.message = "Waiting for MQTT...";

        this.sendSocketNotification("CONNECT_MQTT", {
            broker:   this.config.broker,
            clientId: this.config.clientId,
            caFile:   this.config.caFile,
            certFile: this.config.certFile,
            keyFile:  this.config.keyFile,
            topics:   this.config.topics,
        });
    },

    getDom: function () {
        const wrapper = document.createElement("div");
        wrapper.innerHTML = this.message;
        return wrapper;
    },

    socketNotificationReceived: function (notification, payload) {

        if (notification === "MQTT_SENSOR_UPDATE") {
            // node_helper already translated rmms/...→sensors/... and is
            // sending one MQTT_SENSOR_UPDATE per logical field. Relay it
            // onward so MMM-SensorUI receives it.
            this.sendNotification("MQTT_SENSOR_UPDATE", payload);
            this.message = `${payload.topic} = ${payload.message}`;
            this.updateDom();
        }

        if (notification === "MQTT_BRIDGE_STATUS") {
            console.log("MQTT bridge:", payload);
            this.message = "MQTT: " + payload;
            this.updateDom();
        }
    },

    getHeader: function () {
        return "MQTT bridge";
    }
});
