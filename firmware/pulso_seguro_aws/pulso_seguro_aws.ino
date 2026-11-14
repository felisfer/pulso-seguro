/*
 * ============================================================================
 * PulsoSeguro - Pulsera IoT de monitoreo de signos vitales y detección de caídas
 * Computing in industry - Facultad de Ingeniería, UAEMéx - 2026B
 * ============================================================================
 *
 * Hardware (ESP32-WROOM-32):
 *    MPU6050 (I2C 0x68) ........ SDA = GPIO 21, SCL = GPIO 22
 *    MAX30102 (I2C 0x57) ........ mismo bus I2C (SDA 21, SCL 22)
 *    Push button ................ GPIO 27 -> GND (INPUT_PULLUP)
 *    Buzzer pasivo .............. GPIO 13
 *    TFT ST7789 240x320 (SPI) ... CS = GPIO 15, DC = GPIO 16, RST = GPIO 4,
 *                                 MOSI = GPIO 23, SCK = GPIO 18, BLK = GPIO 32, VCC = 3V3
 *    Cambio respecto a la prueba de la pantalla: DC pasa de GPIO 2 a GPIO 16, porque el
 *    GPIO 2 es un strapping pin (debe estar en LOW/flotante al arrancar para poder cargar código).
 *
 * Cambios:
 *    - El umbral de impacto es fijo (UMBRAL_IMPACTO_G = 1.5 g, el valor
 *      de máxima sensibilidad, con el que P3 detectó las caídas con picos de 1.67-2.22 g).
 *    - Las indicaciones del LED las cubren la pantalla TFT y el buzzer.
 *      El ESP32 solo se conecta como estación a la red con internet para AWS IoT.
 *
 * Captura de datos a AWS IoT Core (versión 4):
 *    - Wi-Fi en modo estación (STA): el ESP32 se conecta a una red con internet
 *      (WIFI_SSID en secrets.h).
 *    - MQTT/TLS corre en una tarea propia en el núcleo 0. La detección de caídas, la
 *      pantalla y el botón siguen en el núcleo 1 y nunca esperan a la red.
 *    - control.py manda {"cmd":"start", ...} / {"cmd":"stop"} a bio/<THING_NAME>/cmd.
 *    - Cada lote se publica en bio/<THING_NAME>/data, con el formato de la guía:
 *        #<sesion>,<label>,<sujeto>
 *        seq,t_ms,red,ir,ax,ay,az,gx,gy,gz      (acel. en m/s^2, giro en rad/s)
 *      Una fila por cada muestra del MAX30102 (100 Hz) con la última lectura del MPU6050.
 *    - Requiere la librería "PubSubClient" (Nick O'Leary) y secrets.h en esta carpeta.
 *    - Tools -> Partition Scheme -> "Huge APP (3MB No OTA/1MB SPIFFS)": con TLS, el
 *      programa ya no cabe en la partición por defecto de 1.2 MB.
 *
 * Predicción de FC a +10 s (versión 6):
 *    - Una tarea en el núcleo 0 calcula 16 features cada 2 s (ventana de 8 s) con
 *      features_fc.h, el mismo algoritmo que modelo/features.py (probar_paridad.py).
 *    - Las ventanas con FC confiable se publican en bio/<THING_NAME>/features; la Lambda
 *      responde en bio/<THING_NAME>/pred con la FC prevista (prediccion_fc.h).
 *    - La pantalla muestra "FC en 10 s". La predicción solo puede ADELANTAR el semáforo de
 *      FC de verde a amarillo ("Prev. alta"); nunca quita un amarillo o un rojo.
 *    - Sin internet no hay FC prevista, pero todo lo demás funciona igual: las caídas,
 *      el pánico y el semáforo se deciden siempre en el ESP32.
 *
 * Reglas automatizadas:
 *    R1 Detección de caída en 4 fases: caída libre -> impacto -> inmovilidad -> cambio de orientación
 *    R2 Ventana de confirmación de 15 s; si no se cancela, alarma completa (máximo 60 s de sonido)
 *    R3 Semáforo de FC y SpO2 en la pantalla (verde/amarillo/rojo)
 *    R4 Pulsación corta (< 1 s): silencia / cancela + pitido corto
 *    R5 Pulsación larga (3 s): botón de pánico
 *    (R6 potenciómetro y R7 "Silenciar" desde el dashboard se eliminaron en la versión 5)
 *    Tras silenciar se esperan 10 s antes de volver a detectar caídas.
 *
 * Pantalla (versión 3):
 *    - Muestra el estado, la cuenta regresiva, FC y SpO2 con el color del semáforo,
 *      el umbral de impacto y el último evento.
 *    - Nunca se borra la pantalla completa en el loop(): solo se redibuja la región que
 *      cambió, una región por vuelta del ciclo, para no retrasar la lectura del MPU6050 (10 ms).
 *    - Mientras se analiza un posible impacto (3.8 s) la pantalla no se actualiza,
 *      para no perder muestras del pico.
 *    - El Monitor Serie reporta el tiempo máximo de dibujo y de ciclo (prueba P19).
 *
 * Valores obtenidos en las pruebas:
 *    P3: caídas del brazo -> caída libre 0.26-0.56 g, pico 1.67-2.22 g, orientación 70-87°
 *        saltos -> orientación 1-9° | movimientos cotidianos -> orientación hasta 43°
 *    P4: MAX30102 con brillo 100 y rango ADC 16384 -> IR con dedo ≈ 118 000
 *        FC por detección de latidos estable (67-76 lpm); el algoritmo de Maxim solo se usa para SpO2
 *
 * Librerías necesarias (Gestor de librerías del IDE de Arduino):
 *    - "SparkFun MAX3010x Pulse and Proximity Sensor Library"
 *    - "Adafruit GFX Library"
 *    - "Adafruit ST7735 and ST7789 Library"
 * Placa: "ESP32 Dev Module" (paquete esp32 de Espressif, versión 3.x)
 * ============================================================================
 */

#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <stdarg.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "MAX30105.h"          // La librería de SparkFun también maneja el MAX30102
#include "spo2_algorithm.h"
#include "heartRate.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <esp_heap_caps.h>      // bloque libre más grande del heap (diagnóstico de TLS)
#include "secrets.h"           // WIFI_SSID, WIFI_PASS, THING_NAME, AWS_ENDPOINT y certificados
#include "prediccion_fc.h"     // features + MQTT de la predicción (incluye features_fc.h)

// ----------------------------------------------------------------------------
// Configuración general
// ----------------------------------------------------------------------------
const bool BUZZER_PASIVO   = true;              // P8: el buzzer es pasivo
// Predicción de FC (versión 6). Ponlo en false para descartar que un fallo de TLS sea por memoria:
// sin la predicción, el ESP32 recupera ~35 KB de heap.
const bool PREDICCION_ACTIVA = true;

// Pines
const int PIN_SDA    = 21;
const int PIN_SCL    = 22;
const int PIN_BOTON  = 27;
const int PIN_BUZZER = 13;

// Pantalla TFT ST7789 (SPI por hardware, VSPI)
const int PIN_TFT_CS   = 15;
const int PIN_TFT_DC   = 16;                    // antes GPIO 2 (strapping pin)
const int PIN_TFT_RST  = 4;
const int PIN_TFT_MOSI = 23;
const int PIN_TFT_SCK  = 18;
const int PIN_TFT_BLK  = 32;                    // retroiluminación

// Tiempos de las reglas (ms)
const unsigned long VENTANA_CONFIRMACION_MS = 15000;   // R2
const unsigned long ALARMA_MAX_SONIDO_MS    = 60000;   // R2: sonido máximo 1 minuto
const unsigned long ESPERA_TRAS_SILENCIO_MS = 10000;   // espera antes de volver a detectar
const unsigned long PULSACION_CORTA_MS      = 1000;    // R4
const unsigned long PULSACION_LARGA_MS      = 3000;    // R5
const unsigned long ANTIRREBOTE_MS          = 30;      // P7: 0 rebotes observados

