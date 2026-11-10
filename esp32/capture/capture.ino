/*
 * ESP32-S3 + MAX30102 + MPU6050 -> streaming CSV por Serial (USB)
 *
 * Librerías (Arduino Library Manager):
 *   - "SparkFun MAX3010x Pulse and Proximity Sensor Library"  (sirve para MAX30102)
 *   - "Adafruit MPU6050"  (instala también Adafruit Unified Sensor y BusIO)
 *
 * Conexión (ambos sensores en el MISMO bus I2C, no chocan las direcciones):
 *   MAX30102  -> 0x57      MPU6050 -> 0x68 (AD0 a GND)
 *   VIN -> 3V3   GND -> GND   SDA -> GPIO 8   SCL -> GPIO 9
 *
 * Protocolo con la PC:
 *   PC envía 's'  -> empieza a transmitir datos
 *   PC envía 'x'  -> detiene la transmisión
 *   Líneas que empiezan con '#' son mensajes de estado, no datos.
 *
 * Formato de cada fila (100 Hz, marcados por el MAX30102):
 *   seq,t_ms,red,ir,ax,ay,az,gx,gy,gz
 *   ax..az en m/s^2, gx..gz en rad/s
 */

#include <Wire.h>
#include "MAX30105.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

#define I2C_SDA 8
#define I2C_SCL 9
#define BAUD    921600

MAX30105 ppg;
Adafruit_MPU6050 mpu;

bool streaming = false;
uint32_t seq = 0;

void detenerConError(const char *msg) {
  while (true) {
    Serial.println(msg);
    delay(2000);
  }
}

void setup() {
  Serial.begin(BAUD);
  while (!Serial && millis() < 3000) delay(10);

  // setPins antes de begin(): las librerías llaman Wire.begin() internamente
  // y así respetan nuestros pines.
  Wire.setPins(I2C_SDA, I2C_SCL);
  Wire.begin();
  Wire.setClock(400000);

  // ---------- MAX30102 ----------
  if (!ppg.begin(Wire, I2C_SPEED_FAST)) {
    detenerConError("#ERR MAX30102 no encontrado (revisa SDA/SCL/3V3)");
  }
  // setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange)
  //   400 sps con promedio de 4  -> 100 muestras/s efectivas
  //   ledMode 2 = Rojo + IR (necesario para SpO2)
  //   pulseWidth 411 us -> 18 bits de resolución
  ppg.setup(0x3F, 4, 2, 400, 411, 16384);

  // ---------- MPU6050 ----------
  if (!mpu.begin(0x68, &Wire)) {
    detenerConError("#ERR MPU6050 no encontrado (revisa AD0 y conexiones)");
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);  // filtro interno anti-ruido

  Serial.println("#READY");
}

void procesarComandos() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 's') {
      ppg.clearFIFO();        // descarta muestras viejas
      seq = 0;
      streaming = true;
      Serial.println("#START");
      Serial.println("#COLS seq,t_ms,red,ir,ax,ay,az,gx,gy,gz");
    } else if (c == 'x') {
      streaming = false;
      Serial.println("#STOP");
    }
  }
}

void loop() {
  procesarComandos();
  if (!streaming) return;

  // El MAX30102 es el "reloj" del muestreo: por cada muestra PPG nueva
  // leemos el MPU6050 y emitimos una fila. Así todo queda alineado a 100 Hz.
  ppg.check();
  while (ppg.available()) {
    uint32_t red = ppg.getFIFORed();
    uint32_t ir  = ppg.getFIFOIR();
    ppg.nextSample();

    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    Serial.printf("%lu,%lu,%lu,%lu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                  (unsigned long)seq++, (unsigned long)millis(),
                  (unsigned long)red, (unsigned long)ir,
                  a.acceleration.x, a.acceleration.y, a.acceleration.z,
                  g.gyro.x, g.gyro.y, g.gyro.z);
  }
}
