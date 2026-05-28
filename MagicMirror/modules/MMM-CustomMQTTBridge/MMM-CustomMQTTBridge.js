Module.register("MMM-CustomMQTTBridge", {

    defaults: {
        broker: "mqtt://localhost:1883",
        topics: ["magicmirror/test", "sensors/#"]
    },

    // Alleen getal (geen unit)
    // mirror/patient01/vitals/heartrate OR respiratory
    // mirror/patient01/environment/CO2 OR 
    // mirror 

    // patient01/bathroom/vitals/



    start: function () {
        this.message = "Waiting for MQTT...";
        
        this.sendSocketNotification("CONNECT_MQTT", {
            broker: this.config.broker,
            topics: this.config.topics
        });
    },

    getDom: function () {
        const wrapper = document.createElement("div");
        wrapper.innerHTML = this.message;
        return wrapper;
    },

    socketNotificationReceived: function(notification, payload) {

        if (notification === "MQTT_MESSAGE") {
            // this.message = payload;
            console.log("Frontend received MQTT:", payload);
            // Broadcast globally to all MM modules
            this.sendNotification("MQTT_SENSOR_UPDATE", payload);

            this.message = JSON.stringify(payload);
            this.updateDom();
        }
    },
    getHeader: function() {
	    return 'MQTT testing';
    }
});