// Detección de caída (R1) - ajustada con la prueba P3
const float DISPARO_G              = 1.5;     // a partir de aquí se analiza el movimiento
const float CAIDA_LIBRE_G          = 0.7;     // fase 1: mínimo antes del impacto
const unsigned long CAIDA_LIBRE_MAX_MS = 800; //          tiempo máximo entre caída libre e impacto
const float UMBRAL_IMPACTO_G       = 1.5;     // fase 2: umbral fijo (antes 1.5-2.5 g con el potenciómetro)
const float TOLERANCIA_INMOVIL_G   = 0.3;     // fase 3: |magnitud - 1 g| máximo entre 0.5 y 2.5 s
const float CAMBIO_ORIENTACION_MIN = 60.0;    // fase 4: grados entre la gravedad antes y después
const unsigned long ANALISIS_MS    = 3500;    // la caída se confirma 3.5 s después del impacto

// Semáforo (R3) - valores de referencia (no son criterios médicos)
const int FC_VERDE_MIN    = 60;   // lpm
const int FC_VERDE_MAX    = 100;
const int FC_AMARILLO_MIN = 50;
const int FC_AMARILLO_MAX = 120;
const int SPO2_VERDE_MIN    = 95; // %
const int SPO2_AMARILLO_MIN = 90;

// MAX30102 - ajustado con la prueba P4
const int BRILLO_LED_MAX = 100;               // IR con dedo ≈ 118 000, sin saturar
const int RANGO_ADC_MAX  = 16384;
const uint32_t IR_MIN_DEDO = 50000;           // sin dedo ≈ 700, con dedo ≈ 118 000

// Pantalla
const unsigned long TFT_CALCULO_MS = 250;     // el contenido se recalcula 4 veces por segundo
const unsigned long TFT_PAUSA_MS   = 40;      // tiempo mínimo entre el dibujo de dos regiones

// ----------------------------------------------------------------------------
// Objetos globales
// ----------------------------------------------------------------------------
MAX30105 oximetro;
Adafruit_ST7789 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
bool oximetroOK = false;
bool mpuOK = false;

// Contenido de cada región de la pantalla TFT.
// Va aquí arriba porque el IDE de Arduino genera los prototipos de las funciones
// antes de la primera función del archivo, y varias usan este tipo.
// Cada región guarda lo que debería mostrar y lo que ya se dibujó.
// Solo se redibuja si cambió algo, y solo la parte que cambió.
struct ContenidoRegion {
  uint16_t fondo = 0;
  uint16_t color = 0;
  char l1[56] = "";
  char l2[56] = "";
  int valor = -1;
  bool operator==(const ContenidoRegion& o) const {
    return fondo == o.fondo && color == o.color && valor == o.valor &&
           strcmp(l1, o.l1) == 0 && strcmp(l2, o.l2) == 0;
  }
};

// ----------------------------------------------------------------------------
// Estados del sistema
// ----------------------------------------------------------------------------
enum EstadoSistema { NORMAL, CONFIRMACION, ALARMA, ESPERA };
EstadoSistema estado = NORMAL;
unsigned long tEstado = 0;
bool alarmaPorPanico = false;

// Registro de eventos (la pantalla muestra el más reciente)
struct Evento { unsigned long t; char msg[96]; };
const int MAX_EVENTOS = 12;
Evento eventos[MAX_EVENTOS];
int numEventos = 0, idxEvento = 0;

void registrarEvento(const char* formato, ...) {
  va_list args;
  va_start(args, formato);
  vsnprintf(eventos[idxEvento].msg, sizeof(eventos[idxEvento].msg), formato, args);
  va_end(args);
  eventos[idxEvento].t = millis();
  Serial.printf("[%lu ms] EVENTO: %s\n", millis(), eventos[idxEvento].msg);
  idxEvento = (idxEvento + 1) % MAX_EVENTOS;
  if (numEventos < MAX_EVENTOS) numEventos++;
}

// ----------------------------------------------------------------------------
// Buzzer
// ----------------------------------------------------------------------------
unsigned long tPitidoCortoFin = 0;

void buzzerTono(int frecuencia) {
  static int frecuenciaActual = -1;
  if (frecuencia == frecuenciaActual) return;
  frecuenciaActual = frecuencia;
  if (BUZZER_PASIVO) ledcWriteTone(PIN_BUZZER, frecuencia);
  else digitalWrite(PIN_BUZZER, frecuencia > 0 ? HIGH : LOW);
}

void pitidoCorto() { tPitidoCortoFin = millis() + 150; }

void actualizarBuzzer() {
  unsigned long ahora = millis();
  if (ahora < tPitidoCortoFin) { buzzerTono(2500); return; }
  if (estado == CONFIRMACION) {
    buzzerTono(((ahora - tEstado) % 1000) < 200 ? 1500 : 0);          // pitidos lentos
  } else if (estado == ALARMA && (ahora - tEstado) < ALARMA_MAX_SONIDO_MS) {
    buzzerTono(((ahora / 300) % 2) ? 1000 : 2000);                     // sirena
  } else {
    buzzerTono(0);
  }
}

// ----------------------------------------------------------------------------
// Estados
// ----------------------------------------------------------------------------
const char* nombreEstado() {
  switch (estado) {
    case CONFIRMACION: return "CONFIRMACION";
    case ALARMA:       return "ALARMA";
    case ESPERA:       return "ESPERA";
    default:           return "NORMAL";
  }
}

void cambiarEstado(EstadoSistema nuevo) {
  estado = nuevo;
  tEstado = millis();
  Serial.printf("[%lu ms] ESTADO -> %s\n", millis(), nombreEstado());
}

void activarPanico() {                                    // R5
  alarmaPorPanico = true;
  cambiarEstado(ALARMA);
  registrarEvento("Botón de pánico activado");
}

void silenciar() {                                        // R4
  pitidoCorto();
  if (estado == CONFIRMACION) {
    registrarEvento("Caída cancelada: la persona está bien");
    cambiarEstado(ESPERA);
  } else if (estado == ALARMA) {
    registrarEvento("Alarma silenciada con el botón");
    cambiarEstado(ESPERA);
  }
}

void actualizarEstados() {
  unsigned long t = millis() - tEstado;
  static unsigned long alarmaRegistrada = 0;
  if (estado == CONFIRMACION && t >= VENTANA_CONFIRMACION_MS) {          // R2
    alarmaPorPanico = false;
    cambiarEstado(ALARMA);
    registrarEvento("Caída confirmada: sin respuesta en 15 s");
  } else if (estado == ALARMA && t >= ALARMA_MAX_SONIDO_MS && alarmaRegistrada != tEstado) {
    alarmaRegistrada = tEstado;
    registrarEvento("Sonido detenido tras 60 s (alarma sin atender)");
  } else if (estado == ESPERA && t >= ESPERA_TRAS_SILENCIO_MS) {
    cambiarEstado(NORMAL);
    registrarEvento("Detección de caídas reanudada");
  }
}

// Segundos que faltan para que termine el estado actual (pantalla)
long segundosRestantes() {
  unsigned long t = millis() - tEstado;
  if (estado == CONFIRMACION) return (VENTANA_CONFIRMACION_MS - min(t, VENTANA_CONFIRMACION_MS)) / 1000;
  if (estado == ALARMA)       return (ALARMA_MAX_SONIDO_MS - min(t, ALARMA_MAX_SONIDO_MS)) / 1000;
  if (estado == ESPERA)       return (ESPERA_TRAS_SILENCIO_MS - min(t, ESPERA_TRAS_SILENCIO_MS)) / 1000;
  return 0;
}

bool alarmaSonando() {
  return estado == ALARMA && (millis() - tEstado) < ALARMA_MAX_SONIDO_MS;
}

