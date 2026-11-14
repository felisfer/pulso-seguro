# Integración en `pulso_seguro_aws.ino` (v4 → v5)

`pulso_seguro_aws.ino` (versión 6) ya trae estos enganches aplicados y está compilado con el core esp32 3.3.12 (partición *Huge APP*). Esta guía explica qué se cambió, por si lo integras en otra versión del firmware. Nada del algoritmo de caídas cambia.

```
núcleo 1 (100 Hz)                     núcleo 0
┌──────────────────────┐   notifica   ┌─────────────────────────┐   cola (3)   ┌──────────────────────┐
│ lectura MAX + MPU    │ ───────────► │ tarea "features" (pr 1) │ ───────────► │ tarea MQTT           │
│ prediccionPush(...)  │  cada 200    │ snapshot + calcular     │  JSON ~230 B │ prediccionPublicar() │
└──────────────────────┘   muestras   └─────────────────────────┘              │ callback → /pred     │
          ▲                                                                    └──────────┬───────────┘
          └──────────── prediccionFcPrevista() / prediccionAmarillo() ◄──────────────────┘
```

## 1. Includes (arriba, junto a los demás)

```cpp
#include "prediccion_fc.h"   // incluye features_fc.h
```

## 2. `setup()` — al principio, antes de Wi-Fi y TLS

```cpp
bool prediccionOK = prediccionInit();   // reserva ~31 KB en el heap, crea la tarea y la cola
```

El extractor **no cabe como variable global** en el ESP32-WROOM: la v5 ya usa 108 KB de DRAM estática (los lotes ocupan ~50 KB) y el enlazador falla con `region 'dram0_0_seg' overflowed`. Por eso `prediccionInit()` lo reserva en el heap. Llámala antes de Wi-Fi para encontrar un bloque contiguo; si devuelve `false`, la pulsera funciona igual, solo sin FC prevista.

## 3. Donde se arma cada muestra (núcleo 1)

En el mismo lugar donde hoy llenas la `Muestra` que va al lote CSV. **Mismas unidades que el CSV** (m/s² y rad/s), porque el modelo se entrenó con ellas:

```cpp
prediccionPush(m.seq, m.red, m.ir, m.ax, m.ay, m.az, m.gx, m.gy, m.gz);
```

Debe llamarse **siempre**, haya o no sesión de captura activa: la predicción no depende de `start/stop`. Si un día dejas de llamarla durante un rato, el salto de `seq` reinicia el extractor solo.

## 4. Tarea MQTT

```cpp
// al (re)conectar, junto al subscribe de /cmd:
mqtt.subscribe("bio/" THING_NAME "/pred");

// en el ciclo, después de mqtt.loop():
if (mqtt.connected()) prediccionPublicar(mqtt, THING_NAME);
```

En el callback:

```cpp
void alRecibir(char* topic, byte* payload, unsigned int len) {
  size_t n = strlen(topic);
  if (n > 5 && strcmp(topic + n - 5, "/pred") == 0) { prediccionRecibir(payload, len); return; }
  // ... lo que ya tienes para /cmd
}
```

> `THING_NAME` como literal de cadena viene de `secrets.h`. Si es una variable `const char*`, arma el tópico con `snprintf`.

## 5. Pantalla y dashboard

```cpp
float fp = prediccionFcPrevista();          // NAN si no hay predicción vigente (< 10 s)
if (!isnan(fp)) tft.printf("Prev: %3.0f", fp); else tft.print("Prev: --");

// semáforo: la predicción solo puede ADELANTAR el amarillo, nunca quitar un rojo
if (estado == VERDE && prediccionAmarillo()) estado = AMARILLO;
```

En el JSON del dashboard local agrega `"fc_prevista": isnan(fp) ? null : fp`.

## Comprobaciones

| Qué | Cómo |
|---|---|
| Memoria | v6 compilada: 89 068 bytes de variables globales (v5: 107 980). Para que el handshake TLS tenga memoria, `N_LOTES` bajó de 5 a 3 (~4.5 s sin red) y la tarea de features usa 4 KB de pila. El extractor toma ~31 KB del heap al arrancar. Si TLS falla con `BIGNUM - Memory allocation failed`, pon `PREDICCION_ACTIVA = false` para confirmar que es memoria y revisa `bloque max` en el Monitor Serie |
| Tiempo | La línea `PRED:` del Monitor Serie muestra `cálculo max` cada 2 s: debe quedar muy por debajo de 2000 ms (esperado: unos pocos ms) |
| Buffer MQTT | Tus lotes de 10 KB ya obligaron a `mqtt.setBufferSize(...)` grande; el mensaje de features cabe de sobra |
| Mensajes | En el cliente de prueba MQTT de la consola: `bio/+/features` debe mostrar 1 mensaje cada 2 s con el dedo puesto, y `bio/+/pred` la respuesta ~100-300 ms después |
