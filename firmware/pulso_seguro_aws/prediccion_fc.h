// prediccion_fc.h · Pegamento entre features_fc.h, FreeRTOS y MQTT para PulsoSeguro.
//
// Qué hace:
//   núcleo 1  → prediccionPush(...) en cada muestra (100 Hz). O(1), no bloquea.
//   núcleo 0  → tarea "features": cada 2 s calcula las 16 features (pocos ms) y, si la
//               ventana es válida, deja el JSON en una cola.
//   tarea MQTT → prediccionPublicar(mqtt) vacía la cola hacia bio/<thing>/features;
//               prediccionRecibir(...) procesa la respuesta de la Lambda (bio/<thing>/pred).
//   TFT / web → prediccionFcPrevista(), prediccionSemaforo(), prediccionVigente().
//
// La alerta crítica NO depende de esto: si no hay internet, simplemente no hay "FC prevista".
// Ver INTEGRACION_FIRMWARE.md para los 5 puntos donde se engancha al .ino.
#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include <new>
#include "features_fc.h"

namespace pred {

static fcfeat::Extractor* ext = nullptr;      // ~31 KB en el heap: en .bss no cabe junto a los lotes
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t tarea = nullptr;
static QueueHandle_t cola = nullptr;          // mensajes JSON listos para publicar
static char bootId[9];

struct Msg { char json[360]; };

static volatile float fcPrevista = NAN;
static volatile bool  amarillo = false;
static volatile uint32_t recibidaMs = 0;

// Contadores para el Monitor Serie (se leen con prediccionEstadisticas)
struct Estadisticas {
  uint32_t ventanas, validas, enviadas, descartadas, recibidas, calcMaxUs, pilaLibre;
};
static volatile uint32_t nVentanas = 0, nValidas = 0, nEnviadas = 0, nDescartadas = 0, nRecibidas = 0;
static volatile uint32_t calcMaxUs = 0;

static void tareaFeatures(void*) {
  static fcfeat::Fila f;
  static Msg m;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // dormida hasta que push() avise
    portENTER_CRITICAL(&mux);
    ext->snapshot();                          // ~3200 copias: decenas de µs
    portEXIT_CRITICAL(&mux);
    uint32_t t0 = micros();
    ext->calcular(f);
    uint32_t dt = micros() - t0;
    if (dt > calcMaxUs) calcMaxUs = dt;
    nVentanas = nVentanas + 1;
    if (!f.valida) continue;                  // la Lambda solo recibe ventanas con FC confiable
    nValidas = nValidas + 1;

    int n = snprintf(m.json, sizeof(m.json), "{\"v\":%d,\"boot\":\"%s\",\"win\":%lu,\"t_ms\":%lu,\"f\":[",
                     fcfeat::VERSION_FEATURES, bootId, (unsigned long)f.win, (unsigned long)millis());
    for (int i = 0; i < fcfeat::N_FEATURES && n < (int)sizeof(m.json) - 16; i++)
      n += snprintf(m.json + n, sizeof(m.json) - n, i ? ",%.4f" : "%.4f", (double)f.f[i]);
    snprintf(m.json + n, sizeof(m.json) - n, "]}");
    if (xQueueSend(cola, &m, 0) != pdTRUE) nDescartadas = nDescartadas + 1;   // cola llena (sin red)
  }
}

}  // namespace pred

// ------------------------------------------------------------------ API
// Devuelve false si no hubo memoria: la pulsera sigue funcionando, solo sin predicción.
// Llamarla al principio de setup(), antes de Wi-Fi y TLS, para encontrar un bloque contiguo.
inline bool prediccionInit() {
  pred::ext = new (std::nothrow) fcfeat::Extractor();
  if (!pred::ext) return false;
  snprintf(pred::bootId, sizeof(pred::bootId), "%08lx", (unsigned long)esp_random());
  pred::cola = xQueueCreate(3, sizeof(pred::Msg));
  // 4 KB de pila bastan (el trabajo pesado vive en el heap); el resumen serie muestra lo que sobra
  xTaskCreatePinnedToCore(pred::tareaFeatures, "features", 4096, nullptr, 1, &pred::tarea, 0);
  return true;
}

// Llamar en el núcleo 1, donde se arma cada muestra (mismas unidades que el CSV: m/s², rad/s).
inline void prediccionPush(uint32_t seq, float red, float ir, float ax, float ay, float az,
                           float gx, float gy, float gz) {
  if (!pred::ext) return;
  portENTER_CRITICAL(&pred::mux);
  bool lista = pred::ext->push(seq, red, ir, ax, ay, az, gx, gy, gz);
  portEXIT_CRITICAL(&pred::mux);
  if (lista && pred::tarea) xTaskNotifyGive(pred::tarea);
}

// Llamar en la tarea MQTT, después de mqtt.loop(), solo si mqtt.connected().
inline void prediccionPublicar(PubSubClient& mqtt, const char* thing) {
  static pred::Msg m;
  static char topico[64];
  if (!pred::cola) return;                    // predicción desactivada o sin memoria
  snprintf(topico, sizeof(topico), "bio/%s/features", thing);
  while (xQueueReceive(pred::cola, &m, 0) == pdTRUE) {
    if (mqtt.publish(topico, m.json)) pred::nEnviadas = pred::nEnviadas + 1;
    else pred::nDescartadas = pred::nDescartadas + 1;
  }
}

// Suscribirse al conectar:  mqtt.subscribe("bio/<thing>/pred");
// En el callback MQTT, si el tópico termina en "/pred":
inline void prediccionRecibir(const byte* payload, unsigned int len) {
  char buf[256];
  len = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, payload, len); buf[len] = 0;
  const char* p = strstr(buf, "\"fc_prevista\":");
  if (!p) return;
  pred::fcPrevista = atof(p + 14);                 // atof salta espacios
  // Tolera `"semaforo":"amarillo"` y `"semaforo": "amarillo"`
  const char* s = strstr(buf, "\"semaforo\":");
  if (s) { s += 11; while (*s == ' ') s++; }
  pred::amarillo = s && strncmp(s, "\"amarillo\"", 10) == 0;
  pred::recibidaMs = millis();
  pred::nRecibidas = pred::nRecibidas + 1;
}

// La predicción caduca a los 10 s (es justo su horizonte): después no se muestra.
inline bool  prediccionVigente()     { return pred::recibidaMs && millis() - pred::recibidaMs < 10000; }
inline float prediccionFcPrevista()  { return prediccionVigente() ? pred::fcPrevista : NAN; }
inline bool  prediccionAmarillo()    { return prediccionVigente() && pred::amarillo; }

// Copia de los contadores; calcMaxUs se reinicia en cada lectura (máximo por intervalo)
inline pred::Estadisticas prediccionEstadisticas() {
  pred::Estadisticas e = {pred::nVentanas, pred::nValidas, pred::nEnviadas,
                          pred::nDescartadas, pred::nRecibidas, pred::calcMaxUs,
                          pred::tarea ? (uint32_t)uxTaskGetStackHighWaterMark(pred::tarea) : 0};
  pred::calcMaxUs = 0;
  return e;
}
