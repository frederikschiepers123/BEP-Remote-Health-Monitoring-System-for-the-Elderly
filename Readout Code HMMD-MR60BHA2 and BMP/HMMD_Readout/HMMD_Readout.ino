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
    Distance is smoothed using EMA.
*/

#define RADAR_RX_PIN 5
#define RADAR_TX_PIN 4
#define OT2_PIN      15
#define WAKE_OUT_PIN 16

#define RadarSerial Serial2

// Main accepted distance window
const uint16_t ENTER_MIN_CM = 90;
const uint16_t ENTER_MAX_CM = 150;

// Hysteresis window.
// Once active, it stays active while still roughly inside this wider range.
const uint16_t EXIT_MIN_CM = 80;
const uint16_t EXIT_MAX_CM = 170;

// Basic sanity limits
const uint16_t ABSOLUTE_MIN_CM = 30;
const uint16_t ABSOLUTE_MAX_CM = 300;

// EMA smoothing.
// Lower = smoother but slower.
// Higher = faster but more jumpy.
const float EMA_ALPHA = 0.25;

// Reject sudden unrealistic jumps
const uint16_t MAX_DISTANCE_JUMP_CM = 50;

// 3 frames = about 300 ms confirmation
const uint8_t REQUIRED_HITS = 3;

// 6 frames = about 600 ms before switching off
const uint8_t REQUIRED_MISSES = 6;

bool emaReady = false;
float emaDistanceCm = 0;
uint16_t lastAcceptedDistanceCm = 0;

bool personInWindow = false;
uint8_t hitCount = 0;
uint8_t missCount = 0;

unsigned long lastPrintMs = 0;

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
void resetFrame();

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("HMMD 90-150cm EMA detector starting...");

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

  // Find strongest gate, but ignore gate 0 completely.
  int strongestGate = findStrongestGateIgnoringGate0(energies);

  // For 90-150 cm, the useful gates are:
  // gate 1: about 70-140 cm
  // gate 2: about 140-210 cm
  bool usefulGate = strongestGate == 1 || strongestGate == 2;

  bool validDistance =
    rawDistanceCm >= ABSOLUTE_MIN_CM &&
    rawDistanceCm <= ABSOLUTE_MAX_CM;

  uint16_t filteredDistanceCm = 0;

  if (radarDetected == 1 && ot2High && validDistance && usefulGate) {
    bool accepted = updateEmaDistance(rawDistanceCm);

    if (accepted) {
      filteredDistanceCm = (uint16_t)(emaDistanceCm + 0.5);
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

  // Start from 1, not 0.
  // Gate 0 is ignored because it is noisy.
  for (int i = 2; i < 16; i++) {
    if (energies[i] > strongestEnergy) {
      strongestEnergy = energies[i];
      strongestGate = i;
    }
  }

  return strongestGate;
}
// exponential smoothing
bool updateEmaDistance(uint16_t newDistanceCm) {
  if (!emaReady) {
    emaDistanceCm = newDistanceCm;
    lastAcceptedDistanceCm = newDistanceCm;
    emaReady = true;
    return true;
  }

  uint16_t difference;

  if (newDistanceCm > lastAcceptedDistanceCm) {
    difference = newDistanceCm - lastAcceptedDistanceCm;
  } else {
    difference = lastAcceptedDistanceCm - newDistanceCm;
  }

  // Reject sudden unrealistic jumps
  if (difference > MAX_DISTANCE_JUMP_CM) {
    return false;
  }

static uint8_t rejectedJumpCount = 0;

if (difference > MAX_DISTANCE_JUMP_CM) {
  rejectedJumpCount++;
  // If we get 3 consecutive "jumps", the target likely moved fast.
  if (rejectedJumpCount >= 3) { 
    emaDistanceCm = newDistanceCm;
    lastAcceptedDistanceCm = newDistanceCm;
    rejectedJumpCount = 0;
    return true; 
  }
  return false;
}

rejectedJumpCount = 0; // Reset counter on valid movement


  emaDistanceCm =
    EMA_ALPHA * newDistanceCm +
    (1.0 - EMA_ALPHA) * emaDistanceCm;

  lastAcceptedDistanceCm = newDistanceCm;

  return true;
}

void resetEmaDistance() {
  emaReady = false;
  emaDistanceCm = 0;
  lastAcceptedDistanceCm = 0;
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

  Serial.print("EMA distance: ");
  if (emaReady && filteredDistanceCm > 0) {
    Serial.print(filteredDistanceCm);
    Serial.println(" cm");
  } else if (emaReady && filteredDistanceCm == 0) {
    Serial.println("jump rejected");
  } else {
    Serial.println("not ready");
  }

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

void resetFrame() {
  framePos = 0;
  expectedFrameLen = 0;
}