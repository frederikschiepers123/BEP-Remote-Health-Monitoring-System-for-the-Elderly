#include <Arduino.h>
#include <Wire.h>

// Pico 2 W UART pins
static const int RADAR_TX_PIN = 0;   // Pico GP0 -> Radar RX0
static const int RADAR_RX_PIN = 1;   // Pico GP1 <- Radar TX0



// Optional reset pin. Leave as -1 if not connected.
static const int RADAR_RST_PIN = -1;

// --- 5-Second Validation Variables ---
static uint32_t distWindowStart = 0;
static float distSum = 0.0;
static int distCount = 0;
// -------------------------------------



static const uint32_t RADAR_BAUD = 115200;
static const bool PRINT_PHASES = false;

static const uint8_t SOF = 0x01;
static const size_t HEADER_LEN = 8;
static const size_t MAX_DATA_LEN = 64;

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

void resetRadarIfConnected() {
  if (RADAR_RST_PIN < 0) return;

  pinMode(RADAR_RST_PIN, OUTPUT);
  digitalWrite(RADAR_RST_PIN, LOW);
  delay(100);
  digitalWrite(RADAR_RST_PIN, HIGH);
  delay(1000);
}

void handleFrame(uint16_t type, const uint8_t *data, uint16_t len) {
  lastValidFrameMs = millis();

  switch (type) {
    case 0x0A13: { // total phase, breath phase, heart phase
      if (len >= 12) {
        float totalPhase  = readFloatLE(data + 0);
        float breathPhase = readFloatLE(data + 4);
        float heartPhase  = readFloatLE(data + 8);

        if (PRINT_PHASES) {
          Serial.print("Phase | total: ");
          Serial.print(totalPhase, 3);
          Serial.print("  breath: ");
          Serial.print(breathPhase, 3);
          Serial.print("  heart: ");
          Serial.println(heartPhase, 3);
        }
      }
      break;
    }

    case 0x0A16: { // detection distance
      if (len >= 8) {
        uint32_t flag = readLE32(data);
        float distance = readFloatLE(data + 4);

        // Algorithm Step 1 & 2: Accumulate valid distances over 0 cm
        if (flag == 1 && distance > 0.0) {
          distSum += distance;
          distCount++;
        }
      }
      break;
    }

    default:
      break;
  }
}

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
      headerBuf[headerIndex++] = b;

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
        dataBuf[dataIndex++] = b;
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

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("MR60BHA2 + Raspberry Pi Pico 2 W reader");

  Serial1.setTX(RADAR_TX_PIN);
  Serial1.setRX(RADAR_RX_PIN);
  Serial1.setFIFOSize(1024);
  Serial1.begin(RADAR_BAUD);

  resetRadarIfConnected();



  Serial.print("Radar UART started at ");
  Serial.println(RADAR_BAUD);

  // Initialize the validation timer
  distWindowStart = millis();
}

void loop() {
  while (Serial1.available() > 0) {
    parseRadarByte((uint8_t)Serial1.read());
  }

  // --- Algorithm Step 3 & 4: Calculate and Print 5-Second Average ---
  if (millis() - distWindowStart >= 5000) {
    Serial.println("--------------------------------------------------");
    if (distCount > 0) {
      float avgDistance = distSum / distCount;
      Serial.print("[VALIDATION] 5-Sec Stable Distance: ");
      Serial.print(avgDistance, 2);
      Serial.print(" cm (Calculated from ");
      Serial.print(distCount);
      Serial.println(" frames)");
    } else {
      Serial.println("[VALIDATION] 5-Sec Stable Distance: NO VALID TARGET");
    }
    Serial.println("--------------------------------------------------");
    
    // Reset for the next 5-second window
    distSum = 0.0;
    distCount = 0;
    distWindowStart = millis();
  }

  static uint32_t lastWarningMs = 0;
  if (millis() > 5000 && lastValidFrameMs == 0 && millis() - lastWarningMs > 3000) {
    lastWarningMs = millis();
    Serial.println("No valid radar frames yet. Check wiring.");
  }

  static uint32_t lastLuxMs = 0;
  if (millis() - lastLuxMs >= 1000) {
    lastLuxMs = millis();

  }
}