#include <Arduino.h>
#include <math.h>

// ============================================================
//  1. HARDWARE / UART SETTINGS
// ============================================================

// Pico 2 W UART pins
static const int RADAR_TX_PIN = 4;   // Pico GP0 -> Radar RX0
static const int RADAR_RX_PIN = 5;   // Pico GP1 <- Radar TX0

#define RADAR_SERIAL Serial2

// Optional reset pin. Leave as -1 if not connected.
static const int RADAR_RST_PIN = -1;

// Seeed examples often use 115200.
// If no valid frames are received, try 1382400.
static const uint32_t RADAR_BAUD = 115200;

// Print continuous phase values only for debugging.
static const bool PRINT_PHASES = false;

// Print raw values before filtering.
static const bool PRINT_RAW = false;


// ============================================================
//  2. RADAR FRAME PROTOCOL SETTINGS
// ============================================================

static const uint8_t SOF = 0x01;       // Start of frame byte
static const size_t HEADER_LEN = 8;    // Radar header length
static const size_t MAX_DATA_LEN = 64; // Maximum accepted payload size

uint8_t headerBuf[HEADER_LEN];
uint8_t dataBuf[MAX_DATA_LEN];

enum ParseState {
  WAIT_SOF,
  READ_HEADER,
  READ_DATA_AND_CHECKSUM
};

ParseState state = WAIT_SOF;

size_t headerIndex = 0;
size_t dataIndex = 0;

uint16_t dataLen = 0;
uint16_t frameType = 0;

uint32_t lastValidFrameMs = 0;


// ============================================================
//  3. FILTER SETTINGS
// ============================================================

// How often the filtered result is printed.
static const uint32_t OUTPUT_EVERY_MS = 1000;

// A radar frame older than this is considered stale.
static const uint32_t FRAME_STALE_MS = 2500;

// Presence must be detected continuously before being trusted.
static const uint32_t PRESENCE_CONFIRM_MS = 5000;

// Absence must also be stable before declaring no target.
static const uint32_t ABSENCE_CONFIRM_MS = 4000;

// Distance range used for vital-sign detection.

static const float MIN_DISTANCE_CM = 40.0f;
static const float MAX_DISTANCE_CM = 150.0f;

// Sudden distance changes larger than this restart validation.
static const float DISTANCE_MAX_JUMP_CM = 20.0f;

// Distance must remain stable for this duration.
static const uint32_t DISTANCE_CONFIRM_MS = 4000;

// EMA smoothing time constant for distance.
// Larger value = slower but smoother.
static const float DISTANCE_TAU_MS = 5000.0f;

static const uint32_t DISTANCE_TIMEOUT_MS = 5000;


// Heart-rate plausibility and smoothing settings
static const float MIN_HEART_BPM = 45.0f;
static const float MAX_HEART_BPM = 125.0f;
static const float HEART_MAX_JUMP_BPM = 8.0f;
static const uint32_t HEART_CONFIRM_MS = 5000;
static const float HEART_TAU_MS = 6000.0f;
static const uint32_t HEART_TIMEOUT_MS = 6000;


// Breathing-rate plausibility and smoothing settings
static const float MIN_BREATH_RATE = 0.0f;
static const float MAX_BREATH_RATE = 30.0f;
static const float BREATH_MAX_JUMP = 3.0f;
static const uint32_t BREATH_CONFIRM_MS = 5000;
static const float BREATH_TAU_MS = 6500.0f;
static const uint32_t BREATH_TIMEOUT_MS = 6000;


// Optional calibration offsets.
// Keep these at 0 unless you have validated a consistent bias.
static const float HEART_CALIBRATION_OFFSET_BPM = -12.0f;
static const float BREATH_CALIBRATION_OFFSET = -2.0f;


// ============================================================
//  4. FINAL ONE-MINUTE ESTIMATE SETTINGS
// ============================================================

// The final estimate is calculated after one minute of stable data.
static const uint32_t ESTIMATE_WINDOW_MS = 20000;