// ----------------------------------------------------------------------------
// Push button (R4, R5)
// ----------------------------------------------------------------------------
void leerBoton() {
  static bool ultimaLectura = HIGH, estable = HIGH, largaDisparada = false;
  static unsigned long tCambio = 0, tPresion = 0;

  bool lectura = digitalRead(PIN_BOTON);
  if (lectura != ultimaLectura) { tCambio = millis(); ultimaLectura = lectura; }

  if (millis() - tCambio > ANTIRREBOTE_MS && lectura != estable) {
    estable = lectura;
    if (estable == LOW) {
      tPresion = millis();
      largaDisparada = false;
    } else {
      unsigned long duracion = millis() - tPresion;
      if (!largaDisparada && duracion < PULSACION_CORTA_MS) silenciar();
    }
  }
  if (estable == LOW && !largaDisparada && millis() - tPresion >= PULSACION_LARGA_MS) {
    largaDisparada = true;
    activarPanico();
  }
}

// ----------------------------------------------------------------------------
// MPU6050 y detección de caída en 4 fases (R1)
// ----------------------------------------------------------------------------
const uint8_t MPU_ADDR = 0x68;
float ax = 0, ay = 0, az = 1, magnitudG = 1.0;
float gx = 0, gy = 0, gz = 0;                            // giroscopio en rad/s (solo para la captura AWS)
const float GYRO_RAD_POR_LSB = (1.0 / 65.5) * PI / 180.0; // rango ±500 °/s
float magPicoActual = 0, magPicoUltimo = 0;
unsigned long tVentanaPico = 0;

// Historial de 2 s (100 muestras por segundo)
const int HIST = 200;
float hMag[HIST], hX[HIST], hY[HIST], hZ[HIST];
unsigned long hT[HIST];
int idxH = 0, nH = 0;

enum FaseCaida { FC_REPOSO, FC_PICO, FC_ANALISIS };
FaseCaida faseCaida = FC_REPOSO;
float pico = 0, minPrevio = 1, desvMax = 0;
unsigned long tPico = 0, tMinPrevio = 0;
float antesX = 0, antesY = 0, antesZ = 1;
double despX = 0, despY = 0, despZ = 0; int nDesp = 0;

bool iniciarMPU() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); Wire.write(0x00);                   // despertar
  if (Wire.endTransmission() != 0) return false;
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x1C); Wire.write(0x18); Wire.endTransmission();   // ±16 g
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x1B); Wire.write(0x08); Wire.endTransmission();   // giro ±500 °/s
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x1A); Wire.write(0x03); Wire.endTransmission();   // filtro ~44 Hz
  return true;
}

bool leerAcelerometro() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  // 14 bytes: acelerómetro (6), temperatura (2) y giroscopio (6), en una sola lectura
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)14) != 14) return false;
  int16_t rx = (Wire.read() << 8) | Wire.read();
  int16_t ry = (Wire.read() << 8) | Wire.read();
  int16_t rz = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();                              // temperatura (no se usa)
  int16_t rgx = (Wire.read() << 8) | Wire.read();
  int16_t rgy = (Wire.read() << 8) | Wire.read();
  int16_t rgz = (Wire.read() << 8) | Wire.read();
  ax = rx / 2048.0; ay = ry / 2048.0; az = rz / 2048.0;
  gx = rgx * GYRO_RAD_POR_LSB; gy = rgy * GYRO_RAD_POR_LSB; gz = rgz * GYRO_RAD_POR_LSB;
  magnitudG = sqrt(ax * ax + ay * ay + az * az);
  return true;
}

void iniciarAnalisis(unsigned long ahora) {
  // Fase 1: mínimo en el último segundo (caída libre)
  minPrevio = 99; tMinPrevio = ahora;
  // Orientación antes: promedio entre 2.0 y 1.5 s antes (o lo más antiguo disponible)
  double sx = 0, sy = 0, sz = 0; int n = 0;
  for (int i = 0; i < nH; i++) {
    int k = (idxH - 1 - i + HIST) % HIST;
    unsigned long edad = ahora - hT[k];
    if (edad <= 1000 && hMag[k] < minPrevio) { minPrevio = hMag[k]; tMinPrevio = hT[k]; }
    if (edad >= 1500) { sx += hX[k]; sy += hY[k]; sz += hZ[k]; n++; }
  }
  if (n == 0) {                                          // historial corto: usa la cuarta parte más antigua
    int m = max(1, nH / 4);
    for (int i = 0; i < m; i++) { int k = (idxH - nH + i + HIST) % HIST; sx += hX[k]; sy += hY[k]; sz += hZ[k]; }
    n = m;
  }
  antesX = sx / n; antesY = sy / n; antesZ = sz / n;
  if (minPrevio > 90) minPrevio = magnitudG;

  pico = magnitudG; tPico = ahora;
  faseCaida = FC_PICO;
}

void evaluarCaida() {
  bool f1 = minPrevio < CAIDA_LIBRE_G && (tPico - tMinPrevio) < CAIDA_LIBRE_MAX_MS;
  bool f2 = pico > UMBRAL_IMPACTO_G;
  bool f3 = desvMax < TOLERANCIA_INMOVIL_G;
  float dx = nDesp ? despX / nDesp : antesX, dy = nDesp ? despY / nDesp : antesY, dz = nDesp ? despZ / nDesp : antesZ;
  float na = sqrt(antesX * antesX + antesY * antesY + antesZ * antesZ), nd = sqrt(dx * dx + dy * dy + dz * dz);
  float c = (na > 0 && nd > 0) ? (antesX * dx + antesY * dy + antesZ * dz) / (na * nd) : 1;
  float angulo = acos(constrain(c, -1.0, 1.0)) * 180.0 / PI;
  bool f4 = angulo > CAMBIO_ORIENTACION_MIN;

  Serial.printf("[%lu ms] ANÁLISIS: caída libre %.2f g %s | pico %.2f g (umbral %.2f) %s | inmóvil %.2f g %s | orientación %.0f° %s\n",
                millis(), minPrevio, f1 ? "OK" : "NO", pico, UMBRAL_IMPACTO_G, f2 ? "OK" : "NO",
                desvMax, f3 ? "OK" : "NO", angulo, f4 ? "OK" : "NO");

  if (f1 && f2 && f3 && f4 && estado == NORMAL) {
    cambiarEstado(CONFIRMACION);                                     // R2
    registrarEvento("Posible caída (pico %.2f g, orientación %.0f°)", pico, angulo);
  }
}

void detectarCaida() {
  static unsigned long tLectura = 0;
  if (!mpuOK || millis() - tLectura < 10) return;          // 100 muestras por segundo
  tLectura = millis();
  if (!leerAcelerometro()) return;
  unsigned long ahora = millis();

  // Pico de cada segundo (encabezado de la pantalla)
  if (magnitudG > magPicoActual) magPicoActual = magnitudG;
  if (ahora - tVentanaPico >= 1000) { magPicoUltimo = magPicoActual; magPicoActual = 0; tVentanaPico = ahora; }

  switch (faseCaida) {
    case FC_REPOSO:
      hMag[idxH] = magnitudG; hX[idxH] = ax; hY[idxH] = ay; hZ[idxH] = az; hT[idxH] = ahora;
      idxH = (idxH + 1) % HIST; if (nH < HIST) nH++;
      if (estado == NORMAL && magnitudG > DISPARO_G) iniciarAnalisis(ahora);
      break;

    case FC_PICO:                                          // el pico termina 300 ms después del último máximo
      if (magnitudG > pico) { pico = magnitudG; tPico = ahora; }
      if (ahora - tPico > 300) { faseCaida = FC_ANALISIS; desvMax = 0; despX = despY = despZ = 0; nDesp = 0; }
      break;

    case FC_ANALISIS: {
      unsigned long t = ahora - tPico;
      float d = fabs(magnitudG - 1.0);
      if (t >= 500 && t < 2500 && d > desvMax) desvMax = d;           // fase 3
      if (t >= 1500) { despX += ax; despY += ay; despZ += az; nDesp++; }   // fase 4
      if (estado != NORMAL) { faseCaida = FC_REPOSO; nH = 0; break; } // pánico durante el análisis
      if (t >= ANALISIS_MS) {
        evaluarCaida();
        faseCaida = FC_REPOSO;
        nH = 0;                                            // el siguiente evento usa historial nuevo
      }
      break;
    }
  }
}

