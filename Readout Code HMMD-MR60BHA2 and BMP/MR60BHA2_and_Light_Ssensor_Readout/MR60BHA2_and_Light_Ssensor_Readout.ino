#include <Arduino.h>
#include <Wire.h>

// Pico 2 W UART pins
static const int RADAR_TX_PIN = 6;   // Pico GP0 -> Radar RX0
static const int RADAR_RX_PIN = 7;   // Pico GP1 <- Radar TX0

#define PIN_I2C_SDA    4   // Pico GP4  -> breakout SDA
#define PIN_I2C_SCL    5  // Pico GP5  -> breakout SCL

#define BH1750_ADDR    0x23
// Optional reset pin. Leave as -1 if not connected.
static const int RADAR_RST_PIN = -1;


//---------------
//BH1750
bool bh1750Write(uint8_t cmd) {
  Wire.beginTransmission(BH1750_ADDR);
  Wire.write(cmd);
  return Wire.endTransmission() == 0;
}

bool bh1750Begin() {
  if (!bh1750Write(0x01)) return false;  // Power on
  delay(10);
  if (!bh1750Write(0x10)) return false;  // Continuous high-resolution mode
  delay(180);
  return true;
}

bool readLux(float &lux) {
  Wire.requestFrom(BH1750_ADDR, (uint8_t)2);

  if (Wire.available() < 2) {
    return false;
  }

  uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
  lux = raw / 1.2;   // BH1750 standard conversion
  return true;
}
//------------


// Seeed wiki examples use 115200.
// If you get no valid frames, try 1382400 because the module datasheet also mentions that baud.
static const uint32_t RADAR_BAUD = 115200;

// Set true if you also want continuous phase data printed.
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

    case 0x0A14: { // breathing rate
      if (len >= 4) {
        float breathRate = readFloatLE(data);
        Serial.print("Breath rate: ");
        Serial.print(breathRate, 2);
        Serial.println(" breaths/min");
      }
      break;
    }

    case 0x0A15: { // heart rate
      if (len >= 4) {
        float heartRate = readFloatLE(data);
        Serial.print("Heart rate: ");
        Serial.print(heartRate, 2);
        Serial.println(" bpm");
      }
      break;
    }

    case 0x0A16: { // detection distance
      if (len >= 8) {
        uint32_t flag = readLE32(data);
        float distance = readFloatLE(data + 4);

        if (flag == 1) {
          Serial.print("Distance: ");
          Serial.print(distance, 2);
          Serial.println(" cm");
        } else {
          Serial.println("Distance: no valid target");
        }
      }
      break;
    }

    case 0x0F09: { // human detected, available on newer firmware
      if (len >= 1) {
        Serial.print("Human detected: ");
        Serial.println(data[0] ? "YES" : "NO");
      }
      break;
    }

    default:
      // Uncomment for debugging unknown frames
      // Serial.print("Unknown frame type: 0x");
      // Serial.println(type, HEX);
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
  Serial.println("MR60BHA2 + Raspberry Pi Pico 2 W reader + BH1750");

  // These functions are available in the Arduino-Pico core.
  // If your installed core does not support them, remove these three lines
  // and keep using GP0/GP1 as the default Serial1 pins.
  Serial1.setTX(RADAR_TX_PIN);
  Serial1.setRX(RADAR_RX_PIN);
  Serial1.setFIFOSize(1024);

  Serial1.begin(RADAR_BAUD);

  resetRadarIfConnected();

  // I2C for BH1750
  Wire.setSDA(PIN_I2C_SDA);
  Wire.setSCL(PIN_I2C_SCL);
  Wire.begin();

  if (bh1750Begin()) {
    Serial.println("BH1750 light sensor found.");
  } else {
    Serial.println("BH1750 not found. Check SDA/SCL and 3.3V power on breakout.");
  }


  Serial.print("Radar UART started at ");
  Serial.print(RADAR_BAUD);
  Serial.println(" baud");
}

void loop() {
  while (Serial1.available() > 0) {
    parseRadarByte((uint8_t)Serial1.read());
  }

  static uint32_t lastWarningMs = 0;
  if (millis() > 5000 && lastValidFrameMs == 0 && millis() - lastWarningMs > 3000) {
    lastWarningMs = millis();
    Serial.println("No valid radar frames yet. Check GND, TX/RX crossing, radar power, and baud rate.");
  }

static uint32_t lastLuxMs = 0;
  if (millis() - lastLuxMs >= 1000) {
    lastLuxMs = millis();

    float lux;
    if (readLux(lux)) {
      Serial.print("Light: ");
      Serial.print(lux, 1);
      Serial.println(" lux");
    } else {
      Serial.println("Light: read failed");
    }

}
}