// Collect one filtered sample per second.
static const uint32_t ESTIMATE_SAMPLE_EVERY_MS = 1000;

static const int ESTIMATE_MAX_SAMPLES = 20;
static const int ESTIMATE_MIN_SAMPLES = 15;

// Outlier rejection strength.
// Smaller = stricter, larger = more tolerant.
static const float ESTIMATE_OUTLIER_K = 2.5f;

// Prevent divide-by-zero when all samples are nearly identicRal.
static const float MIN_ROBUST_SIGMA = 0.5f;


// ============================================================
//  5. RAW LATEST RADAR VALUES
// ============================================================

// These are the newest decoded values from the radar.
// They are not trusted immediately.

bool rawHumanDetected = false;
uint32_t lastHumanFrameMs = 0;

bool rawDistanceValid = false;
float rawDistanceCm = 0.0f;
uint32_t lastDistanceFrameMs = 0;

float rawHeartBpm = 0.0f;
uint32_t lastHeartFrameMs = 0;

float rawBreathRate = 0.0f;
uint32_t lastBreathFrameMs = 0;


// These prevent the same radar frame from being processed many times.
uint32_t processedDistanceFrameMs = 0;
uint32_t processedHeartFrameMs = 0;
uint32_t processedBreathFrameMs = 0;


// ============================================================
//  6. STABLE ANALOG FILTER
// ============================================================
//
// This reusable filter is used for:
// - distance
// - heart rate
// - breathing rate
//
// It performs:
// 1. range check
// 2. jump rejection
// 3. confirmation delay
// 4. exponential moving average smoothing
// 5. timeout reset
// ============================================================

struct StableAnalogFilter {
  float minValue;
  float maxValue;
  float maxJump;
  float tauMs;

  uint32_t confirmMs;
  uint32_t timeoutMs;

  bool candidateActive;
  bool stable;
  bool lpfActive;

  float candidate;
  float value;

  uint32_t candidateSinceMs;
  uint32_t lastInputMs;
  uint32_t lpfLastMs;

  StableAnalogFilter(
    float minV,
    float maxV,
    float jump,
    float tau,
    uint32_t confirm,
    uint32_t timeout
  ) {
    minValue = minV;
    maxValue = maxV;
    maxJump = jump;
    tauMs = tau;
    confirmMs = confirm;
    timeoutMs = timeout;
    reset();
  }

  void reset() {
    candidateActive = false;
    stable = false;
    lpfActive = false;

    candidate = 0.0f;
    value = 0.0f;

    candidateSinceMs = 0;
    lastInputMs = 0;
    lpfLastMs = 0;
  }

  bool update(float x, uint32_t now) {
    // Step 1: reject impossible or invalid values.
    if (!isfinite(x) || x < minValue || x > maxValue) {
      return false;
    }

    lastInputMs = now;

    // Step 2: reject sudden jumps.
    // If the jump is too large, start a new candidate.
    if (!candidateActive || fabsf(x - candidate) > maxJump) {
      candidateActive = true;
      candidate = x;
      candidateSinceMs = now;

      stable = false;
      lpfActive = false;

      return false;
    }

    // Step 3: candidate tracking.
    // The candidate slowly follows small changes.
    candidate += 0.25f * (x - candidate);

    // Step 4: confirmation delay.
    // The value must remain stable long enough before being trusted.
    if (now - candidateSinceMs < confirmMs) {
      return false;
    }

    stable = true;

    // Step 5: exponential moving average.
    //
    // Formula:
    // y_k = y_(k-1) + alpha * (x_k - y_(k-1))
    //
    // where:
    // alpha = dt / (tau + dt)
    //
    // This makes the filter time-aware.
    if (!lpfActive) {
      value = x;
      lpfActive = true;
      lpfLastMs = now;
    } else {
      uint32_t dt = now - lpfLastMs;
      lpfLastMs = now;

      float alpha = (float)dt / (tauMs + (float)dt);
      value += alpha * (x - value);
    }

    return true;
  }

