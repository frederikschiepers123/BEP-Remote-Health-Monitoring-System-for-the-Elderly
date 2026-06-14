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

    /* Phase-based breath-hold detection (firmware ADR-0006, forwarded as
     * sensors/respiratorymotion). "false" = the radar sees no chest motion
     * while a person is present → show a "No breathing" state on the breath
     * tile instead of the (then-suppressed) rate. "true"/"" = normal. */
    this.respiratoryMotion = "";

    this.temperature = "...";
    this.temperatureTL = "green";
    this.humidity = "...";
    this.humidityTL = "green";

    this.airQuality = "...";
    this.airQualityTL = "green";

    /* Operator message (rmms/<uuid>/info → sensors/infomessage).  Empty until
     * one arrives — the footer's default content is the last-reading
     * timestamp (supervisor feedback), not a static placeholder. */
    this.infoMessage = "";

    /* Timestamp of the most recently received sensor reading (any data
     * topic: vitals or environment).  Rendered in the footer; freezes when
     * the device stops publishing, which makes an outage visible at a
     * glance. */
    this.lastReadingAt = null;

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

    // Each vital tile is independent: heart and breath arrive (and clear) on
    // their own cadence — the radar carries one through the other's burst —
    // so an absent vital must show "Measuring..." for ITS tile only, never
    // blank the other. Only when BOTH are absent do we collapse to the
    // combined measuring panel.
    const heartReady  = hasValue(this.heartRate);
    const breathReady = hasValue(this.respiratoryRate);
    // A confirmed breath-hold (resp_motion=false) is itself a vitals reading,
    // so the breath tile is never "blank" while it holds — keep us out of the
    // combined "Measuring..." panel even if both rate numbers are absent.
    const breathHold  = this.respiratoryMotion === "false";

    if (!heartReady && !breathReady && !breathHold) {

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
        heartReady
          ? createStatusCard("fa-heart-pulse", this.heartRate, "BPM",
                             this.heartRateTL)
          : createMeasuringCard("fa-heart-pulse")
      );

      // Breath tile precedence: a confirmed hold overrides the rate (which the
      // firmware nulls during a hold), then a live rate, then "Measuring...".
      let breathCard;
      if (breathHold) {
        breathCard = createStatusCard("fa-lungs", "No breathing", "", "red", true);
      } else if (breathReady) {
        breathCard = createStatusCard("fa-lungs", this.respiratoryRate,
                                      "breaths<br>per min", this.respiratoryRateTL);
      } else {
        breathCard = createMeasuringCard("fa-lungs");
      }
      vitalWrapper.appendChild(breathCard);
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

    /* Footer content: TWO separate "last stable reading" timestamps — one for
     * vitals (heart/breath), one for environmentals (temp/humidity/air) —
     * since the two come from different sensors at different rates and either
     * can go silent independently. A frozen vitals stamp with a live env stamp
     * means the radar stopped while the environment sensors kept publishing.
     * An operator info message (if any) is shown above. */
    const vitalsLine = this.lastVitalAt
      ? "Vitals updated: " + this.lastVitalAt.toLocaleTimeString()
      : "Vitals updated: waiting…";
    const envLine = this.lastEnvAt
      ? "Environment updated: " + this.lastEnvAt.toLocaleTimeString()
      : "Environment updated: waiting…";
    const stampBlock =
      '<span style="font-size:0.75em;opacity:0.7;">' +
      vitalsLine + "<br>" + envLine + "</span>";

    if (this.infoMessage) {
      infoText.innerHTML = this.infoMessage + "<br>" + stampBlock;
    } else {
      infoText.innerHTML = stampBlock;
    }

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

          /* Refresh the per-category "last stable reading" timestamp. A
           * non-empty value here is already a stable/filtered reading from the
           * firmware (empty = a clear sent when nobody is present), so the
           * stamp freezes exactly when that category goes silent. Vitals and
           * environmentals are tracked separately (two footer lines). */
          if (payload.message !== "") {
              if (payload.topic === "sensors/heartrate" ||
                  payload.topic === "sensors/respiratoryrate" ||
                  payload.topic === "sensors/respiratorymotion") {
                  this.lastVitalAt = new Date();
              } else if (payload.topic === "sensors/temperature" ||
                         payload.topic === "sensors/humidity" ||
                         payload.topic === "sensors/airquality" ||
                         payload.topic === "sensors/co2" ||
                         payload.topic === "sensors/tvoc") {
                  this.lastEnvAt = new Date();
              }
          }

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

              this.respiratoryRateTL =
                getTrafficLight(
                  this.respiratoryRate,
                  THRESHOLDS.respiratoryRate
                );
          }
          if (payload.topic === "sensors/respiratorymotion") {
              // "true" / "false" / "" (undetermined). getDom shows a
              // "No breathing" tile when this is "false" (ADR-0006).
              this.respiratoryMotion = payload.message;
          }
          if (payload.topic === "sensors/temperature") {
              this.temperature = payload.message;

              this.temperatureTL =
                getTrafficLight(
                  this.temperature,
                  THRESHOLDS.temperature
                );
          }
          if (payload.topic === "sensors/humidity") {
              this.humidity = payload.message;

              this.humidityTL =
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


/* A single-tile "Measuring..." placeholder, occupying the same card slot as a
 * real vital so the layout doesn't shift when only ONE vital is absent (the
 * other keeps showing its number). */
function createMeasuringCard(iconName) {
  const card = document.createElement("div");
  card.className = "statusCard measuringCard";

  const icon = document.createElement("div");
  icon.className = "statusMainIcon";
  icon.innerHTML = `<i class="fas ${iconName}"></i>`;

  const valueDiv = document.createElement("div");
  valueDiv.className = "statusValue statusValueTextOnly";
  const textSpan = document.createElement("span");
  textSpan.className = "statusTextOnly";
  textSpan.innerHTML = "Measuring...";
  valueDiv.appendChild(textSpan);

  card.appendChild(icon);
  card.appendChild(valueDiv);
  return card;
}

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

  // An absent/cleared vital ("" sent when nobody is present, or a non-numeric
  // message) has no severity — return "" so it never renders as red.
  if (!hasValue(value) || !Number.isFinite(Number(value))) {
    return "";
  }

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