// ============================================================================
// AWS IoT Core: captura de datos para el modelo
//
//   núcleo 1 (loop)                          núcleo 0 (tareaAWS)
//   leerOximetro -> capturarFila  --lote-->  qListos -> mqtt.publish -> AWS
//                                <--libre--  qLibres
//   procesarComandosAWS  <--cmdPendiente--   callback de bio/<thing>/cmd
//
// Los lotes viven en un arreglo fijo (sin PSRAM en el ESP32-WROOM). Si AWS no
// responde y se llenan los 5 lotes (~8 s), las muestras nuevas se descartan y quedan
// como huecos en "seq" (cargar_s3.py los reporta como "perdidas").
// ============================================================================
#define LIMITE_LOTE  9800      // bytes: < 10 KB = se cobra como 2 mensajes, ~1.5 s de datos
#define MAX_FILA     120       // bytes máximos de una fila
#define TAM_SLOT     (LIMITE_LOTE + MAX_FILA)
#define N_LOTES      3         // ~4.5 s de datos si se cae la conexión (v6: antes 5; los 19.8 KB
                               // liberados los necesita el handshake TLS con la predicción activa)

const char* TOPIC_DATA   = "bio/" THING_NAME "/data";
const char* TOPIC_CMD    = "bio/" THING_NAME "/cmd";
const char* TOPIC_STATUS = "bio/" THING_NAME "/status";
const char* TOPIC_PRED   = "bio/" THING_NAME "/pred";     // respuesta de la Lambda
const float G_A_MS2 = 9.80665;

WiFiClientSecure awsNet;
PubSubClient mqtt(awsNet);

struct LoteListo { uint8_t idx; uint16_t largo; };
char lotesMem[N_LOTES][TAM_SLOT];
QueueHandle_t qLibres = nullptr, qListos = nullptr;

// Compartido entre núcleos
volatile bool awsWifi = false, awsConectado = false;
volatile uint32_t awsLotesEnviados = 0, awsMuestrasDescartadas = 0, awsIntentos = 0;
portMUX_TYPE muxCmd = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t cmdPendiente = 0;                       // 0 = nada, 1 = start, 2 = stop
char cmdEncabezado[96] = "";

// Solo núcleo 1
bool capturando = false;
uint32_t seqCaptura = 0;
char encabezadoCaptura[96] = "#sin_sesion,sin_label,sin_sujeto";
int loteActual = -1;
size_t largoLote = 0;
int filasLote = 0;

// --- Núcleo 0 -----------------------------------------------------------------
// Extrae "campo": "valor" de un JSON simple (tolera espacios)
String campoJSON(const String& json, const char* nombre) {
  int i = json.indexOf(String("\"") + nombre + "\"");
  if (i < 0) return "";
  i = json.indexOf(':', i);
  i = json.indexOf('"', i) + 1;
  int f = json.indexOf('"', i);
  return (i <= 0 || f < 0) ? "" : json.substring(i, f);
}

// Se ejecuta dentro de mqtt.loop(), en el núcleo 0: solo deja el comando pendiente
void alRecibirAWS(char* topic, byte* payload, unsigned int n) {
  if (strcmp(topic, TOPIC_PRED) == 0) { prediccionRecibir(payload, n); return; }   // FC prevista
  String msg;
  msg.reserve(n);
  for (unsigned int i = 0; i < n; i++) msg += (char)payload[i];
  String cmd = campoJSON(msg, "cmd");
  if (cmd == "start") {
    char enc[96];
    snprintf(enc, sizeof(enc), "#%s,%s,%s", campoJSON(msg, "sesion").c_str(),
             campoJSON(msg, "label").c_str(), campoJSON(msg, "sujeto").c_str());
    portENTER_CRITICAL(&muxCmd);
    memcpy(cmdEncabezado, enc, sizeof(enc));
    cmdPendiente = 1;
    portEXIT_CRITICAL(&muxCmd);
  } else if (cmd == "stop") {
    portENTER_CRITICAL(&muxCmd);
    cmdPendiente = 2;
    portEXIT_CRITICAL(&muxCmd);
  }
}

