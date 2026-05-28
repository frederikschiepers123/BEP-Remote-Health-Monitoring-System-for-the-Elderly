//MMM-SensorUI.js:

// Potential placeholder values:
// let heartRate = 65; // UWB-HF
// let respiratoryRate = 10; // UWB-HF
// let temperature = 22; // BME or ENS
// let humidity = 45; // BME or ENS

// let pressure = 101; // BME
// let co2 = 600; // ENS
// let tvoc = 300; // ENS
// let aqi = 28; // ENS

Module.register("MMM-SensorUI", {
  defaults: {},

  start: function () {
    this.heartRate = "...";
    this.respiratoryRate = "...";
    this.temperature = "...";
    this.humidity = "...";

    this.pressure = "...";
    this.co2 = "...";
    this.tvoc = "...";
    this.aqi = "...";
  },

  // Define required scripts.
  getStyles () {
    return ["MMM-SensorUI.css", "font-awesome.css"];
  },

  getDom: function () {
    // Main container
    const wrapper = document.createElement("div");
    wrapper.className = "sensorWrapper";

    // LEFT COLUMN (4 sensors)
    const col1 = document.createElement("div");
    col1.className = "sensorColumn";

    const sensorsLeft = [
      {
        icon: "fa-heartbeat",
        value: this.heartRate,
        label: "BPM"
      },
      {
        icon: "fa-lungs",
        value: this.respiratoryRate,
        label: "RPM"
      },
      {
        icon: "fa-temperature-half",
        value: this.temperature + "°C",
        label: "Temperature"
      },
      {
        icon: "fa-droplet",
        value: this.humidity + "%",
        label: "Humidity"
      }
    ];

    sensorsLeft.forEach(sensor => {
      col1.appendChild(createSensor(sensor));
    });

    // RIGHT COLUMN (3 sensors)
    const col2 = document.createElement("div");
    col2.className = "sensorColumn";

    const sensorsRight = [
      // {
      //   icon: "fa-gauge",
      //   value: this.pressure + " hPa",
      //   label: "Pressure"
      // },
      {
        icon: "fa-smog",
        value: this.co2 + " ppm",
        label: "CO2"
      },
      {
        icon: "fa-wind",
        value: this.tvoc + " ppb",
        label: "TVOC"
      },
      {
        icon: "fa-chart-simple",
        value: this.aqi,
        label: "AQI"
      }
    ];

    sensorsRight.forEach(sensor => {
      col2.appendChild(createSensor(sensor));
    });

    // Assemble layout
    wrapper.appendChild(col1);
    wrapper.appendChild(col2);

    return wrapper;
  },


  notificationReceived: function(notification, payload, sender) {

      if (notification === "MQTT_SENSOR_UPDATE") {

          console.log("SensorUI received:", payload);

          // Example topic handling
          if (payload.topic === "sensors/temperature") {
              this.temperature = payload.message;
          }

          if (payload.topic === "sensors/humidity") {
              this.humidity = payload.message;
          }

          this.updateDom();
      }
  }

});

function createSensor(sensor) {
  const sensorDiv = document.createElement("div");
  sensorDiv.className = "sensor";

  const icon = document.createElement("i");
  icon.className = `fas ${sensor.icon} sensorIcon`;

  const value = document.createElement("div");
  value.className = "sensorValue";
  value.innerHTML = sensor.value;

  const label = document.createElement("div");
  label.className = "sensorLabel";
  label.innerHTML = sensor.label;

  sensorDiv.appendChild(icon);
  sensorDiv.appendChild(value);
  sensorDiv.appendChild(label);

  return sensorDiv;
}