  void expire(uint32_t now) {
    if (lastInputMs == 0) return;

    // If no valid input arrived for too long, reset the filter.
    if (now - lastInputMs > timeoutMs) {
      reset();
    }
  }

  bool isStable() const {
    return stable && lpfActive;
  }

  float get() const {
    return value;
  }
};


// ============================================================
//  7. PRESENCE DEBOUNCE FILTER
// ============================================================
//
// This filter prevents one short false detection from being accepted.
// Presence must remain true for PRESENCE_CONFIRM_MS.
// Absence must remain true for ABSENCE_CONFIRM_MS.
// ============================================================

struct DebouncedPresence {
  bool stablePresent;
  bool candidateActive;

  uint32_t candidateSinceMs;
  uint32_t lastEvidenceMs;

  DebouncedPresence() {
    stablePresent = false;
    candidateActive = false;
    candidateSinceMs = 0;
    lastEvidenceMs = 0;
  }

  void update(bool evidence, uint32_t now) {
    if (evidence) {
      lastEvidenceMs = now;

      if (!candidateActive) {
        candidateActive = true;
        candidateSinceMs = now;
      }

      if (!stablePresent && now - candidateSinceMs >= PRESENCE_CONFIRM_MS) {
        stablePresent = true;
      }
    } else {
      if (!stablePresent) {
        candidateActive = false;
        candidateSinceMs = 0;
      }

      if (stablePresent && now - lastEvidenceMs >= ABSENCE_CONFIRM_MS) {
        stablePresent = false;
        candidateActive = false;
        candidateSinceMs = 0;
      }
    }
  }

  bool warmingUp() const {
    return candidateActive && !stablePresent;
  }
};


// ============================================================
//  8. ROBUST FINAL WINDOW ESTIMATOR
// ============================================================
//
// This is used to calculate one final value after one stable minute.
// It uses:
// 1. median
// 2. MAD: median absolute deviation
// 3. outlier rejection
// 4. mean of accepted samples
// ============================================================

struct RobustWindowEstimator {
  float samples[ESTIMATE_MAX_SAMPLES];

  int count;
  uint32_t firstSampleMs;
  uint32_t lastSampleMs;

  RobustWindowEstimator() {
    reset();
  }

  void reset() {
    count = 0;
    firstSampleMs = 0;
    lastSampleMs = 0;
  }

  void add(float x, uint32_t now) {
    if (!isfinite(x)) return;

    if (count == 0) {
      firstSampleMs = now;
    }

    if (count < ESTIMATE_MAX_SAMPLES) {
      samples[count] = x;
      count++;
      lastSampleMs = now;
    }
  }

  bool ready(uint32_t now) const {
    return count >= ESTIMATE_MIN_SAMPLES &&
           firstSampleMs > 0 &&
           now - firstSampleMs >= ESTIMATE_WINDOW_MS;
  }

  void sortArray(float *arr, int n) {
    for (int i = 1; i < n; i++) {
      float key = arr[i];
      int j = i - 1;

      while (j >= 0 && arr[j] > key) {
        arr[j + 1] = arr[j];
        j--;
      }

      arr[j + 1] = key;
    }
  }

  float median(float *arr, int n) {
    sortArray(arr, n);

    if (n % 2 == 1) {
      return arr[n / 2];
    }

    return 0.5f * (arr[n / 2 - 1] + arr[n / 2]);
  }

  bool estimate(float &estimatedValue, float &robustSpread, int &usedSamples) {
    if (count < ESTIMATE_MIN_SAMPLES) {
      return false;
    }

    float sorted[ESTIMATE_MAX_SAMPLES];
    float deviations[ESTIMATE_MAX_SAMPLES];

    for (int i = 0; i < count; i++) {
      sorted[i] = samples[i];
    }

    float med = median(sorted, count);

    for (int i = 0; i < count; i++) {
      deviations[i] = fabsf(samples[i] - med);
    }

    float mad = median(deviations, count);

    // 1.4826 converts MAD to a standard-deviation-like robust spread.
    robustSpread = 1.4826f * mad;

    if (robustSpread < MIN_ROBUST_SIGMA) {
      robustSpread = MIN_ROBUST_SIGMA;
    }

    float threshold = ESTIMATE_OUTLIER_K * robustSpread;

    float sum = 0.0f;
    usedSamples = 0;

    for (int i = 0; i < count; i++) {
      if (fabsf(samples[i] - med) <= threshold) {
        sum += samples[i];
        usedSamples++;
      }
    }

    if (usedSamples < ESTIMATE_MIN_SAMPLES / 2) {
      return false;
    }

    estimatedValue = sum / usedSamples;
    return true;
  }
};


