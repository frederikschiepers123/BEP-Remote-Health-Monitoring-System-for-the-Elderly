const NodeHelper = require("node_helper");
const mqtt = require("mqtt");

module.exports = NodeHelper.create({

    start: function () {
        console.log("MMM-MQTTBridge helper started");
    },

    socketNotificationReceived: function(notification, payload) {

        if (notification === "CONNECT_MQTT") {

            const self = this;

            const client = mqtt.connect(payload.broker);

            client.on("connect", function () {

                console.log("Connected to MQTT broker");

                client.subscribe(payload.topics, function (err) {

                    if (!err) {
                        console.log("Subscribed to:", payload.topic);
                    }
                });
            });

            client.on("message", function(topic, message) {

                const msg = message.toString();

                console.log("MQTT message:", msg);

                self.sendSocketNotification("MQTT_MESSAGE", {
                    topic: topic,
                    message: msg
                });
            });

            client.on("error", function(error) {
                console.log("MQTT Error:", error);
            });
        }
    }
});