void conectarAWS() {
  awsIntentos++;
  // Last Will: si el ESP32 desaparece, AWS publica "offline" (retenido)
  if (mqtt.connect(THING_NAME, TOPIC_STATUS, 1, true, "offline")) {
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_CMD, 1);
    mqtt.subscribe(TOPIC_PRED, 0);
    Serial.println("[AWS] conectado");
    return;
  }
  // -2 = no hubo conexión TCP/TLS (endpoint, certificados, red)
  // -4 / desconexión inmediata = AWS lo rechazó (política o client ID)
  Serial.printf("[AWS] fallo, estado=%d", mqtt.state());
  if (mqtt.state() == -2) {
    char err[100] = "";
    int codigo = awsNet.lastError(err, sizeof(err));
    // Para TLS importa el bloque contiguo más grande, no solo el total libre
    Serial.printf(" | TLS %d (-0x%04X): %s | heap libre %u | bloque max %u | mínimo %u", codigo,
                  (unsigned)-codigo, err, (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
  }
  Serial.println();
}

void tareaAWS(void* parametro) {
  awsNet.setCACert(AWS_ROOT_CA);
  awsNet.setCertificate(DEVICE_CERT);
  awsNet.setPrivateKey(DEVICE_KEY);
  mqtt.setServer(AWS_ENDPOINT, 8883);
  mqtt.setBufferSize(TAM_SLOT + 512);                    // por defecto: 256 bytes
  mqtt.setKeepAlive(60);
  mqtt.setCallback(alRecibirAWS);

  unsigned long ultimoIntento = 0;
  bool primerIntento = true;
  LoteListo pendiente;
  bool hayPendiente = false;

  for (;;) {
    awsWifi = (WiFi.status() == WL_CONNECTED);
    if (!awsWifi) { awsConectado = false; vTaskDelay(pdMS_TO_TICKS(500)); continue; }

    if (!mqtt.connected()) {
      awsConectado = false;
      if (primerIntento || millis() - ultimoIntento > 5000) {
        primerIntento = false;
        ultimoIntento = millis();
        conectarAWS();                                   // bloquea 1-3 s, pero solo a este núcleo
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    awsConectado = true;
    mqtt.loop();
    prediccionPublicar(mqtt, THING_NAME);                // features listas (~230 bytes cada 2 s)
    if (!hayPendiente) hayPendiente = (xQueueReceive(qListos, &pendiente, 0) == pdTRUE);
    if (hayPendiente &&
        mqtt.publish(TOPIC_DATA, (const uint8_t*)lotesMem[pendiente.idx], pendiente.largo, false)) {
      xQueueSend(qLibres, &pendiente.idx, 0);            // el lote vuelve a estar libre
      hayPendiente = false;
      awsLotesEnviados++;
      continue;                                          // revisa de inmediato si hay otro
    }
    vTaskDelay(pdMS_TO_TICKS(10));                       // cede el núcleo (evita el watchdog)
  }
}

// --- Núcleo 1 -----------------------------------------------------------------
void iniciarAWS() {
  qLibres = xQueueCreate(N_LOTES, sizeof(uint8_t));
  qListos = xQueueCreate(N_LOTES, sizeof(LoteListo));
  for (uint8_t i = 0; i < N_LOTES; i++) xQueueSend(qLibres, &i, 0);
  xTaskCreatePinnedToCore(tareaAWS, "aws", 10240, nullptr, 1, nullptr, 0);
}

// Entrega el lote actual a la tarea AWS (o lo libera si no tiene filas)
void cerrarLote() {
  if (loteActual < 0) return;
  uint8_t idx = (uint8_t)loteActual;
  if (filasLote > 0) {
    LoteListo l = { idx, (uint16_t)largoLote };
    xQueueSend(qListos, &l, 0);                          // nunca falla: hay N_LOTES lugares
  } else {
    xQueueSend(qLibres, &idx, 0);
  }
  loteActual = -1; largoLote = 0; filasLote = 0;
}

// Una fila por muestra del MAX30102 (100 Hz) con la última lectura del MPU6050
void capturarFila(uint32_t rojo, uint32_t ir) {
  if (!capturando) return;
  if (loteActual < 0) {
    uint8_t idx;
    if (xQueueReceive(qLibres, &idx, 0) != pdTRUE) {     // AWS atrasado: se pierde la muestra
      awsMuestrasDescartadas++;
      seqCaptura++;                                      // queda el hueco en seq
      return;
    }
    loteActual = idx;
    largoLote = snprintf(lotesMem[idx], TAM_SLOT, "%s\n", encabezadoCaptura);
  }
  largoLote += snprintf(lotesMem[loteActual] + largoLote, TAM_SLOT - largoLote,
                        "%lu,%lu,%lu,%lu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                        (unsigned long)seqCaptura++, (unsigned long)millis(),
                        (unsigned long)rojo, (unsigned long)ir,
                        ax * G_A_MS2, ay * G_A_MS2, az * G_A_MS2, gx, gy, gz);
  filasLote++;
  if (largoLote > LIMITE_LOTE - MAX_FILA) cerrarLote();
}

// Aplica en el núcleo 1 los comandos que llegaron por MQTT
void procesarComandosAWS() {
  static bool conectadoAntes = false;
  if (awsConectado != conectadoAntes) {
    conectadoAntes = awsConectado;
    registrarEvento(awsConectado ? "Conectado a AWS IoT" : "Sin conexión con AWS IoT");
  }
  if (!cmdPendiente) return;

  uint8_t c;
  char enc[96];
  portENTER_CRITICAL(&muxCmd);
  c = cmdPendiente;
  cmdPendiente = 0;
  memcpy(enc, cmdEncabezado, sizeof(enc));
  portEXIT_CRITICAL(&muxCmd);

  if (c == 1) {
    cerrarLote();
    strncpy(encabezadoCaptura, enc, sizeof(encabezadoCaptura) - 1);
    encabezadoCaptura[sizeof(encabezadoCaptura) - 1] = 0;
    seqCaptura = 0;
    awsMuestrasDescartadas = 0;
    capturando = true;
    registrarEvento("Captura AWS iniciada: %s", encabezadoCaptura + 1);
  } else if (c == 2 && capturando) {
    cerrarLote();                                        // manda lo que quedó
    capturando = false;
    registrarEvento("Captura AWS detenida: %lu muestras", (unsigned long)seqCaptura);
  }
}

const char* estadoAWS() {
  if (awsConectado) return capturando ? "REC" : "OK";
  return awsWifi ? "..." : "--";
}

// ----------------------------------------------------------------------------
// MAX30102: FC por detección de latidos y SpO2 con el algoritmo de Maxim
// ----------------------------------------------------------------------------
const int BUF = 100;                                     // 4 s a 25 muestras por segundo
uint32_t bufIR[BUF], bufRojo[BUF];
int muestras = 0, nuevas = 0;
uint32_t acIR = 0, acRojo = 0; int k4 = 0;
uint32_t ultimoIR = 0;
bool hayDedo = false;

unsigned long nMuestra = 0, muestraUltLatido = 0, muestraDedo = 0;
const int NLAT = 4;
float lat[NLAT]; int idxLat = 0, nLat = 0, rechazosSeguidos = 0, latidosAceptados = 0;
int fc = 0, spo2 = 0;
float fcProm = 0;                                        // promedio interno (fc se muestra tras 6 latidos)
bool spo2Valida = false;
unsigned long tUltimaSpO2 = 0, tUltimoLatido = 0;

void reiniciarOximetro() {
  muestras = 0; nuevas = 0; acIR = acRojo = 0; k4 = 0;
  nLat = 0; idxLat = 0; rechazosSeguidos = 0; latidosAceptados = 0; muestraUltLatido = 0;
  fc = 0; fcProm = 0; spo2Valida = false;
}

bool iniciarOximetro() {
  if (!oximetro.begin(Wire, I2C_SPEED_FAST)) return false;
  // brillo, promedio, modo (2 = rojo + IR), tasa, ancho de pulso, rango ADC -> 100 muestras por segundo
  oximetro.setup(BRILLO_LED_MAX, 4, 2, 400, 411, RANGO_ADC_MAX);
  return true;
}

void procesarLatido() {
  if (nMuestra - muestraDedo < 200) { muestraUltLatido = nMuestra; return; }   // 2 s de estabilización
  if (muestraUltLatido == 0) { muestraUltLatido = nMuestra; return; }
  float bpm = 60000.0 / ((nMuestra - muestraUltLatido) * 10);

  // Latido demasiado cercano: ruido o muesca del mismo pulso; no se mueve la referencia
  if (bpm > 180 || (nLat >= NLAT && bpm > fcProm * 1.30)) {
    if (++rechazosSeguidos >= 3) { nLat = 0; idxLat = 0; rechazosSeguidos = 0; muestraUltLatido = nMuestra; }
    return;
  }
  muestraUltLatido = nMuestra;
  // Latido demasiado lejano: se perdió uno en medio
  if (bpm < 40 || (nLat >= NLAT && bpm < fcProm * 0.70)) {
    if (++rechazosSeguidos >= 3) { nLat = 0; idxLat = 0; rechazosSeguidos = 0; }
    return;
  }
  rechazosSeguidos = 0;
  lat[idxLat] = bpm; idxLat = (idxLat + 1) % NLAT; if (nLat < NLAT) nLat++;
  latidosAceptados++;
  float suma = 0; for (int i = 0; i < nLat; i++) suma += lat[i];
  fcProm = suma / nLat;
  if (nLat >= NLAT && latidosAceptados >= 6) { fc = round(fcProm); tUltimoLatido = millis(); }
}

void leerOximetro() {
  if (!oximetroOK) return;
  oximetro.check();
  while (oximetro.available()) {
    uint32_t rojo = oximetro.getFIFORed();
    uint32_t ir   = oximetro.getFIFOIR();
    oximetro.nextSample();
    nMuestra++;
    ultimoIR = ir;
    capturarFila(rojo, ir);                                  // AWS: también sin dedo (IR bajo)
    // Predicción: mismas unidades que el CSV del entrenamiento (m/s², rad/s).
    // nMuestra nunca salta, así que el extractor no se reinicia.
    prediccionPush(nMuestra, rojo, ir, ax * G_A_MS2, ay * G_A_MS2, az * G_A_MS2, gx, gy, gz);

    bool antes = hayDedo;
    if (!hayDedo && ir > IR_MIN_DEDO) hayDedo = true;
    else if (hayDedo && ir < IR_MIN_DEDO * 8 / 10) hayDedo = false;
    if (hayDedo != antes) {
      if (hayDedo) { muestraDedo = nMuestra; registrarEvento("Dedo detectado en el sensor"); }
      else registrarEvento("Sensor sin lectura (dedo retirado)");
    }
    if (!hayDedo) { reiniciarOximetro(); continue; }

    if (checkForBeat((int32_t)(ir >> 2))) procesarLatido();    // FC

    acIR += ir; acRojo += rojo;                              // SpO2: promedio de 4 -> 25 por segundo
    if (++k4 < 4) continue;
    uint32_t irP = acIR / 4, rojoP = acRojo / 4;
    acIR = acRojo = 0; k4 = 0;
    if (muestras < BUF) { bufRojo[muestras] = rojoP; bufIR[muestras] = irP; muestras++; }
    nuevas++;
    if (muestras == BUF && nuevas >= 25) {
      int32_t sp, h; int8_t sOk, hOk;
      maxim_heart_rate_and_oxygen_saturation(bufIR, BUF, bufRojo, &sp, &sOk, &h, &hOk);
      if (sOk && sp >= 70 && sp <= 100) { spo2 = sp; spo2Valida = true; tUltimaSpO2 = millis(); }
      memmove(bufRojo, bufRojo + 25, (BUF - 25) * sizeof(uint32_t));
      memmove(bufIR, bufIR + 25, (BUF - 25) * sizeof(uint32_t));
      muestras = BUF - 25; nuevas = 0;
    }
  }
  if (fc > 0 && millis() - tUltimoLatido > 6000) fc = 0;      // sin latidos válidos en 6 s
  if (millis() - tUltimaSpO2 > 6000) spo2Valida = false;
}

// Semáforo (R3)
const char* semaforoFC() {
  if (!hayDedo || fc == 0) return "gris";
  if (fc >= FC_VERDE_MIN && fc <= FC_VERDE_MAX) return "verde";
  if (fc >= FC_AMARILLO_MIN && fc <= FC_AMARILLO_MAX) return "amarillo";
  return "rojo";
}

// La predicción solo ADELANTA el semáforo de FC: verde -> amarillo si la FC prevista pasa de 120
bool fcPrevistaAlta() {
  return hayDedo && fc > 0 && !strcmp(semaforoFC(), "verde") && prediccionAmarillo();
}

// Registra en la pantalla cuándo empieza y termina un aviso de FC prevista alta
void procesarPrediccion() {
  static bool antes = false;
  bool ahora = fcPrevistaAlta();
  if (ahora && !antes) registrarEvento("FC prevista alta: %.0f lpm en 10 s", prediccionFcPrevista());
  else if (!ahora && antes) registrarEvento("FC prevista de vuelta en rango");
  antes = ahora;
}

const char* semaforoSpO2() {
  if (!hayDedo || !spo2Valida) return "gris";
  if (spo2 >= SPO2_VERDE_MIN) return "verde";
  if (spo2 >= SPO2_AMARILLO_MIN) return "amarillo";
  return "rojo";
}

// ============================================================================
// PANTALLA TFT ST7789 (versión 3)
// Distribución en horizontal (320 x 240):
//   y   0- 23  Encabezado: "PulsoSeguro", estado de Wi-Fi/AWS y pico de aceleración
//   y  28- 87  Estado: color según el estado, título grande y cuenta regresiva
//   y  94-177  Dos tarjetas: FC y SpO2 con el color del semáforo
//   y 184-213  FC prevista a +10 s y umbral de impacto (versión 6)
//   y 218-239  Último evento del registro
// ============================================================================
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
// Paleta de colores (tema oscuro)
const uint16_t C_FONDO    = rgb565(16, 20, 28);
const uint16_t C_TARJETA  = rgb565(42, 51, 66);
const uint16_t C_CABECERA = rgb565(26, 32, 43);
const uint16_t C_TEXTO    = rgb565(232, 236, 243);
const uint16_t C_SUB      = rgb565(152, 162, 179);
const uint16_t C_VERDE    = rgb565(22, 163, 74);
const uint16_t C_AMARILLO = rgb565(217, 154, 0);
const uint16_t C_ROJO     = rgb565(220, 38, 38);
const uint16_t C_GRIS     = rgb565(102, 112, 133);
const uint16_t C_AZUL     = rgb565(37, 99, 235);
const uint16_t C_BLANCO   = 0xFFFF;
const uint16_t C_NEGRO    = 0x0000;

// La fuente de Adafruit GFX usa la tabla CP437; esta función traduce los
// caracteres del español (á é í ó ú ñ ¿ ¡ ° ·) para que se vean bien.
void aCP437(const char* in, char* out, size_t tam) {
  size_t i = 0, j = 0;
  while (in[i] && j < tam - 1) {
    uint8_t c = (uint8_t)in[i];
    if (c < 0x80) { out[j++] = (char)c; i++; continue; }
    size_t ini = i++;
    while (((uint8_t)in[i] & 0xC0) == 0x80) i++;         // salta los bytes de continuación
    size_t n = i - ini;
    uint8_t b1 = n > 1 ? (uint8_t)in[ini + 1] : 0, b2 = n > 2 ? (uint8_t)in[ini + 2] : 0;
    uint8_t r = '?';
    if (c == 0xC3) {
      switch (b1) {
        case 0xA1: r = 0xA0; break;  // á
        case 0xA9: r = 0x82; break;  // é
        case 0xAD: r = 0xA1; break;  // í
        case 0xB3: r = 0xA2; break;  // ó
        case 0xBA: r = 0xA3; break;  // ú
        case 0xB1: r = 0xA4; break;  // ñ
        case 0x91: r = 0xA5; break;  // Ñ
        case 0xBC: r = 0x81; break;  // ü
        case 0x89: r = 0x90; break;  // É
        case 0x81: r = 'A'; break;   // Á, Í, Ó, Ú no existen en CP437
        case 0x8D: r = 'I'; break;
        case 0x93: r = 'O'; break;
        case 0x9A: r = 'U'; break;
      }
    } else if (c == 0xC2) {
      switch (b1) {
        case 0xBF: r = 0xA8; break;  // ¿
        case 0xA1: r = 0xAD; break;  // ¡
        case 0xB0: r = 0xF8; break;  // °
        case 0xB7: r = 0xFA; break;  // ·
      }
    } else if (c == 0xE2 && b1 == 0x82 && b2 == 0x82) {
      r = '2';                       // ₂
    } else if (c == 0xE2 && b1 == 0x80 && b2 == 0xA6) {
      r = '.';                       // … -> "..."
      if (j < tam - 3) { out[j++] = '.'; out[j++] = '.'; }
    }
    out[j++] = (char)r;
  }
  out[j] = 0;
}

// Escribe en dest un texto con formato, ya convertido a CP437
void ponerTexto(char* dest, size_t tam, const char* formato, ...) {
  char tmp[128];
  va_list args;
  va_start(args, formato);
  vsnprintf(tmp, sizeof(tmp), formato, args);
  va_end(args);
  aCP437(tmp, dest, tam);
}

// Dibujo de texto (la fuente básica mide 6 x 8 px multiplicado por el tamaño)
void texto(int x, int y, const char* s, uint8_t tam, uint16_t color, uint16_t fondo) {
  tft.setTextSize(tam);
  tft.setTextColor(color, fondo);
  tft.setCursor(x, y);
  tft.print(s);
}
void textoCentrado(int xc, int y, const char* s, uint8_t tam, uint16_t color, uint16_t fondo) {
  texto(xc - (int)strlen(s) * 6 * tam / 2, y, s, tam, color, fondo);
}
void textoDerecha(int xd, int y, const char* s, uint8_t tam, uint16_t color, uint16_t fondo) {
  texto(xd - (int)strlen(s) * 6 * tam, y, s, tam, color, fondo);
}

enum { R_ENCABEZADO, R_ESTADO, R_FC, R_SPO2, R_PREDICCION, R_EVENTO, NUM_REGIONES };
ContenidoRegion deseado[NUM_REGIONES], dibujado[NUM_REGIONES];
bool regionInvalida[NUM_REGIONES];
unsigned long dibujoMaxUs = 0;                           // para la prueba P19

uint16_t colorSemaforo(const char* s) {
  if (!strcmp(s, "verde")) return C_VERDE;
  if (!strcmp(s, "amarillo")) return C_AMARILLO;
  if (!strcmp(s, "rojo")) return C_ROJO;
  return C_GRIS;
}

const char* etiquetaSemaforo(const char* s) {
  if (!strcmp(s, "verde")) return "Normal";
  if (!strcmp(s, "amarillo")) return "Irregular";
  if (!strcmp(s, "rojo")) return "Crítico";
  return "Sin lectura";
}

void calcularSigno(ContenidoRegion& r, const char* sem, int valor, bool hayValor) {
  r.fondo = colorSemaforo(sem);
  r.color = (r.fondo == C_AMARILLO) ? C_NEGRO : C_BLANCO;
  if (hayValor) ponerTexto(r.l1, sizeof(r.l1), "%d", valor);
  else ponerTexto(r.l1, sizeof(r.l1), "--");
  if (!hayDedo) ponerTexto(r.l2, sizeof(r.l2), "Sin dedo");
  else if (!hayValor) ponerTexto(r.l2, sizeof(r.l2), "Midiendo…");
  else ponerTexto(r.l2, sizeof(r.l2), "%s", etiquetaSemaforo(sem));
}

// Calcula lo que debe mostrar cada región con el estado actual del sistema
void calcularVista() {
  // Encabezado
  ContenidoRegion& e = deseado[R_ENCABEZADO];
  e.fondo = C_CABECERA; e.color = C_TEXTO;
  ponerTexto(e.l1, sizeof(e.l1), "Wi-Fi %s · AWS %s", awsWifi ? "OK" : "--", estadoAWS());
  ponerTexto(e.l2, sizeof(e.l2), "Pico: %.2f g", magPicoUltimo);

  // Estado
  ContenidoRegion& s = deseado[R_ESTADO];
  long rest = segundosRestantes();
  switch (estado) {
    case CONFIRMACION:
      s.fondo = C_AMARILLO;
      ponerTexto(s.l1, sizeof(s.l1), "¿CAÍDA?");
      ponerTexto(s.l2, sizeof(s.l2), "Presiona el botón · %ld s", rest);
      break;
    case ALARMA:
      s.fondo = C_ROJO;
      ponerTexto(s.l1, sizeof(s.l1), alarmaPorPanico ? "¡PÁNICO!" : "¡CAÍDA!");
      if (alarmaSonando()) ponerTexto(s.l2, sizeof(s.l2), "Alarma sonando · %ld s", rest);
      else ponerTexto(s.l2, sizeof(s.l2), "Alarma sin atender");
      break;
    case ESPERA:
      s.fondo = C_AZUL;
      ponerTexto(s.l1, sizeof(s.l1), "EN ESPERA");
      ponerTexto(s.l2, sizeof(s.l2), "Reanuda en %ld s", rest);
      break;
    default:
      s.fondo = C_VERDE;
      ponerTexto(s.l1, sizeof(s.l1), "NORMAL");
      if (mpuOK && oximetroOK) ponerTexto(s.l2, sizeof(s.l2), "Monitoreando");
      else ponerTexto(s.l2, sizeof(s.l2), "%s sin respuesta", !mpuOK ? "MPU6050" : "MAX30102");
      break;
  }
  s.color = (s.fondo == C_AMARILLO) ? C_NEGRO : C_BLANCO;

  // Semáforo (R3)
  bool adelantada = fcPrevistaAlta();
  calcularSigno(deseado[R_FC], adelantada ? "amarillo" : semaforoFC(), fc, hayDedo && fc > 0);
  if (adelantada) ponerTexto(deseado[R_FC].l2, sizeof(deseado[R_FC].l2), "Prev. alta");
  calcularSigno(deseado[R_SPO2], semaforoSpO2(), spo2, hayDedo && spo2Valida);

  // FC prevista a +10 s (versión 6) y umbral de impacto (fijo desde la versión 5)
  ContenidoRegion& p = deseado[R_PREDICCION];
  p.fondo = C_FONDO;
  p.color = prediccionAmarillo() ? C_AMARILLO : C_TEXTO;
  float fp = prediccionFcPrevista();                     // NAN si no hay una vigente (< 10 s)
  if (!isnan(fp)) ponerTexto(p.l1, sizeof(p.l1), "FC en 10 s: %.0f lpm", fp);
  else ponerTexto(p.l1, sizeof(p.l1), "FC en 10 s: --");
  ponerTexto(p.l2, sizeof(p.l2), "Umbral de impacto: %.2f g", UMBRAL_IMPACTO_G);

  // Último evento (dos renglones de 52 caracteres)
  ContenidoRegion& v = deseado[R_EVENTO];
  v.fondo = C_FONDO; v.color = C_SUB;
  char ev[128] = "";
  if (numEventos > 0) {
    int k = (idxEvento - 1 + MAX_EVENTOS) % MAX_EVENTOS;
    ponerTexto(ev, sizeof(ev), "> %s", eventos[k].msg);
  }
  strncpy(v.l1, ev, 52); v.l1[52] = 0;
  strncpy(v.l2, strlen(ev) > 52 ? ev + 52 : "", 52); v.l2[52] = 0;
}

// --- Dibujo de cada región ---
void dibujarEncabezado(const ContenidoRegion& n, const ContenidoRegion& v, bool completo) {
  if (completo) {
    tft.fillRect(0, 0, 320, 24, n.fondo);
    texto(6, 4, "PulsoSeguro", 2, n.color, n.fondo);
  }
  if (completo || strcmp(n.l1, v.l1) || strcmp(n.l2, v.l2)) {
    tft.fillRect(170, 1, 148, 22, n.fondo);
    textoDerecha(314, 3, n.l1, 1, C_SUB, n.fondo);
    textoDerecha(314, 13, n.l2, 1, C_SUB, n.fondo);
  }
}

void dibujarEstado(const ContenidoRegion& n, const ContenidoRegion& v, bool completo) {
  const int X = 4, Y = 28, W = 312, H = 60;
  if (completo || strcmp(n.l1, v.l1)) {
    tft.fillRoundRect(X, Y, W, H, 8, n.fondo);
    textoCentrado(160, Y + 4, n.l1, 4, n.color, n.fondo);
    textoCentrado(160, Y + 40, n.l2, 2, n.color, n.fondo);
  } else if (strcmp(n.l2, v.l2)) {                        // solo la cuenta regresiva
    tft.fillRect(X + 6, Y + 38, W - 12, 20, n.fondo);
    textoCentrado(160, Y + 40, n.l2, 2, n.color, n.fondo);
  }
}

void dibujarSigno(int x, const char* titulo, const ContenidoRegion& n, const ContenidoRegion& v, bool completo) {
  const int Y = 94, W = 154, H = 84;
  if (completo) {
    tft.fillRoundRect(x, Y, W, H, 8, n.fondo);
    textoCentrado(x + W / 2, Y + 6, titulo, 2, n.color, n.fondo);
  }
  if (completo || strcmp(n.l1, v.l1)) {
    tft.fillRect(x + 4, Y + 26, W - 8, 34, n.fondo);
    textoCentrado(x + W / 2, Y + 28, n.l1, 4, n.color, n.fondo);
  }
  if (completo || strcmp(n.l2, v.l2)) {
    tft.fillRect(x + 4, Y + 62, W - 8, 20, n.fondo);
    textoCentrado(x + W / 2, Y + 64, n.l2, 2, n.color, n.fondo);
  }
}

void dibujarPrediccion(const ContenidoRegion& n, const ContenidoRegion& v, bool completo) {
  if (completo || strcmp(n.l1, v.l1)) {                   // y 184-201: FC prevista (tamaño 2)
    tft.fillRect(4, 184, 312, 18, n.fondo);
    textoCentrado(160, 186, n.l1, 2, n.color, n.fondo);
  }
  if (completo || strcmp(n.l2, v.l2)) {                   // y 204-213: umbral (tamaño 1)
    tft.fillRect(4, 204, 312, 10, n.fondo);
    textoCentrado(160, 205, n.l2, 1, C_SUB, n.fondo);
  }
}

void dibujarEvento(const ContenidoRegion& n, const ContenidoRegion& v, bool completo) {
  if (completo) tft.drawFastHLine(4, 218, 312, C_TARJETA);
  if (completo || strcmp(n.l1, v.l1) || strcmp(n.l2, v.l2)) {
    tft.fillRect(4, 221, 312, 18, n.fondo);
    texto(4, 221, n.l1, 1, n.color, n.fondo);
    texto(4, 231, n.l2, 1, n.color, n.fondo);
  }
}

void dibujarRegion(int i) {
  const ContenidoRegion& n = deseado[i];
  const ContenidoRegion& v = dibujado[i];
  bool completo = regionInvalida[i] || n.fondo != v.fondo || n.color != v.color;
  switch (i) {
    case R_ENCABEZADO: dibujarEncabezado(n, v, completo); break;
    case R_ESTADO:     dibujarEstado(n, v, completo); break;
    case R_FC:         dibujarSigno(4,   "FC (lpm)", n, v, completo); break;
    case R_SPO2:       dibujarSigno(162, "SpO2 (%)", n, v, completo); break;
    case R_PREDICCION: dibujarPrediccion(n, v, completo); break;
    case R_EVENTO:     dibujarEvento(n, v, completo); break;
  }
  dibujado[i] = n;
  regionInvalida[i] = false;
}

bool regionPendiente(int i) {
  return regionInvalida[i] || !(deseado[i] == dibujado[i]);
}

// Se llama en cada vuelta del loop(); dibuja como máximo UNA región por llamada
void actualizarPantalla() {
  static unsigned long tCalculo = 0, tDibujo = 0;
  static int siguiente = 0;
  if (faseCaida != FC_REPOSO) return;     // no robar tiempo al análisis de un posible impacto

  unsigned long ahora = millis();
  if (ahora - tCalculo >= TFT_CALCULO_MS) { tCalculo = ahora; calcularVista(); }
  if (ahora - tDibujo < TFT_PAUSA_MS) return;

  int elegida = -1;
  if (regionPendiente(R_ESTADO)) elegida = R_ESTADO;     // el estado tiene prioridad
  for (int k = 0; elegida < 0 && k < NUM_REGIONES; k++) {
    int i = (siguiente + k) % NUM_REGIONES;
    if (regionPendiente(i)) elegida = i;
  }
  if (elegida < 0) return;

  unsigned long t0 = micros();
  dibujarRegion(elegida);
  unsigned long dt = micros() - t0;
  if (dt > dibujoMaxUs) dibujoMaxUs = dt;
  tDibujo = millis();
  siguiente = (elegida + 1) % NUM_REGIONES;
}

void iniciarPantalla() {
  pinMode(PIN_TFT_BLK, OUTPUT);
  digitalWrite(PIN_TFT_BLK, HIGH);                       // retroiluminación encendida
  SPI.begin(PIN_TFT_SCK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(40000000);                             // si hay ruido en la imagen, bajar a 20000000
  tft.setRotation(1);                                    // horizontal: 320 x 240
  tft.invertDisplay(false);
  tft.cp437(true);                                       // tabla correcta para los acentos
  tft.setTextWrap(false);

  // Pantalla de arranque (en setup sí se puede usar fillScreen)
  tft.fillScreen(C_FONDO);
  textoCentrado(160, 90, "PulsoSeguro", 3, C_TEXTO, C_FONDO);
  char buf[32];
  ponerTexto(buf, sizeof(buf), "Iniciando…");
  textoCentrado(160, 130, buf, 2, C_SUB, C_FONDO);
}

// Borra la pantalla de arranque; las regiones se dibujan solas en el loop()
void mostrarInterfaz() {
  tft.fillScreen(C_FONDO);
  for (int i = 0; i < NUM_REGIONES; i++) regionInvalida[i] = true;
  calcularVista();
}

// Resumen por el Monitor Serie cada 2 s (para las pruebas)
unsigned long cicloMaxUs = 0;

void imprimirResumen() {
  static unsigned long t = 0;
  if (millis() - t < 2000) return;
  t = millis();
  Serial.printf("[%lu ms] %s | FC %s | SpO2 %s | dedo %s (IR %lu) | |a| %.2f g | umbral %.2f g | TFT max %.1f ms | ciclo max %.1f ms\n",
                millis(), nombreEstado(),
                fc > 0 ? String(fc).c_str() : "--",
                (hayDedo && spo2Valida) ? String(spo2).c_str() : "--",
                hayDedo ? "SI" : "NO", (unsigned long)ultimoIR,
                magnitudG, UMBRAL_IMPACTO_G,
                dibujoMaxUs / 1000.0, cicloMaxUs / 1000.0);
  Serial.printf("            AWS: wifi %s | mqtt %s | captura %s (%lu muestras) | lotes enviados %lu | descartadas %lu | intentos %lu | heap %u (bloque max %u, mínimo %u)\n",
                awsWifi ? "OK" : "--", awsConectado ? "OK" : "--", capturando ? "SI" : "NO",
                (unsigned long)seqCaptura, (unsigned long)awsLotesEnviados,
                (unsigned long)awsMuestrasDescartadas, (unsigned long)awsIntentos,
                (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)ESP.getMinFreeHeap());
  pred::Estadisticas pe = prediccionEstadisticas();
  float fp = prediccionFcPrevista();
  Serial.printf("            PRED: ventanas %lu (válidas %lu) | features enviadas %lu | descartadas %lu | respuestas %lu | FC en 10 s %s | cálculo max %.1f ms | pila libre %lu B\n",
                (unsigned long)pe.ventanas, (unsigned long)pe.validas, (unsigned long)pe.enviadas,
                (unsigned long)pe.descartadas, (unsigned long)pe.recibidas,
                isnan(fp) ? "--" : String(fp, 0).c_str(), pe.calcMaxUs / 1000.0, (unsigned long)pe.pilaLibre);
  dibujoMaxUs = 0;
  cicloMaxUs = 0;
}

// ----------------------------------------------------------------------------
// setup y loop
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n===== PulsoSeguro (versión 6: pantalla TFT + AWS IoT + predicción de FC) =====");

  // Predicción: ~31 KB en el heap. Se reserva primero, antes de Wi-Fi y TLS, para encontrar
  // un bloque contiguo. Si no hay memoria, todo lo demás funciona igual, sin FC prevista.
  bool prediccionOK = PREDICCION_ACTIVA && prediccionInit();   // tarea "features" en el núcleo 0
  Serial.printf("Predicción de FC: %s | heap libre %u | bloque max %u\n",
                prediccionOK ? "OK" : (PREDICCION_ACTIVA ? "SIN MEMORIA" : "DESACTIVADA"),
                (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  pinMode(PIN_BOTON, INPUT_PULLUP);

  if (BUZZER_PASIVO) ledcAttach(PIN_BUZZER, 2000, 8);
  else pinMode(PIN_BUZZER, OUTPUT);
  buzzerTono(0);

  iniciarPantalla();

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  mpuOK = iniciarMPU();
  oximetroOK = iniciarOximetro();
  Serial.printf("MPU6050: %s | MAX30102: %s\n", mpuOK ? "OK" : "ERROR", oximetroOK ? "OK" : "ERROR");

  WiFi.mode(WIFI_STA);                                   // solo estación: red con internet para AWS
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);                      // no bloquea: la tarea AWS espera la conexión
  Serial.printf("Conectando a la red con internet \"%s\" para AWS IoT\n", WIFI_SSID);
  iniciarAWS();

  // Autoprueba: un pitido
  buzzerTono(2000); delay(120); buzzerTono(0);

  registrarEvento("Sistema iniciado");
  if (PREDICCION_ACTIVA && !prediccionOK) registrarEvento("Predicción desactivada: sin memoria");
  cambiarEstado(NORMAL);
  mostrarInterfaz();
}

void loop() {
  static unsigned long tPrev = 0;
  unsigned long us = micros();
  if (tPrev && us - tPrev > cicloMaxUs) cicloMaxUs = us - tPrev;   // duración de la vuelta anterior
  tPrev = us;

  leerOximetro();              // MAX30102 (R3)
  detectarCaida();             // MPU6050 (R1)
  leerBoton();                 // R4, R5
  actualizarEstados();         // R2 y espera de 10 s
  actualizarBuzzer();
  actualizarPantalla();        // TFT: una región por vuelta, como máximo
  procesarComandosAWS();       // start / stop recibidos por MQTT
  procesarPrediccion();        // evento al empezar/terminar un aviso de FC prevista alta
  imprimirResumen();
}