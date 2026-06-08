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

const DISPLAY_MODE = "icons";
// const DISPLAY_MODE = "colors";

const THRESHOLDS = {

  heartRate: {
    green: [60, 100],
    yellow: [50, 110]
  },

  respiratoryRate: {
    green: [12, 20],
    yellow: [10, 24]
  },

  temperature: {
    green: [20, 24],
    yellow: [18, 27]
  },

  humidity: {
    green: [40, 60],
    yellow: [30, 70]
  },

};

const AIR_QUALITY_STATUS = {
  "Excellent": "green",
  "Good": "green",
  "Moderate": "yellow",
  "Poor": "red",
  "Unhealthy": "red",
};

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

  },

  // start: function () {
  //   this.heartRate = "55";
  //   this.respiratoryRate = "12";
  //   this.temperature = "17";
  //   this.humidity = "65";
  //   this.airQuality = "Moderate";

  //   this.heartRateTL = getTrafficLight(this.heartRate, THRESHOLDS.heartRate); // TL = trafficlight
  //   this.respiratoryRateTL = getTrafficLight(this.respiratoryRate, THRESHOLDS.respiratoryRate);
  //   this.temperatureTL = getTrafficLight(this.temperature, THRESHOLDS.temperature);
  //   this.humidityTL = getTrafficLight(this.humidity, THRESHOLDS.humidity);
  //   this.airQualityTL = AIR_QUALITY_STATUS[this.airQuality] || "red";

  //   this.infoMessage = "All systems operating normally.";
  // },

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


notificationReceived: function(notification, payload, sender) {

      if (notification === "MQTT_SENSOR_UPDATE") {

          console.log("SensorUI received:", payload);

          // Example topic handling
          if (payload.topic === "sensors/heartrate") {
              this.heartRate = payload.message;

              this.heartRateTL =
                getTrafficLight(
                  this.heartRate,
                  THRESHOLDS.heartRate
                );
          }
          if (payload.topic === "sensors/respiratoryrate") {
              this.respiratoryRate = payload.message;

              this.heartRateTL =
                getTrafficLight(
                  this.respiratoryRate,
                  THRESHOLDS.respiratoryRate
                );
          }
          if (payload.topic === "sensors/temperature") {
              this.temperature = payload.message;

              this.heartRateTL =
                getTrafficLight(
                  this.temperature,
                  THRESHOLDS.temperature
                );
          }
          if (payload.topic === "sensors/humidity") {
              this.humidity = payload.message;

              this.heartRateTL =
                getTrafficLight(
                  this.humidity,
                  THRESHOLDS.humidity
                );
          }
          if (payload.topic === "sensors/airquality") {
              this.airQuality = payload.message;

              this.airQualityTL = AIR_QUALITY_STATUS[this.airQuality] || "red";
          }
          if (payload.topic === "sensors/infomessage") {
              this.infoMessage = payload.message;
          }

          this.updateDom();
      }
  }

});


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

  if (DISPLAY_MODE === "colors") {

    if (status === "green") {
      valueDiv.classList.add("statusGreen");
    }

    else if (status === "yellow") {
      valueDiv.classList.add("statusYellow");
    }

    else if (status === "red") {
      valueDiv.classList.add("statusRed");
    }
  }

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
  if (DISPLAY_MODE === "icons") {
    card.appendChild(statusDiv);
  }

  return card;
}

function hasValue(value) {
  return value !== "..." &&
         value !== undefined &&
         value !== null &&
         value !== "";
}

function getTrafficLight(value, threshold) {

  value = Number(value);

  const [greenMin, greenMax] = threshold.green;

  if (value >= greenMin && value <= greenMax) {
    return "green";
  }

  const [yellowMin, yellowMax] = threshold.yellow;

  if (value >= yellowMin && value <= yellowMax) {
    return "yellow";
  }

  return "red";
}