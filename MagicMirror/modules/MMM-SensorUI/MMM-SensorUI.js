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
    this.heartRateTL = "green";  // TL = trafficlight
    this.respiratoryRate = "...";
    this.respiratoryRateTL = "green";

    this.temperature = "...";
    this.temperatureTL = "green";
    this.humidity = "...";
    this.humidityTL = "green";

    this.airQuality = "...";
    this.airQualityTL = "green";

    this.infoMessage = "All systems operating normally.";

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

    /*
      * =========================
      * VITAL WRAPPER
      * =========================
      */

    const vitalWrapper = document.createElement("div");
    vitalWrapper.className = "vitalWrapper";

    const vitalReady =
      hasValue(this.heartRate) &&
      hasValue(this.respiratoryRate);

    if (!vitalReady) {

      vitalWrapper.classList.add("measuringMode");

      const iconRow = document.createElement("div");
      iconRow.className = "measuringIcons";

      iconRow.innerHTML = `
        <i class="fas fa-heart-pulse"></i>
        <i class="fas fa-lungs"></i>
      `;

      const measuringText = document.createElement("div");
      measuringText.className = "measuringText";
      measuringText.innerHTML = "Measuring...";

      vitalWrapper.appendChild(iconRow);
      vitalWrapper.appendChild(measuringText);

    } else {

      vitalWrapper.appendChild(
        createStatusCard(
          "fa-heart-pulse",
          this.heartRate,
          "BPM",
          this.heartRateTL
        )
      );

      vitalWrapper.appendChild(
        createStatusCard(
          "fa-lungs",
          this.respiratoryRate,
          "breaths<br>per min",
          this.respiratoryRateTL
        )
      );
    }

    // vitalWrapper.appendChild(
    //   createStatusCard(
    //     "fa-heart-pulse",
    //     this.heartRate,
    //     "BPM",
    //     this.heartRateTL
    //   )
    // );

    // vitalWrapper.appendChild(
    //   createStatusCard(
    //     "fa-lungs",
    //     this.respiratoryRate,
    //     "RPM",
    //     this.respiratoryRateTL
    //   )
    // );

    /*
      * =========================
      * ENVIRONMENT WRAPPER
      * =========================
      */

    const envWrapper = document.createElement("div");
    envWrapper.className = "envWrapper";

    const envReady =
      hasValue(this.temperature) &&
      hasValue(this.humidity) &&
      hasValue(this.airQuality);

    if (!envReady) {

      envWrapper.classList.add("measuringMode");

      const iconRow = document.createElement("div");
      iconRow.className = "measuringIcons";

      iconRow.innerHTML = `
        <i class="fas fa-temperature-half"></i>
        <i class="fas fa-droplet"></i>
        <i class="fas fa-wind"></i>
      `;

      const measuringText = document.createElement("div");
      measuringText.className = "measuringText";
      measuringText.innerHTML = "Measuring...";

      envWrapper.appendChild(iconRow);
      envWrapper.appendChild(measuringText);

    } else {

      envWrapper.appendChild(
        createStatusCard(
          "fa-temperature-half",
          this.temperature,
          "°C",
          this.temperatureTL
        )
      );

      envWrapper.appendChild(
        createStatusCard(
          "fa-droplet",
          this.humidity,
          "%",
          this.humidityTL
        )
      );

      envWrapper.appendChild(
        createStatusCard(
          "fa-wind",
          this.airQuality,
          "",
          this.airQualityTL,
          true
        )
      );
    }

    // envWrapper.appendChild(
    //   createStatusCard(
    //     "fa-temperature-half",
    //     this.temperature,
    //     "°C",
    //     this.temperatureTL
    //   )
    // );

    // envWrapper.appendChild(
    //   createStatusCard(
    //     "fa-droplet",
    //     this.humidity,
    //     "%",
    //     this.humidityTL
    //   )
    // );

    // envWrapper.appendChild(
    //   createStatusCard(
    //     "fa-wind",
    //     this.airQuality,
    //     "AQI",
    //     this.airQualityTL
    //   )
    // );    

    /*
    * =========================
    * INFO WRAPPER
    * =========================
    */

    const infoWrapper = document.createElement("div");
    infoWrapper.className = "infoWrapper";

    const infoIcon = document.createElement("div");
    infoIcon.className = "infoIcon";

    infoIcon.innerHTML = `<i class="fas fa-circle-info"></i>`;

    const infoText = document.createElement("div");
    infoText.className = "infoText";
    infoText.innerHTML = this.infoMessage;

    infoWrapper.appendChild(infoIcon);
    infoWrapper.appendChild(infoText);

    /*
    * =========================
    * ASSEMBLE
    * =========================
    */

    wrapper.appendChild(vitalWrapper);
    wrapper.appendChild(envWrapper);
    wrapper.appendChild(infoWrapper);

    return wrapper;

  },
    // // LEFT COLUMN (4 sensors)
    // const col1 = document.createElement("div");
    // col1.className = "sensorColumn";

    // const sensorsLeft = [
    //   {
    //     icon: "fa-heartbeat",
    //     value: this.heartRate,
    //     label: "BPM"
    //   },
    //   {
    //     icon: "fa-lungs",
    //     value: this.respiratoryRate,
    //     label: "RPM"
    //   },
    //   {
    //     icon: "fa-temperature-half",
    //     value: this.temperature + "°C",
    //     label: "Temperature"
    //   },
    //   {
    //     icon: "fa-droplet",
    //     value: this.humidity + "%",
    //     label: "Humidity"
    //   }
    // ];

    // sensorsLeft.forEach(sensor => {
    //   col1.appendChild(createSensor(sensor));
    // });

    // // RIGHT COLUMN (3 sensors)
    // const col2 = document.createElement("div");
    // col2.className = "sensorColumn";

    // const sensorsRight = [
    //   // {
    //   //   icon: "fa-gauge",
    //   //   value: this.pressure + " hPa",
    //   //   label: "Pressure"
    //   // },
    //   {
    //     icon: "fa-smog",
    //     value: this.co2 + " ppm",
    //     label: "CO2"
    //   },
    //   {
    //     icon: "fa-wind",
    //     value: this.tvoc + " ppb",
    //     label: "TVOC"
    //   },
    //   {
    //     icon: "fa-chart-simple",
    //     value: this.aqi,
    //     label: "AQI"
    //   }
    // ];

    // sensorsRight.forEach(sensor => {
    //   col2.appendChild(createSensor(sensor));
    // });

    // // Assemble layout
    // wrapper.appendChild(col1);
    // wrapper.appendChild(col2);

    // return wrapper;
  // }, 

  notificationReceived: function(notification, payload, sender) {

      if (notification === "MQTT_SENSOR_UPDATE") {

          console.log("SensorUI received:", payload);

          // Example topic handling
          if (payload.topic === "sensors/heartrate") {
              this.heartRate = payload.message;
          }
          if (payload.topic === "sensors/respiratoryrate") {
              this.respiratoryRate = payload.message;
          }
          if (payload.topic === "sensors/temperature") {
              this.temperature = payload.message;
          }
          if (payload.topic === "sensors/humidity") {
              this.humidity = payload.message;
          }
          if (payload.topic === "sensors/airquality") {
              this.airQuality = payload.message;
          }
          if (payload.topic === "sensors/infomessage") {
              this.infoMessage = payload.message;
          }

          this.updateDom();
      }
  }

});

