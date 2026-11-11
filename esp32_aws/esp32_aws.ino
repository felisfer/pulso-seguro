/*
 * ESP32-S3 + MAX30102 + MPU6050 -> AWS IoT Core (MQTT/TLS, lotes CSV)
 * Librerias: PubSubClient, SparkFun MAX3010x, Adafruit MPU6050
 *
 * Cada lote publicado en bio/<thing>/data:
 *   #<sesion>,<label>,<sujeto>
 *   seq,t_ms,red,ir,ax,ay,az,gx,gy,gz
 *   ...
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "MAX30105.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "secrets.h"

#define I2C_SDA      8
#define I2C_SCL      9
#define LIMITE_LOTE  24500  // bytes: < 25 KB = 5 bloques de 5 KB, ~3 s de datos
#define MAX_FILA     120    // bytes maximos de una fila
#define TAM_SLOT     (LIMITE_LOTE + MAX_FILA)

const char *TOPIC_DATA   = "bio/" THING_NAME "/data";
const char *TOPIC_CMD    = "bio/" THING_NAME "/cmd";
const char *TOPIC_STATUS = "bio/" THING_NAME "/status";

WiFiClientSecure net;
PubSubClient mqtt(net);
MAX30105 ppg;
Adafruit_MPU6050 mpu;

bool streaming = false;
uint32_t seq = 0;
char encabezado[96] = "#sin_sesion,sin_label,sin_sujeto";
char lote[TAM_SLOT];
size_t largo = 0;
uint32_t ultimoIntento = 0;

// ---------- cola circular de lotes pendientes (PSRAM si existe) ----------
char *cola = nullptr;
uint16_t *colaLargo = nullptr;
int capacidad = 0, colaIni = 0, colaN = 0;

void crearCola() {
  capacidad = 40;                                   // ~2 min de datos (~1 MB)
  cola = (char *)ps_malloc(capacidad * TAM_SLOT);
  if (!cola) {                                      // sin PSRAM: cola chica en RAM
    capacidad = 2;
    cola = (char *)malloc(capacidad * TAM_SLOT);
  }
  colaLargo = (uint16_t *)malloc(capacidad * sizeof(uint16_t));
  Serial.printf("Cola de %d lotes\n", capacidad);
}

void encolar(const char *datos, size_t n) {
  if (colaN == capacidad) {                         // llena: descarta el mas viejo
    colaIni = (colaIni + 1) % capacidad;
    colaN--;
  }
  int pos = (colaIni + colaN) % capacidad;
  memcpy(cola + pos * TAM_SLOT, datos, n);
  colaLargo[pos] = n;
  colaN++;
}

void vaciarCola() {                                 // max. 2 lotes por vuelta
  for (int k = 0; k < 2 && colaN > 0 && mqtt.connected(); k++) {
    const uint8_t *p = (const uint8_t *)(cola + colaIni * TAM_SLOT);
    if (!mqtt.publish(TOPIC_DATA, p, colaLargo[colaIni], false)) return;
    colaIni = (colaIni + 1) % capacidad;
    colaN--;
  }
}

// ---------- lotes ----------
void enviarLote() {
  if (largo == 0) return;
  bool ok = mqtt.connected() && colaN == 0 &&
            mqtt.publish(TOPIC_DATA, (const uint8_t *)lote, largo, false);
  if (!ok) encolar(lote, largo);                    // conserva el orden
  largo = 0;
}

void agregarFila(uint32_t red, uint32_t ir, sensors_event_t &a, sensors_event_t &g) {
  if (largo == 0) largo = snprintf(lote, sizeof(lote), "%s\n", encabezado);
  largo += snprintf(lote + largo, sizeof(lote) - largo,
                    "%lu,%lu,%lu,%lu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                    (unsigned long)seq++, (unsigned long)millis(),
                    (unsigned long)red, (unsigned long)ir,
                    a.acceleration.x, a.acceleration.y, a.acceleration.z,
                    g.gyro.x, g.gyro.y, g.gyro.z);
  if (largo > LIMITE_LOTE - MAX_FILA) enviarLote();
}

// ---------- comandos ----------
// Extrae "campo": "valor" de un JSON simple (tolera espacios)
String campo(const String &json, const char *nombre) {
  int i = json.indexOf(String("\"") + nombre + "\"");
  if (i < 0) return "";
  i = json.indexOf(':', i);
  i = json.indexOf('"', i) + 1;
  int f = json.indexOf('"', i);
  return (i <= 0 || f < 0) ? "" : json.substring(i, f);
}

void alRecibir(char *topic, byte *payload, unsigned int n) {
  String msg;                                       // copia antes de publicar:
  msg.reserve(n);                                   // PubSubClient reusa su buffer
  for (unsigned int i = 0; i < n; i++) msg += (char)payload[i];

  String cmd = campo(msg, "cmd");
  if (cmd == "start") {
    snprintf(encabezado, sizeof(encabezado), "#%s,%s,%s",
             campo(msg, "sesion").c_str(), campo(msg, "label").c_str(),
             campo(msg, "sujeto").c_str());
    largo = 0;
    seq = 0;
    ppg.clearFIFO();
    streaming = true;
    Serial.printf("START %s\n", encabezado);
  } else if (cmd == "stop") {
    enviarLote();                                   // manda lo que quedo
    streaming = false;
    Serial.println("STOP");
  }
}

bool conectarMQTT() {
  Serial.print("Conectando a AWS IoT... ");
  // Last Will: si el ESP32 desaparece, AWS publica "offline" (retenido)
  if (mqtt.connect(THING_NAME, TOPIC_STATUS, 1, true, "offline")) {
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_CMD, 1);
    Serial.println("ok");
    return true;
  }
  Serial.printf("fallo, estado=%d\n", mqtt.state());
  return false;
}

// ---------- setup / loop ----------
void detenerConError(const char *msg) {
  while (true) { Serial.println(msg); delay(2000); }
}

void setup() {
  Serial.begin(115200);
  crearCola();

  Wire.setPins(I2C_SDA, I2C_SCL);
  Wire.begin();
  Wire.setClock(400000);
  if (!ppg.begin(Wire, I2C_SPEED_FAST)) detenerConError("MAX30102 no encontrado");
  ppg.setup(0x3F, 4, 2, 400, 411, 16384);           // 100 muestras/s, rojo + IR
  if (!mpu.begin(0x68, &Wire)) detenerConError("MPU6050 no encontrado");
  mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.printf(" ok (%s)\n", WiFi.localIP().toString().c_str());

  net.setCACert(AWS_ROOT_CA);
  net.setCertificate(DEVICE_CERT);
  net.setPrivateKey(DEVICE_KEY);
  mqtt.setServer(AWS_ENDPOINT, 8883);
  mqtt.setBufferSize(TAM_SLOT + 512);               // default: 256 bytes
  mqtt.setKeepAlive(60);
  mqtt.setCallback(alRecibir);
  conectarMQTT();
}

void loop() {
  if (!mqtt.connected() && WiFi.status() == WL_CONNECTED &&
      millis() - ultimoIntento > 5000) {
    ultimoIntento = millis();
    conectarMQTT();
  }
  mqtt.loop();
  vaciarCola();

  if (!streaming) return;
  ppg.check();
  while (ppg.available()) {
    uint32_t red = ppg.getFIFORed();
    uint32_t ir = ppg.getFIFOIR();
    ppg.nextSample();
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    agregarFila(red, ir, a, g);
  }
}
