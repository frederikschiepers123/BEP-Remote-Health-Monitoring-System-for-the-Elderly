#include <Wire.h>
#include <Adafruit_BMP280.h>

// Your PCB I2C pins
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9

// Your detected BMP280 address
#define BMP280_I2C_ADDRESS 0x76

Adafruit_BMP280 bmp;  // Uses Wire / I2C0

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("BMP280 readout on Raspberry Pi Pico 2 W");

  Wire.setSDA(I2C_SDA_PIN);
  Wire.setSCL(I2C_SCL_PIN);
  Wire.begin();

  if (!bmp.begin(BMP280_I2C_ADDRESS)) {
    Serial.println("Could not find BMP280 sensor!");
    Serial.println("Check wiring, address, and I2C pins.");
    while (1) {
      delay(10);
    }
  }

  Serial.println("BMP280 found successfully!");

  bmp.setSampling(
    Adafruit_BMP280::MODE_NORMAL,
    Adafruit_BMP280::SAMPLING_X2,   // temperature
    Adafruit_BMP280::SAMPLING_X16,  // pressure
    Adafruit_BMP280::FILTER_X16,
    Adafruit_BMP280::STANDBY_MS_500
  );
}

void loop() {
  float temperature = bmp.readTemperature();
  float pressure = bmp.readPressure() / 100.0F; // hPa

  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.println(" °C");

  Serial.print("Pressure: ");
  Serial.print(pressure);
  Serial.println(" hPa");

  Serial.println("----------------------");

  delay(2000);
}