// function createSensor(sensor) {
//   const sensorDiv = document.createElement("div");
//   sensorDiv.className = "sensor";

//   const icon = document.createElement("i");
//   icon.className = `fas ${sensor.icon} sensorIcon`;

//   const value = document.createElement("div");
//   value.className = "sensorValue";
//   value.innerHTML = sensor.value;

//   const label = document.createElement("div");
//   label.className = "sensorLabel";
//   label.innerHTML = sensor.label;

//   sensorDiv.appendChild(icon);
//   sensorDiv.appendChild(value);
//   sensorDiv.appendChild(label);

//   return sensorDiv;
// }

function createStatusCard(iconName, value, unit, status, textOnly = false) {

  const card = document.createElement("div");
  card.className = "statusCard";

  /*
   * TOP ICON
   */

  const icon = document.createElement("div");
  icon.className = "statusMainIcon";
  icon.innerHTML = `<i class="fas ${iconName}"></i>`;

  /*
   * VALUE
   */

  const valueDiv = document.createElement("div");
  valueDiv.className = "statusValue";

  if (textOnly) {

    valueDiv.classList.add("statusValueTextOnly");

    const textSpan = document.createElement("span");
    textSpan.className = "statusTextOnly";
    textSpan.innerHTML = value;

    valueDiv.appendChild(textSpan);

  } else {

    const numberSpan = document.createElement("span");
    numberSpan.className = "statusNumber";
    numberSpan.innerHTML = value;

    const unitSpan = document.createElement("span");
    unitSpan.className = "statusUnit";
    unitSpan.innerHTML = unit;

    valueDiv.appendChild(numberSpan);
    valueDiv.appendChild(unitSpan);
  }

  // const valueDiv = document.createElement("div");
  // valueDiv.className = "statusValue";

  // const numberSpan = document.createElement("span");
  // numberSpan.className = "statusNumber";
  // numberSpan.innerHTML = value;

  // const unitSpan = document.createElement("span");
  // unitSpan.className = "statusUnit";
  // unitSpan.innerHTML = unit;

  // valueDiv.appendChild(numberSpan);
  // valueDiv.appendChild(unitSpan);

  // valueDiv.innerHTML = `${value}${unit}`;

  /*
   * STATUS ICON
   */

  const statusDiv = document.createElement("div");
  statusDiv.className = "statusIndicator";

  let statusHTML = "";

  if (status === "green") {
    statusHTML =
      `<i class="fas fa-circle-check statusGreen"></i>`;
  }

  else if (status === "yellow") {
    statusHTML =
      `<i class="fas fa-circle-exclamation statusYellow"></i>`;
  }

  else if (status === "red") {
    statusHTML =
      `<i class="fas fa-triangle-exclamation statusRed"></i>`;
  }

  statusDiv.innerHTML = statusHTML;

  /*
   * ASSEMBLE
   */

  card.appendChild(icon);
  card.appendChild(valueDiv);
  card.appendChild(statusDiv);

  return card;
}

function hasValue(value) {
  return value !== "..." &&
         value !== undefined &&
         value !== null &&
         value !== "";
}