/*
 * esp32_aws_sim.ino — prueba de conectividad ESP32-S3 -> AWS IoT Core
 *
 * Mismo protocolo que esp32_aws.ino (tópicos, comandos, lotes CSV, cola de
 * reconexión), pero SIN sensores: genera a 100 Hz señales sintéticas de
 * PPG (rojo / IR) y de IMU. No necesitas conectar el MAX30102 ni el MPU6050.
 * Usa el mismo secrets.h y la misma librería PubSubClient.
 *
 * La señal depende de la etiqueta que manda control.py en "start":
 *   contiene "correr"   -> FC ~145 lpm + movimiento fuerte
 *   contiene "caminar"  -> FC ~100 lpm + movimiento moderado
 *   cualquier otra      -> reposo, FC ~70 lpm, sin movimiento
 * Así pruebas el pipeline completo (S3 -> pandas) con datos que se ven reales.
 *
 * Formato de cada lote en bio/<thing>/data (idéntico al firmware real):
 *   #<sesion>,<label>,<sujeto>
 *   seq,t_ms,red,ir,ax,ay,az,gx,gy,gz
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <math.h>
#include "secrets.h"

// ---------- prueba sin PC ----------
// 1 = al conectarse empieza a transmitir solo (label "sim_reposo") durante
//     AUTO_START_SEG segundos y se detiene. Útil para probar sin control.py.
#define AUTO_START      0
#define AUTO_START_SEG  30

// ---------- muestreo y lotes ----------
#define FS_HZ        100
#define PERIODO_US   (1000000UL / FS_HZ)
#define FIFO_MAX     32       // como el MAX30102: si el loop se atrasa más, se pierden muestras
#define LIMITE_LOTE  24500    // bytes: < 25 KB = 5 bloques de 5 KB, ~3 s de datos
#define MAX_FILA     120
#define TAM_SLOT     (LIMITE_LOTE + MAX_FILA)

const char *TOPIC_DATA   = "bio/" THING_NAME "/data";
const char *TOPIC_CMD    = "bio/" THING_NAME "/cmd";
const char *TOPIC_STATUS = "bio/" THING_NAME "/status";

WiFiClientSecure net;
PubSubClient mqtt(net);

bool streaming = false;
uint32_t seq = 0;
char encabezado[96] = "#sin_sesion,sin_label,sin_sujeto";
char lote[TAM_SLOT];
size_t largo = 0;
uint32_t ultimoIntento = 0;
uint32_t proximoUs = 0;          // instante de la siguiente muestra
uint32_t finAutoMs = 0;

// estadísticas para el monitor serie
uint32_t lotesEnviados = 0, lotesEncolados = 0, muestrasPerdidas = 0, reconexiones = 0;
uint32_t ultimoReporte = 0;

// ======================================================================
//                         SIMULACIÓN DE SEÑALES
// ======================================================================
struct Perfil {
  const char *nombre;
  float fcBase;     // frecuencia cardiaca media (lpm)
  float mov;        // intensidad de movimiento (0 = quieto)
  float fPaso;      // frecuencia de zancada (Hz)
};
const Perfil REPOSO  = {"reposo",  70.0f, 0.0f, 0.0f};
const Perfil CAMINAR = {"caminar", 100.0f, 1.0f, 0.9f};
const Perfil CORRER  = {"correr",  145.0f, 2.2f, 1.4f};
Perfil perfil = REPOSO;

// Debe ir ANTES de la primera función: el IDE de Arduino inserta los
// prototipos automáticos (p. ej. "Muestra simular();") justo antes de ella.
struct Muestra { uint32_t red, ir; float ax, ay, az, gx, gy, gz; };

uint32_t nSim = 0;    // muestras simuladas desde el start (incluye las perdidas)
float faseCard = 0;    // fase del ciclo cardiaco, 0..1

// ruido aproximadamente gaussiano (suma de 3 uniformes), media 0 y desv. ~sigma
float ruido(float sigma) {
  float s = 0;
  for (int i = 0; i < 3; i++) s += (float)esp_random() / 4294967295.0f;
  return (s - 1.5f) * 2.0f * sigma;
}

// forma de un pulso PPG: pico sistólico + muesca dicrota, p en [0,1)
float pulso(float p) {
  float a = (p - 0.15f) / 0.06f;
  float b = (p - 0.45f) / 0.08f;
  return expf(-a * a) + 0.35f * expf(-b * b);
}

Muestra simular() {
  const float t = nSim++ / (float)FS_HZ;
  const float w = 2.0f * PI * perfil.fPaso;
  const float m = perfil.mov;

  // FC con variabilidad lenta (±3 lpm en ~20 s)
  float fc = perfil.fcBase + 3.0f * sinf(2.0f * PI * t / 20.0f);
  faseCard += fc / 60.0f / FS_HZ;
  if (faseCard >= 1.0f) faseCard -= 1.0f;
  float pul = pulso(faseCard);

  float resp = sinf(2.0f * PI * 0.25f * t);          // respiración, 15 rpm
  float artefacto = fminf(m, 1.0f) * 120.0f * sinf(w * t + 0.7f); // el movimiento ensucia el PPG

  Muestra s;
  // AC/DC elegidos para una razón R ~0.5  ->  SpO2 ~97 %
  s.ir  = (uint32_t)(115000.0f - 1200.0f * pul + 300.0f * resp + artefacto + ruido(40));
  s.red = (uint32_t)( 90000.0f -  470.0f * pul + 230.0f * resp + 0.8f * artefacto + ruido(40));

  // IMU: gravedad en z + oscilación de la zancada (m/s^2 y rad/s)
  s.ax = m * 1.5f * sinf(w * t)              + ruido(0.03f);
  s.ay = m * 0.8f * sinf(2.0f * w * t + 1.0f) + ruido(0.03f);
  s.az = 9.81f + m * 2.0f * sinf(2.0f * w * t) + ruido(0.03f);
  s.gx = m * 0.6f * cosf(w * t)              + ruido(0.005f);
  s.gy = m * 0.3f * sinf(2.0f * w * t)       + ruido(0.005f);
  s.gz = m * 0.2f * cosf(w * t + 0.5f)       + ruido(0.005f);
  return s;
}

// ======================================================================
//                COLA, LOTES Y MQTT (igual que el firmware real)
// ======================================================================
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
  Serial.printf("Cola de %d lotes (%s)\n", capacidad, psramFound() ? "PSRAM" : "RAM");
}

void encolar(const char *datos, size_t n) {
  if (colaN == capacidad) {                         // llena: descarta el más viejo
    colaIni = (colaIni + 1) % capacidad;
    colaN--;
  }
  int pos = (colaIni + colaN) % capacidad;
  memcpy(cola + pos * TAM_SLOT, datos, n);
  colaLargo[pos] = n;
  colaN++;
  lotesEncolados++;
}

void vaciarCola() {                                 // máx. 2 lotes por vuelta
  for (int k = 0; k < 2 && colaN > 0 && mqtt.connected(); k++) {
    const uint8_t *p = (const uint8_t *)(cola + colaIni * TAM_SLOT);
    if (!mqtt.publish(TOPIC_DATA, p, colaLargo[colaIni], false)) return;
    colaIni = (colaIni + 1) % capacidad;
    colaN--;
    lotesEnviados++;
  }
}

void enviarLote() {
  if (largo == 0) return;
  bool ok = mqtt.connected() && colaN == 0 &&
            mqtt.publish(TOPIC_DATA, (const uint8_t *)lote, largo, false);
  if (ok) lotesEnviados++;
  else encolar(lote, largo);                        // conserva el orden
  largo = 0;
}

void agregarFila(const Muestra &s) {
  if (largo == 0) largo = snprintf(lote, sizeof(lote), "%s\n", encabezado);
  largo += snprintf(lote + largo, sizeof(lote) - largo,
                    "%lu,%lu,%lu,%lu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                    (unsigned long)seq++, (unsigned long)millis(),
                    (unsigned long)s.red, (unsigned long)s.ir,
                    s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
  if (largo > LIMITE_LOTE - MAX_FILA) enviarLote();
}

// ---------- comandos ----------
String campo(const String &json, const char *nombre) {
  int i = json.indexOf(String("\"") + nombre + "\"");
  if (i < 0) return "";
  i = json.indexOf(':', i);
  i = json.indexOf('"', i) + 1;
  int f = json.indexOf('"', i);
  return (i <= 0 || f < 0) ? "" : json.substring(i, f);
}

void iniciar(const String &sesion, const String &label, const String &sujeto) {
  snprintf(encabezado, sizeof(encabezado), "#%s,%s,%s",
           sesion.c_str(), label.c_str(), sujeto.c_str());
  String l = label;
  l.toLowerCase();
  if (l.indexOf("correr") >= 0)       perfil = CORRER;
  else if (l.indexOf("caminar") >= 0) perfil = CAMINAR;
  else                                perfil = REPOSO;

  largo = 0;
  seq = 0;
  nSim = 0;
  faseCard = 0;
  muestrasPerdidas = 0;
  proximoUs = micros();
  streaming = true;
  Serial.printf("START %s  (perfil simulado: %s)\n", encabezado, perfil.nombre);
}

void detener() {
  enviarLote();                                     // manda lo que quedó
  streaming = false;
  Serial.printf("STOP  muestras=%lu perdidas=%lu\n",
                (unsigned long)seq, (unsigned long)muestrasPerdidas);
}

void alRecibir(char *topic, byte *payload, unsigned int n) {
  String msg;                                       // copia antes de publicar:
  msg.reserve(n);                                   // PubSubClient reusa su buffer
  for (unsigned int i = 0; i < n; i++) msg += (char)payload[i];

  String cmd = campo(msg, "cmd");
  if (cmd == "start") {
    iniciar(campo(msg, "sesion"), campo(msg, "label"), campo(msg, "sujeto"));
  } else if (cmd == "stop") {
    detener();
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
  // -2 = no hubo conexión TCP/TLS (endpoint, certificados, red)
  // -4 / desconexión inmediata = AWS lo rechazó (política o client ID)
  Serial.printf("fallo, estado=%d\n", mqtt.state());
  if (mqtt.state() == -2) diagnosticar();
  return false;
}

// Explica en qué paso falla la conexión TLS (solo se llama si estado = -2)
void diagnosticar() {
  IPAddress ip;
  if (!WiFi.hostByName(AWS_ENDPOINT, ip)) {
    Serial.printf("  [DNS] no resuelve '%s' -> revisa AWS_ENDPOINT en secrets.h\n", AWS_ENDPOINT);
    return;
  }
  Serial.printf("  [DNS] ok -> %s\n", ip.toString().c_str());
  Serial.printf("  [RAM] heap libre=%u  bloque max=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  char err[128] = "";
  int codigo = net.lastError(err, sizeof(err));
  Serial.printf("  [TLS] error %d (-0x%04X): %s\n", codigo, (unsigned)-codigo, err);
}

// ======================================================================
//                              SETUP / LOOP
// ======================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n== ESP32-S3 -> AWS IoT Core | DATOS SIMULADOS ==");
  crearCola();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.printf(" ok (%s, RSSI %d dBm)\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // Hora por NTP: no es obligatoria para conectar, pero sirve para los logs.
  // Se espera máximo 10 s por si la red bloquea NTP.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Hora NTP");
  for (int i = 0; i < 33 && time(nullptr) < 1700000000; i++) { delay(300); Serial.print("."); }
  Serial.println(time(nullptr) >= 1700000000 ? " ok" : " sin respuesta (se continúa)");

  net.setCACert(AWS_ROOT_CA);
  net.setCertificate(DEVICE_CERT);
  net.setPrivateKey(DEVICE_KEY);
  mqtt.setServer(AWS_ENDPOINT, 8883);
  mqtt.setBufferSize(TAM_SLOT + 512);               // default: 256 bytes
  mqtt.setKeepAlive(60);
  mqtt.setCallback(alRecibir);
  conectarMQTT();

#if AUTO_START
  iniciar(String("auto_") + millis(), "sim_reposo", "s00");
  finAutoMs = millis() + AUTO_START_SEG * 1000UL;
#endif
}

void reportar() {
  if (millis() - ultimoReporte < 5000) return;
  ultimoReporte = millis();
  Serial.printf("[%s] enviados=%lu en_cola=%d encolados=%lu perdidas=%lu reconexiones=%lu RSSI=%d\n",
                streaming ? "grabando" : "en espera",
                (unsigned long)lotesEnviados, colaN, (unsigned long)lotesEncolados,
                (unsigned long)muestrasPerdidas, (unsigned long)reconexiones, WiFi.RSSI());
}

void loop() {
  if (!mqtt.connected() && WiFi.status() == WL_CONNECTED &&
      millis() - ultimoIntento > 5000) {
    ultimoIntento = millis();
    reconexiones++;
    conectarMQTT();
  }
  mqtt.loop();
  vaciarCola();
  reportar();

#if AUTO_START
  if (streaming && finAutoMs && (int32_t)(millis() - finAutoMs) >= 0) {
    finAutoMs = 0;
    detener();
  }
#endif

  if (!streaming) return;

  // ¿Cuántas muestras tocan desde la última vuelta?  (reloj de 100 Hz)
  uint32_t ahora = micros();
  if ((int32_t)(ahora - proximoUs) < 0) return;
  uint32_t debidas = (ahora - proximoUs) / PERIODO_US + 1;
  proximoUs += debidas * PERIODO_US;

  // Como el FIFO real: si el loop se bloqueó (p. ej. reconexión TLS),
  // solo sobreviven las últimas 32 muestras; el tiempo simulado sí avanza.
  if (debidas > FIFO_MAX) {
    muestrasPerdidas += debidas - FIFO_MAX;
    for (uint32_t i = 0; i < debidas - FIFO_MAX; i++) simular();
    debidas = FIFO_MAX;
  }
  for (uint32_t i = 0; i < debidas; i++) agregarFila(simular());
}
