Implemented the three enhancements:

1. **Cold-start protection**: EMA starts only after two stable raw readings.
2. **Jump-rejection deadlock fix**: after 3 consecutive rejected jumps, the EMA is re-initialized to the new position.
3. **Integer EMA**: uses alpha = 1/4 without floating-point math.

Copy-paste this full code:

```cpp
/*
  Waveshare HMMD mmWave Sensor + Raspberry Pi Pico 2W
  Arduino IDE

  Wiring:
    HMMD 3V3 -> Pico 3V3
    HMMD GND -> Pico GND
    HMMD TX  -> Pico GP5
    HMMD RX  -> Pico GP4
    HMMD OT2 -> Pico GP15

  Output:
    Pico GP16 -> wake/start input of another 3.3V system

  Detection condition:
    Person must be detected between 90 cm and 150 cm.
    Energy gate 0 is ignored completely.
    Distance is smoothed using integer EMA.

  Filtering:
    - Gate 0 ignored
    - Gate 1 and gate 2 used as supporting evidence
    - OT2 must be HIGH
    - UART presence must be detected
    - Raw distance must pass sanity limits
    - Cold start requires two stable raw readings
    - Sudden jumps are rejected
    - After 3 consecutive jumps, EMA is re-initialized to avoid deadlock
    - EMA alpha = 1/4
    - Hysteresis and consecutive-frame confirmation are applied
*/

#define RADAR_RX_PIN 5
#define RADAR_TX_PIN 4
#define OT2_PIN      15
#define WAKE_OUT_PIN 16

#define RadarSerial Serial2

// Main activation distance window
const uint16_t ENTER_MIN_CM = 90;
const uint16_t ENTER_MAX_CM = 150;

// Hysteresis window.
// Once active, it remains active while still inside this wider range.
const uint16_t EXIT_MIN_CM = 80;
const uint16_t EXIT_MAX_CM = 170;

// Basic sanity limits for raw distance
const uint16_t ABSOLUTE_MIN_CM = 30;
const uint16_t ABSOLUTE_MAX_CM = 300;

// Maximum allowed jump between accepted raw samples
const uint16_t MAX_DISTANCE_JUMP_CM = 50;

// Cold start requires two raw readings within this jump limit
const uint8_t REQUIRED_COLD_START_SAMPLES = 2;

// If this many jumps happen in a row, assume the target moved quickly
// and re-initialize the EMA at the new distance.
const uint8_t MAX_CONSECUTIVE_REJECTED_JUMPS = 3;

// EMA alpha = 1/4
// Integer formula:
// ema = (newDistance + 3 * previousEma) / 4
const uint8_t EMA_DIVISOR = 4;

// 3 frames = about 300 ms confirmation
const uint8_t REQUIRED_HITS = 3;

// 6 frames = about 600 ms before switching off
const uint8_t REQUIRED_MISSES = 6;

// EMA state
bool emaReady = false;
uint16_t emaDistanceCm = 0;
uint16_t lastAcceptedDistanceCm = 0;

// Cold-start state
bool coldStartHasFirstSample = false;
uint16_t coldStartFirstSampleCm = 0;

// Jump rejection state
uint8_t rejectedJumpCount = 0;

// Final decision state
bool personInWindow = false;
uint8_t hitCount = 0;
uint8_t missCount = 0;

unsigned long lastPrintMs = 0;

// Filter status for debugging
enum FilterStatus {
  FILTER_NOT_READY,
  FILTER_COLD_START_WAITING,
  FILTER_ACCEPTED,
  FILTER_JUMP_REJECTED,
  FILTER_FORCED_REINIT,
  FILTER_RESET_INVALID
};

FilterStatus lastFilterStatus = FILTER_NOT_READY;

// Command: set HMMD to report mode
const uint8_t CMD_REPORT_MODE[] = {
  0xFD, 0xFC, 0xFB, 0xFA,
  0x08, 0x00,
  0x12, 0x00,
  0x00, 0x00,
  0x04, 0x00, 0x00, 0x00,
  0x04, 0x03, 0x02, 0x01
};

const uint8_t REPORT_HEADER[4] = {0xF4, 0xF3, 0xF2, 0xF1};
const uint8_t REPORT_TAIL[4]   = {0xF8, 0xF7, 0xF6, 0xF5};

uint8_t frame[96];
uint16_t framePos = 0;
uint16_t expectedFrameLen = 0;

// Function declarations
void processByte(uint8_t b);
bool validTail();
void parseReportFrame();
int findStrongestGateIgnoringGate0(uint16_t energies[16]);
bool updateEmaDistance(uint16_t newDistanceCm);
void resetEmaDistance();
uint16_t absDiffCm(uint16_t a, uint16_t b);
void updateDecision(
  uint8_t radarDetected,
  bool ot2High,
  bool validDistance,
  bool usefulGate,
  uint16_t filteredDistanceCm
);
void printDebug(
  uint8_t radarDetected,
  bool ot2High,
  uint16_t rawDistanceCm,
  uint16_t filteredDistanceCm,
  int strongestGate,
  uint16_t energies[16]
);
const char* filterStatusToText(FilterStatus status);
void resetFrame();

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("HMMD 90-150cm detector with enhanced integer EMA starting...");

  pinMode(OT2_PIN, INPUT);
  pinMode(WAKE_OUT_PIN, OUTPUT);
  digitalWrite(WAKE_OUT_PIN, LOW);

  RadarSerial.setRX(RADAR_RX_PIN);
  RadarSerial.setTX(RADAR_TX_PIN);
  RadarSerial.setFIFOSize(256);
  RadarSerial.begin(115200);

  delay(500);

  Serial.println("Switching HMMD to report mode...");
  RadarSerial.write(CMD_REPORT_MODE, sizeof(CMD_REPORT_MODE));

  Serial.println("Ready.");
}

void loop() {
  while (RadarSerial.available()) {
    uint8_t b = RadarSerial.read();
    processByte(b);
  }
}

void processByte(uint8_t b) {
  // Find report header: F4 F3 F2 F1
  if (framePos < 4) {
    if (b == REPORT_HEADER[framePos]) {
      frame[framePos++] = b;
    } else {
      framePos = (b == REPORT_HEADER[0]) ? 1 : 0;
      if (framePos == 1) {
        frame[0] = b;
      }
    }
    return;
  }

  if (framePos >= sizeof(frame)) {
    resetFrame();
    return;
  }

  frame[framePos++] = b;

  // After header + length bytes
  if (framePos == 6) {
    uint16_t payloadLen = frame[4] | (frame[5] << 8);
    expectedFrameLen = 4 + 2 + payloadLen + 4;

    if (expectedFrameLen > sizeof(frame) || payloadLen < 35) {
      resetFrame();
      return;
    }
  }

  if (expectedFrameLen > 0 && framePos >= expectedFrameLen) {
    if (validTail()) {
      parseReportFrame();
    }
    resetFrame();
  }
}

bool validTail() {
  return frame[expectedFrameLen - 4] == REPORT_TAIL[0] &&
         frame[expectedFrameLen - 3] == REPORT_TAIL[1] &&
         frame[expectedFrameLen - 2] == REPORT_TAIL[2] &&
         frame[expectedFrameLen - 1] == REPORT_TAIL[3];
}

void parseReportFrame() {
  uint16_t payloadLen = frame[4] | (frame[5] << 8);
  if (payloadLen < 35) {
    return;
  }

  uint8_t radarDetected = frame[6];
  uint16_t rawDistanceCm = frame[7] | (frame[8] << 8);

  uint16_t energies[16];

  for (int i = 0; i < 16; i++) {
    energies[i] = frame[9 + i * 2] | (frame[10 + i * 2] << 8);
  }

  bool ot2High = digitalRead(OT2_PIN) == HIGH;

  // Find strongest gate, ignoring noisy gate 0.
  int strongestGate = findStrongestGateIgnoringGate0(energies);

  // For 90-150 cm, useful gates are:
  // gate 1: approximately 70-140 cm
  // gate 2: approximately 140-210 cm
  bool usefulGate = strongestGate == 1 || strongestGate == 2;

  bool validDistance =
    rawDistanceCm >= ABSOLUTE_MIN_CM &&
    rawDistanceCm <= ABSOLUTE_MAX_CM;

  uint16_t filteredDistanceCm = 0;

  if (radarDetected == 1 && ot2High && validDistance && usefulGate) {
    bool filterHasOutput = updateEmaDistance(rawDistanceCm);

    if (filterHasOutput) {
      filteredDistanceCm = emaDistanceCm;
    } else {
      filteredDistanceCm = 0;
    }
  } else {
    resetEmaDistance();
  }

  updateDecision(
    radarDetected,
    ot2High,
    validDistance,
    usefulGate,
    filteredDistanceCm
  );

  printDebug(
    radarDetected,
    ot2High,
    rawDistanceCm,
    filteredDistanceCm,
    strongestGate,
    energies
  );
}

int findStrongestGateIgnoringGate0(uint16_t energies[16]) {
  int strongestGate = 1;
  uint16_t strongestEnergy = energies[1];

  // Start at 2 because gate 1 is already used as the initial value.
  // Gate 0 is ignored completely.
  for (int i = 2; i < 16; i++) {
    if (energies[i] > strongestEnergy) {
      strongestEnergy = energies[i];
      strongestGate = i;
    }
  }

  return strongestGate;
}

bool updateEmaDistance(uint16_t newDistanceCm) {
  /*
    Cold-start protection:
    The first valid raw reading is not immediately trusted.
    The EMA becomes ready only when two consecutive raw readings
    are within MAX_DISTANCE_JUMP_CM of each other.
  */
  if (!emaReady) {
    rejectedJumpCount = 0;

    if (!coldStartHasFirstSample) {
      coldStartFirstSampleCm = newDistanceCm;
      coldStartHasFirstSample = true;
      lastFilterStatus = FILTER_COLD_START_WAITING;
      return false;
    }

    uint16_t difference = absDiffCm(newDistanceCm, coldStartFirstSampleCm);

    if (difference <= MAX_DISTANCE_JUMP_CM) {
      // Initialize EMA with the average of the two stable startup samples.
      emaDistanceCm = (coldStartFirstSampleCm + newDistanceCm + 1) / 2;
      lastAcceptedDistanceCm = newDistanceCm;

      emaReady = true;
      coldStartHasFirstSample = false;
      rejectedJumpCount = 0;

      lastFilterStatus = FILTER_ACCEPTED;
      return true;
    } else {
      // First startup sample was probably noisy.
      // Replace it and wait for the next stable reading.
      coldStartFirstSampleCm = newDistanceCm;
      lastFilterStatus = FILTER_COLD_START_WAITING;
      return false;
    }
  }

  /*
    Jump rejection:
    Normally reject a sudden large jump.
    However, if several large jumps happen consecutively,
    the target probably moved quickly. In that case, reset the EMA
    to the new position to avoid deadlock.
  */
  uint16_t difference = absDiffCm(newDistanceCm, lastAcceptedDistanceCm);

  if (difference > MAX_DISTANCE_JUMP_CM) {
    rejectedJumpCount++;

    if (rejectedJumpCount >= MAX_CONSECUTIVE_REJECTED_JUMPS) {
      // Forced re-initialization after repeated rejected jumps.
      emaDistanceCm = newDistanceCm;
      lastAcceptedDistanceCm = newDistanceCm;
      rejectedJumpCount = 0;

      lastFilterStatus = FILTER_FORCED_REINIT;
      return true;
    }

    lastFilterStatus = FILTER_JUMP_REJECTED;
    return false;
  }

  // Valid movement: reset jump rejection counter.
  rejectedJumpCount = 0;

  /*
    Integer EMA with alpha = 1/4:

    ema = 0.25 * newDistance + 0.75 * previousEma

    Integer form:
    ema = (newDistance + 3 * previousEma) / 4
  */
  uint32_t emaCalc =
    (uint32_t)newDistanceCm +
    (uint32_t)3 * emaDistanceCm;

  // +2 gives simple rounding before division by 4.
  emaDistanceCm = (uint16_t)((emaCalc + 2) / EMA_DIVISOR);

  lastAcceptedDistanceCm = newDistanceCm;

  lastFilterStatus = FILTER_ACCEPTED;
  return true;
}

void resetEmaDistance() {
  emaReady = false;
  emaDistanceCm = 0;
  lastAcceptedDistanceCm = 0;

  coldStartHasFirstSample = false;
  coldStartFirstSampleCm = 0;

  rejectedJumpCount = 0;

  lastFilterStatus = FILTER_RESET_INVALID;
}

uint16_t absDiffCm(uint16_t a, uint16_t b) {
  if (a > b) {
    return a - b;
  } else {
    return b - a;
  }
}

void updateDecision(
  uint8_t radarDetected,
  bool ot2High,
  bool validDistance,
  bool usefulGate,
  uint16_t filteredDistanceCm
) {
  bool candidateInWindow =
    emaReady &&
    filteredDistanceCm > 0 &&
    radarDetected == 1 &&
    ot2High &&
    validDistance &&
    usefulGate &&
    filteredDistanceCm >= ENTER_MIN_CM &&
    filteredDistanceCm <= ENTER_MAX_CM;

  bool stillInWiderWindow =
    emaReady &&
    filteredDistanceCm > 0 &&
    radarDetected == 1 &&
    ot2High &&
    validDistance &&
    usefulGate &&
    filteredDistanceCm >= EXIT_MIN_CM &&
    filteredDistanceCm <= EXIT_MAX_CM;

  if (!personInWindow) {
    if (candidateInWindow) {
      hitCount++;
      missCount = 0;

      if (hitCount >= REQUIRED_HITS) {
        personInWindow = true;
        hitCount = 0;
        missCount = 0;

        digitalWrite(WAKE_OUT_PIN, HIGH);
        Serial.println(">>> PERSON BETWEEN 90-150 CM: WAKE OUTPUT HIGH");
      }
    } else {
      hitCount = 0;
    }
  } else {
    if (stillInWiderWindow) {
      missCount = 0;
    } else {
      missCount++;

      if (missCount >= REQUIRED_MISSES) {
        personInWindow = false;
        hitCount = 0;
        missCount = 0;

        digitalWrite(WAKE_OUT_PIN, LOW);
        Serial.println(">>> PERSON LEFT WINDOW: WAKE OUTPUT LOW");
      }
    }
  }
}

void printDebug(
  uint8_t radarDetected,
  bool ot2High,
  uint16_t rawDistanceCm,
  uint16_t filteredDistanceCm,
  int strongestGate,
  uint16_t energies[16]
) {
  if (millis() - lastPrintMs < 500) {
    return;
  }

  lastPrintMs = millis();

  Serial.println("----------------------------");

  Serial.print("OT2 GP15: ");
  Serial.println(ot2High ? "HIGH" : "LOW");

  Serial.print("UART presence: ");
  Serial.println(radarDetected == 1 ? "PRESENT" : "ABSENT");

  Serial.print("Raw distance: ");
  Serial.print(rawDistanceCm);
  Serial.println(" cm");

  Serial.print("Filter status: ");
  Serial.println(filterStatusToText(lastFilterStatus));

  Serial.print("EMA ready: ");
  Serial.println(emaReady ? "YES" : "NO");

  Serial.print("EMA distance: ");
  if (emaReady && filteredDistanceCm > 0) {
    Serial.print(filteredDistanceCm);
    Serial.println(" cm");
  } else {
    Serial.println("no valid output yet");
  }

  Serial.print("Rejected jump count: ");
  Serial.println(rejectedJumpCount);

  Serial.print("Strongest useful gate, ignoring gate 0: ");
  Serial.print(strongestGate);
  Serial.print(" approx ");
  Serial.print(strongestGate * 70);
  Serial.print("-");
  Serial.print((strongestGate + 1) * 70);
  Serial.println(" cm");

  Serial.print("Gate 0 energy ignored: ");
  Serial.println(energies[0]);

  Serial.print("Gate 1 energy: ");
  Serial.println(energies[1]);

  Serial.print("Gate 2 energy: ");
  Serial.println(energies[2]);

  Serial.print("Decision 90-150 cm: ");
  Serial.println(personInWindow ? "YES" : "NO");

  Serial.print("Wake output GP16: ");
  Serial.println(digitalRead(WAKE_OUT_PIN) == HIGH ? "HIGH" : "LOW");
}

const char* filterStatusToText(FilterStatus status) {
  switch (status) {
    case FILTER_NOT_READY:
      return "not ready";

    case FILTER_COLD_START_WAITING:
      return "cold start waiting for stable second sample";

    case FILTER_ACCEPTED:
      return "accepted";

    case FILTER_JUMP_REJECTED:
      return "jump rejected";

    case FILTER_FORCED_REINIT:
      return "forced EMA re-initialization after repeated jumps";

    case FILTER_RESET_INVALID:
      return "reset because frame was invalid or presence lost";

    default:
      return "unknown";
  }
}

void resetFrame() {
  framePos = 0;
  expectedFrameLen = 0;
}
```
