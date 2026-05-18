//MMM-SensorUI.js:

let heartRate = 65; // UWB-HF
let respiratoryRate = 10; // UWB-HF
let temperature = 22; // BME or ENS
let humidity = 45; // BME or ENS

let pressure = 101; // BME
let co2 = 600; // ENS
let tvoc = 300; // ENS
let aqi = 28; // ENS

// Test?


Module.register("MMM-SensorUI", {
  // Default module config.
  defaults: {
    text: "Hello World!",
  },

  // Override dom generator.
  getDom: function () {
    const wrapper = document.createElement("div");
    wrapper.innerHTML = this.config.text;
    return wrapper;
  },
//   getHeader: function() { // Not sure if this is necessary
// 	return this.data.header + ' Foo Bar';
//   }
});