// ============================================================
//  9. CREATE FILTER OBJECTS
// ============================================================

DebouncedPresence presenceGate;

StableAnalogFilter distanceFilter(
  MIN_DISTANCE_CM,
  MAX_DISTANCE_CM,
  DISTANCE_MAX_JUMP_CM,
  DISTANCE_TAU_MS,
  DISTANCE_CONFIRM_MS,
  DISTANCE_TIMEOUT_MS
);

StableAnalogFilter heartFilter(
  MIN_HEART_BPM,
  MAX_HEART_BPM,
  HEART_MAX_JUMP_BPM,
  HEART_TAU_MS,
  HEART_CONFIRM_MS,
  HEART_TIMEOUT_MS
);

StableAnalogFilter breathFilter(
  MIN_BREATH_RATE,
  MAX_BREATH_RATE,
  BREATH_MAX_JUMP,
  BREATH_TAU_MS,
  BREATH_CONFIRM_MS,
  BREATH_TIMEOUT_MS
);

RobustWindowEstimator heartFinalEstimator;
RobustWindowEstimator breathFinalEstimator;

bool finalEstimatePrinted = false;
uint32_t lastEstimateSampleMs = 0;


// ============================================================
//  10. BASIC BYTE DECODING HELPERS
// ============================================================

uint8_t checksum(const uint8_t *data, size_t len) {
  uint8_t ret = 0;

  for (size_t i = 0; i < len; i++) {
    ret ^= data[i];
  }

  return ~ret;
}

uint16_t readBE16(const uint8_t *p) {
  return ((uint16_t)p[0] << 8) | p[1];
}

