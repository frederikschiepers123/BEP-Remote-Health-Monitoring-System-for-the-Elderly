//MMM-SensorUI.js:

let heartRate = 65; // UWB-HF
let respiratoryRate = 10; // UWB-HF
let temperature = 22; // BME or ENS
let humidity = 45; // BME or ENS

let pressure = 101; // BME
let co2 = 600; // ENS
let tvoc = 300; // ENS
let aqi = 28; // ENS

Module.register("MMM-SensorUI", {
  defaults: {
    // Define required scripts.
    getStyles () {
      return ["MMM-SensorUI.css", "font-awesome.css"];
    },
  },
  // Override dom generator.

  getDom: function () {
    // Main container
    const wrapper = document.createElement("div");
    wrapper.className = "sensorWrapper";

    // Sensor data array
    const sensors = [
      {
        icon: "fa-heartbeat",
        value: heartRate,
        label: "BPM"
      },
      {
        icon: "fa-lungs",
        value: respiratoryRate,
        label: "RPM"
      },
      {
        icon: "fa-temperature-half",
        value: temperature + "°C",
        label: "Temp"
      },
      {
        icon: "fa-droplet",
        value: humidity + "%",
        label: "Humidity"
      }
    ];

    // Create each sensor block
    sensors.forEach(sensor => {

      // Individual sensor container
      const sensorDiv = document.createElement("div");
      sensorDiv.className = "sensor";

      // Icon
      const icon = document.createElement("i");
      icon.className = `fas ${sensor.icon} sensorIcon`;

      // Value text
      const value = document.createElement("div");
      value.className = "sensorValue";
      value.innerHTML = sensor.value;

      // Optional label
      const label = document.createElement("div");
      label.className = "sensorLabel";
      label.innerHTML = sensor.label;

      // Assemble
      sensorDiv.appendChild(icon);
      sensorDiv.appendChild(value);
      sensorDiv.appendChild(label);

      wrapper.appendChild(sensorDiv);
    });

    return wrapper;
  },
});

// Module.register("MMM-SensorUI", {
//   // Default module config.
//   defaults: {
//     // text: "Hello World!",
//     text: heartRate,
//   },

//   // Override dom generator.
//   getDom: function () {  // visual part of module
//     const wrapper = document.createElement("div");
//     // wrapper.innerHTML = this.config.text;
//     wrapper.innerHTML = `
//       Heart Rate: ${heartRate}<br>
//       Temperature: ${temperature}°C<br>
//       CO2: ${co2} ppm`;
//     return wrapper;
//   },
//   // getHeader: function() {
// 	// return "Sensor Measurements";
//   // }
// });