uint32_t readLE32(const uint8_t *p) {
  return ((uint32_t)p[0]) |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

float readFloatLE(const uint8_t *p) {
  uint32_t raw = readLE32(p);

  float value;
  memcpy(&value, &raw, sizeof(value));

  return value;
}


// ============================================================
//  11. OPTIONAL RADAR RESET
// ============================================================

void resetRadarIfConnected() {
  if (RADAR_RST_PIN < 0) return;

  pinMode(RADAR_RST_PIN, OUTPUT);

  digitalWrite(RADAR_RST_PIN, LOW);
  delay(100);

  digitalWrite(RADAR_RST_PIN, HIGH);
  delay(1000);
}


// ============================================================
//  12. HANDLE COMPLETE RADAR FRAMES
// ============================================================
//
// This function receives valid decoded radar frames.
// It only stores raw values.
// Filtering is done later in updateFilteredOutput().
// ============================================================

void handleFrame(uint16_t type, const uint8_t *data, uint16_t len) {
  uint32_t now = millis();

  lastValidFrameMs = now;

  switch (type) {
    case 0x0A13: { // total phase, breath phase, heart phase
      if (len >= 12) {
        float totalPhase  = readFloatLE(data + 0);
        float breathPhase = readFloatLE(data + 4);
        float heartPhase  = readFloatLE(data + 8);

        if (PRINT_PHASES) {
          Serial.print("Phase | total: ");
          Serial.print(totalPhase, 3);

          Serial.print(" | breath: ");
          Serial.print(breathPhase, 3);

          Serial.print(" | heart: ");
          Serial.println(heartPhase, 3);
        }
      }
      break;
    }

    case 0x0A14: { // breathing rate
      if (len >= 4) {
        rawBreathRate = readFloatLE(data);
        lastBreathFrameMs = now;

        if (PRINT_RAW) {
          Serial.print("RAW breath rate: ");
          Serial.println(rawBreathRate, 2);
        }
      }
      break;
    }

    case 0x0A15: { // heart rate
      if (len >= 4) {
        rawHeartBpm = readFloatLE(data);
        lastHeartFrameMs = now;

        if (PRINT_RAW) {
          Serial.print("RAW heart rate: ");
          Serial.println(rawHeartBpm, 2);
        }
      }
      break;
    }

    case 0x0A16: { // detection distance
      if (len >= 8) {
        uint32_t flag = readLE32(data);
        float distance = readFloatLE(data + 4);

        rawDistanceValid = (flag == 1);
        rawDistanceCm = distance;
        lastDistanceFrameMs = now;

        if (PRINT_RAW) {
          Serial.print("RAW distance: ");

          if (rawDistanceValid) {
            Serial.print(rawDistanceCm, 2);
            Serial.println(" cm");
          } else {
            Serial.println("no valid target");
          }
        }
      }
      break;
    }

    case 0x0F09: { // human detected, available on newer firmware
      if (len >= 1) {
        rawHumanDetected = data[0] ? true : false;
        lastHumanFrameMs = now;

        if (PRINT_RAW) {
          Serial.print("RAW human detected: ");
          Serial.println(rawHumanDetected ? "YES" : "NO");
        }
      }
      break;
    }

    default:
      // Unknown frame type. Keep silent during normal use.
      // Uncomment for debugging:
      // Serial.print("Unknown frame type: 0x");
      // Serial.println(type, HEX);
      break;
  }
}


// ============================================================
//  13. BYTE-BY-BYTE RADAR FRAME PARSER
// ============================================================
//
// UART data arrives as a stream of bytes.
// This state machine rebuilds valid radar frames.
// ============================================================

void parseRadarByte(uint8_t b) {
  switch (state) {
    case WAIT_SOF:
      if (b == SOF) {
        headerBuf[0] = b;
        headerIndex = 1;
        state = READ_HEADER;
      }
      break;

    case READ_HEADER:
      headerBuf[headerIndex] = b;
      headerIndex++;

      if (headerIndex >= HEADER_LEN) {
        uint8_t expectedHeaderChecksum = checksum(headerBuf, 7);

        if (expectedHeaderChecksum != headerBuf[7]) {
          state = WAIT_SOF;
          return;
        }

        dataLen = readBE16(headerBuf + 3);
        frameType = readBE16(headerBuf + 5);

        if (dataLen > MAX_DATA_LEN) {
          state = WAIT_SOF;
          return;
        }

        dataIndex = 0;
        state = READ_DATA_AND_CHECKSUM;
      }
      break;

    case READ_DATA_AND_CHECKSUM:
      if (dataIndex < dataLen) {
        dataBuf[dataIndex] = b;
        dataIndex++;
      } else {
        uint8_t receivedDataChecksum = b;
        uint8_t expectedDataChecksum = checksum(dataBuf, dataLen);

        if (receivedDataChecksum == expectedDataChecksum) {
          handleFrame(frameType, dataBuf, dataLen);
        }

        state = WAIT_SOF;
      }
      break;
  }
}


// ============================================================
//  14. FINAL ONE-MINUTE ESTIMATE
// ============================================================
//
// This function collects stable filtered values for one minute.
// Then it calculates one robust estimate using median + MAD.
// ============================================================

void updateFinalEstimate(uint32_t now) {
  // Only collect samples if both vitals are already stable.
  if (!heartFilter.isStable() || !breathFilter.isStable()) {
    heartFinalEstimator.reset();
    breathFinalEstimator.reset();

    finalEstimatePrinted = false;
    lastEstimateSampleMs = 0;

    return;
  }

  // After one final estimate has been printed, stop until the target is lost.
  if (finalEstimatePrinted) {
    return;
  }

  // Collect one filtered sample per second.
  if (now - lastEstimateSampleMs >= ESTIMATE_SAMPLE_EVERY_MS) {
    lastEstimateSampleMs = now;

    float calibratedHeart = heartFilter.get() + HEART_CALIBRATION_OFFSET_BPM;
    float calibratedBreath = breathFilter.get() + BREATH_CALIBRATION_OFFSET;

    heartFinalEstimator.add(calibratedHeart, now);
    breathFinalEstimator.add(calibratedBreath, now);
  }

  // Wait until both windows are ready.
  if (!heartFinalEstimator.ready(now) || !breathFinalEstimator.ready(now)) {
    return;
  }

  float estimatedHeart = 0.0f;
  float estimatedBreath = 0.0f;

  float heartSpread = 0.0f;
  float breathSpread = 0.0f;

  int usedHeartSamples = 0;
  int usedBreathSamples = 0;

  bool heartOk = heartFinalEstimator.estimate(
    estimatedHeart,
    heartSpread,
    usedHeartSamples
  );

  bool breathOk = breathFinalEstimator.estimate(
    estimatedBreath,
    breathSpread,
    usedBreathSamples
  );

  if (heartOk && breathOk) {
    Serial.println();
    Serial.println("========== FINAL STABLE ESTIMATE ==========");

    Serial.print("Estimated heart rate: ");
    Serial.print(estimatedHeart, 1);
    Serial.print(" bpm");
    Serial.print(" | robust spread: ±");
    Serial.print(heartSpread, 1);
    Serial.print(" | samples used: ");
    Serial.println(usedHeartSamples);

    Serial.print("Estimated breath rate: ");
    Serial.print(estimatedBreath, 1);
    Serial.print(" breaths/min");
    Serial.print(" | robust spread: ±");
    Serial.print(breathSpread, 1);
    Serial.print(" | samples used: ");
    Serial.println(usedBreathSamples);

    Serial.println("===========================================");
    Serial.println();

    finalEstimatePrinted = true;
  }
}


// ============================================================
//  15. MAIN FILTERING ARCHITECTURE
// ============================================================
//
// This is the central decision layer.
// It converts raw radar values into trusted filtered output.
// ============================================================

void updateFilteredOutput() {
  uint32_t now = millis();

  // ------------------------------------------------------------
  // A. PRESENCE EVIDENCE
  // ------------------------------------------------------------
  // Evidence can come from:
  // 1. human detected frame
  // 2. valid distance frame
  // ------------------------------------------------------------

  bool humanEvidence =
    lastHumanFrameMs > 0 &&
    now - lastHumanFrameMs <= FRAME_STALE_MS &&
    rawHumanDetected;

  bool distanceEvidence =
    lastDistanceFrameMs > 0 &&
    now - lastDistanceFrameMs <= FRAME_STALE_MS &&
    rawDistanceValid &&
    rawDistanceCm >= MIN_DISTANCE_CM &&
    rawDistanceCm <= MAX_DISTANCE_CM;

  bool presenceEvidence = humanEvidence || distanceEvidence;

  presenceGate.update(presenceEvidence, now);


  // ------------------------------------------------------------
  // B. DISTANCE FILTER
  // ------------------------------------------------------------
  // Distance must be valid, inside range, and stable.
  // ------------------------------------------------------------

  if (lastDistanceFrameMs > processedDistanceFrameMs) {
    processedDistanceFrameMs = lastDistanceFrameMs;

    if (rawDistanceValid) {
      distanceFilter.update(rawDistanceCm, lastDistanceFrameMs);
    }
  }

  distanceFilter.expire(now);


  // ------------------------------------------------------------
  // C. VITAL-SIGN FILTERS
  // ------------------------------------------------------------
  // Heart and breath are only processed when:
  // 1. presence is stable
  // 2. distance is stable
  // ------------------------------------------------------------

  if (!presenceGate.stablePresent || !distanceFilter.isStable()) {
    heartFilter.reset();
    breathFilter.reset();

    heartFinalEstimator.reset();
    breathFinalEstimator.reset();

    finalEstimatePrinted = false;
    lastEstimateSampleMs = 0;

    // Consume old vital frames so they are not accepted later.
    processedHeartFrameMs = lastHeartFrameMs;
    processedBreathFrameMs = lastBreathFrameMs;
  } else {
    if (lastHeartFrameMs > processedHeartFrameMs) {
      processedHeartFrameMs = lastHeartFrameMs;
      heartFilter.update(rawHeartBpm, lastHeartFrameMs);
    }

    if (lastBreathFrameMs > processedBreathFrameMs) {
      processedBreathFrameMs = lastBreathFrameMs;
      breathFilter.update(rawBreathRate, lastBreathFrameMs);
    }

    heartFilter.expire(now);
    breathFilter.expire(now);
  }


  // ------------------------------------------------------------
  // D. PRINT FILTERED OUTPUT ONCE PER SECOND
  // ------------------------------------------------------------

  static uint32_t lastOutputMs = 0;

  if (now - lastOutputMs < OUTPUT_EVERY_MS) {
    return;
  }

  lastOutputMs = now;

  if (!presenceGate.stablePresent) {
    if (presenceGate.warmingUp()) {
      Serial.println("Target candidate detected; waiting for stable presence...");
    } else {
      Serial.println("No stable target");
    }

    return;
  }

  if (!distanceFilter.isStable()) {
    Serial.println("Human candidate detected; waiting for stable distance...");
    return;
  }

  Serial.print("Stable target");

  Serial.print(" | Distance: ");
  Serial.print(distanceFilter.get(), 1);
  Serial.print(" cm");

  Serial.print(" | Breath: ");
  if (breathFilter.isStable()) {
    Serial.print(breathFilter.get() + BREATH_CALIBRATION_OFFSET, 1);
    Serial.print(" breaths/min");
  } else {
    Serial.print("validating");
  }

  Serial.print(" | Heart: ");
  if (heartFilter.isStable()) {
    Serial.print(heartFilter.get() + HEART_CALIBRATION_OFFSET_BPM, 1);
    Serial.print(" bpm");
  } else {
    Serial.print("validating");
  }

  Serial.println();

  // ------------------------------------------------------------
  // E. UPDATE FINAL ONE-MINUTE ESTIMATE
  // ------------------------------------------------------------
  // This must be called here, not inside loop(),
  // because this function has access to now.
  // ------------------------------------------------------------

  updateFinalEstimate(now);
}


// ============================================================
//  16. SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("MR60BHA2 + Raspberry Pi Pico 2 W filtered reader");

  // These functions are available in the Arduino-Pico core.
  // If your installed core does not support them, remove these lines
  // and use the default Serial1 pins.
  RADAR_SERIAL.setTX(RADAR_TX_PIN);
  RADAR_SERIAL.setRX(RADAR_RX_PIN);
  RADAR_SERIAL.setFIFOSize(1024);

  RADAR_SERIAL.begin(RADAR_BAUD);

  resetRadarIfConnected();

  Serial.print("Radar UART started at ");
  Serial.print(RADAR_BAUD);
  Serial.println(" baud");
}


// ============================================================
//  17. MAIN LOOP
// ============================================================
//
// Main flow:
// 1. read all available UART bytes
// 2. parse radar frames
// 3. update filters
// 4. warn if no valid frames arrive
// ============================================================

void loop() {
  while (RADAR_SERIAL.available() > 0) {
  parseRadarByte((uint8_t)RADAR_SERIAL.read());
}
  
while (RADAR_SERIAL.available() > 0) {
  parseRadarByte((uint8_t)RADAR_SERIAL.read());
}
  updateFilteredOutput();

  static uint32_t lastWarningMs = 0;

  if (
    millis() > 5000 &&
    lastValidFrameMs == 0 &&
    millis() - lastWarningMs > 3000
  ) {
    lastWarningMs = millis();

    Serial.println(
      "No valid radar frames yet. Check GND, TX/RX crossing, radar power, and baud rate."
    );
  }
}