/*
 * ============================================================================
 * PulsoSeguro - Pulsera IoT de monitoreo de signos vitales y detección de caídas
 * Tecnologías Computacionales II - Facultad de Ingeniería, UAEMéx - 2026B
 * Versión 3: pantalla TFT 2.8" ST7789 integrada (dibujo por regiones, sin bloqueos)
 * ============================================================================
 *
 * Hardware (ESP32-WROOM-32):
 *    MPU6050 (I2C 0x68) ........ SDA = GPIO 21, SCL = GPIO 22
 *    MAX30102 (I2C 0x57) ........ mismo bus I2C (SDA 21, SCL 22)
 *    Potenciómetro 10 kΩ ........ GPIO 34 (ADC1, solo entrada)
 *    Push button ................ GPIO 27 -> GND (INPUT_PULLUP)
 *    LED RGB (cátodo común) ..... R = GPIO 25, G = GPIO 26, B = GPIO 33 (con resistencias de 220 Ω)
 *    Buzzer pasivo .............. GPIO 13
 *    TFT ST7789 240x320 (SPI) ... CS = GPIO 15, DC = GPIO 16, RST = GPIO 4,
 *                                 MOSI = GPIO 23, SCK = GPIO 18, BLK = GPIO 32, VCC = 3V3
 *    Cambio respecto a la prueba de la pantalla: DC pasa de GPIO 2 a GPIO 16, porque el
 *    GPIO 2 es un strapping pin (debe estar en LOW/flotante al arrancar para poder cargar código).
 *
 * Comunicación IoT:
 *    El ESP32 crea un punto de acceso Wi-Fi. Desde el celular o la computadora
 *    conectarse a la red "PulsoSeguro-ESP32" y abrir http://192.168.4.1
 *
 * Captura de datos a AWS IoT Core (versión 4):
 *    - Wi-Fi en modo AP + STA: el punto de acceso y el dashboard siguen igual, y al
 *      mismo tiempo el ESP32 se conecta a una red con internet (WIFI_SSID en secrets.h).
 *      Nota: el AP adopta el canal del router; al conectarse, el celular puede
 *      desconectarse un momento del AP y reconectarse solo.
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
 * Reglas automatizadas:
 *    R1 Detección de caída en 4 fases: caída libre -> impacto -> inmovilidad -> cambio de orientación
 *    R2 Ventana de confirmación de 15 s; si no se cancela, alarma completa (máximo 60 s de sonido)
 *    R3 Semáforo de FC y SpO2 en el dashboard y en la pantalla (verde/amarillo/rojo)
 *    R4 Pulsación corta (< 1 s): silencia / cancela + pitido corto
 *    R5 Pulsación larga (3 s): botón de pánico
 *    R6 Potenciómetro: ajusta la sensibilidad de caída (1.5 a 2.5 g) y el LED muestra el nivel 2 s
 *    R7 Botón "Silenciar" en el dashboard (misma acción que la pulsación corta)
 *    Tras silenciar se esperan 10 s antes de volver a detectar caídas.
 *
 * Pantalla (versión 3):
 *    - Muestra el estado, la cuenta regresiva, FC y SpO2 con el color del semáforo,
 *      la sensibilidad y el último evento.
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
#include <WebServer.h>
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
#include "secrets.h"           // WIFI_SSID, WIFI_PASS, THING_NAME, AWS_ENDPOINT y certificados

// ----------------------------------------------------------------------------
// Configuración general
// ----------------------------------------------------------------------------
const char* AP_SSID     = "PulsoSeguro-ESP32";
const char* AP_PASSWORD = "123456789";          // mínimo 8 caracteres

const bool LED_ANODO_COMUN = false;             // P8: el LED es de cátodo común
const bool BUZZER_PASIVO   = true;              // P8: el buzzer es pasivo

// Pines
const int PIN_SDA    = 21;
const int PIN_SCL    = 22;
const int PIN_POT    = 34;
const int PIN_BOTON  = 27;
const int PIN_LED_R  = 25;
const int PIN_LED_G  = 26;
const int PIN_LED_B  = 33;
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
const unsigned long MOSTRAR_NIVEL_POT_MS    = 2000;    // R6
const unsigned long ANTIRREBOTE_MS          = 30;      // P7: 0 rebotes observados

// Detección de caída (R1) - ajustada con la prueba P3
const float DISPARO_G              = 1.5;     // a partir de aquí se analiza el movimiento
const float CAIDA_LIBRE_G          = 0.7;     // fase 1: mínimo antes del impacto
const unsigned long CAIDA_LIBRE_MAX_MS = 800; //          tiempo máximo entre caída libre e impacto
const float IMPACTO_MIN_G          = 1.5;     // fase 2: umbral con sensibilidad máxima (potenciómetro 100 %)
const float IMPACTO_MAX_G          = 2.5;     //          umbral con sensibilidad mínima (potenciómetro 0 %)
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
WebServer server(80);
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

// Registro de eventos para el dashboard y la pantalla
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
// Página web del dashboard (guardada en la memoria del ESP32).
// No usa librerías externas porque el celular no tiene internet en esta red.
// ----------------------------------------------------------------------------
const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PulsoSeguro</title>
<link rel="icon" href="data:,">
<style>
:root{--bg:#f3f5f8;--card:#fff;--txt:#1d2433;--sub:#667085;--borde:#e3e7ee;
--verde:#16a34a;--amarillo:#d99a00;--rojo:#dc2626;--gris:#98a2b3;--azul:#2563eb;}
@media (prefers-color-scheme: dark){:root{--bg:#10141c;--card:#1a202b;--txt:#e8ecf3;--sub:#98a2b3;--borde:#2a3342;}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{padding:16px;display:flex;justify-content:space-between;align-items:center}
h1{font-size:20px;margin:0}
#conn{font-size:12px;padding:4px 10px;border-radius:99px;background:var(--gris);color:#fff}
#conn.ok{background:var(--verde)}
main{display:grid;gap:12px;padding:0 16px 24px;max-width:900px;margin:0 auto;grid-template-columns:1fr}
@media (min-width:700px){main{grid-template-columns:1fr 1fr}.ancho{grid-column:1/-1}}
.card{background:var(--card);border:1px solid var(--borde);border-radius:14px;padding:14px}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:.05em;color:var(--sub);margin:0 0 10px}
#estado{color:#fff;text-align:center}
#estado.NORMAL{background:var(--verde)}#estado.ESPERA{background:var(--azul)}
#estado.CONFIRMACION{background:var(--amarillo)}#estado.ALARMA{background:var(--rojo);animation:parpadeo 1s infinite}
@keyframes parpadeo{50%{opacity:.75}}
#estado h2{color:#fff;opacity:.85}
#estadoTxt{font-size:26px;font-weight:700}
#estadoDet{font-size:14px;margin-top:4px;min-height:18px}
button{width:100%;margin-top:12px;padding:14px;font-size:17px;font-weight:600;border:0;border-radius:10px;background:#fff;color:var(--rojo);cursor:pointer}
button:disabled{opacity:.45;cursor:default}
.semaforo{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.sig{border-radius:12px;padding:12px;color:#fff;background:var(--gris)}
.sig.verde{background:var(--verde)}.sig.amarillo{background:var(--amarillo)}.sig.rojo{background:var(--rojo)}
.sig .val{font-size:34px;font-weight:700}.sig .u{font-size:14px;opacity:.9}.sig .lbl{font-size:13px}
#semMsg{margin-top:10px;font-size:14px;color:var(--sub)}
.barra{height:12px;border-radius:99px;background:var(--borde);overflow:hidden}
.barra div{height:100%;background:linear-gradient(90deg,var(--verde),var(--amarillo),var(--rojo))}
.fila{display:flex;justify-content:space-between;font-size:14px;margin-top:8px}
canvas{width:100%;height:120px;display:block}
.dato{font-size:28px;font-weight:700}
ul{list-style:none;margin:0;padding:0;max-height:220px;overflow:auto}
li{padding:7px 0;border-bottom:1px solid var(--borde);font-size:14px;display:flex;gap:10px}
li span{color:var(--sub);font-variant-numeric:tabular-nums}
.leyenda{font-size:12px;color:var(--sub);margin-top:4px}
</style>
</head>
<body>
<header><h1>PulsoSeguro</h1><span id="conn">Conectando…</span></header>
<main>
  <section class="card ancho" id="estado">
    <h2>Estado del sistema</h2>
    <div id="estadoTxt">—</div>
    <div id="estadoDet"></div>
    <button id="btnSil" disabled>Silenciar alarma</button>
  </section>

  <section class="card ancho">
    <h2>Semáforo de signos vitales</h2>
    <div class="semaforo">
      <div class="sig" id="sigFC"><div class="lbl">Frecuencia cardiaca</div><div><span class="val" id="fc">--</span> <span class="u">lpm</span></div></div>
      <div class="sig" id="sigSp"><div class="lbl">Oxigenación (SpO₂)</div><div><span class="val" id="spo2">--</span> <span class="u">%</span></div></div>
    </div>
    <div id="semMsg"></div>
  </section>

  <section class="card">
    <h2>Historial de signos (último minuto)</h2>
    <canvas id="grafFC"></canvas>
    <div class="leyenda">FC (lpm)</div>
    <canvas id="grafSp"></canvas>
    <div class="leyenda">SpO₂ (%)</div>
  </section>

  <section class="card">
    <h2>Movimiento</h2>
    <div class="dato"><span id="mag">--</span> g</div>
    <canvas id="grafMag"></canvas>
    <div class="leyenda">Pico de aceleración por segundo (línea punteada = umbral de impacto)</div>
  </section>

  <section class="card">
    <h2>Sensibilidad de caída</h2>
    <div class="barra"><div id="barSens" style="width:0%"></div></div>
    <div class="fila"><span>Nivel: <b id="sens">--</b> %</span><span>Umbral de impacto: <b id="umbral">--</b> g</span></div>
    <div class="leyenda">Se ajusta con el potenciómetro de la pulsera.</div>
  </section>

  <section class="card">
    <h2>Registro de eventos</h2>
    <ul id="eventos"><li>Sin eventos</li></ul>
  </section>
</main>
<script>
const N=60, hFC=[], hSp=[], hMag=[];
let umbralActual=3, ocupado=false;
const textos={NORMAL:"Normal",CONFIRMACION:"¿Caída detectada?",ALARMA:"¡ALARMA!",ESPERA:"En espera"};
const etiquetas={verde:"Normal",amarillo:"Irregular",rojo:"Crítico",gris:"Sin lectura"};

function push(a,v){a.push(v);if(a.length>N)a.shift();}

function grafica(id,datos,min,max,color,umbral){
  const c=document.getElementById(id),r=c.getBoundingClientRect(),dpr=window.devicePixelRatio||1;
  c.width=r.width*dpr;c.height=r.height*dpr;
  const g=c.getContext('2d');g.scale(dpr,dpr);
  const w=r.width,h=r.height,css=getComputedStyle(document.documentElement);
  g.clearRect(0,0,w,h);
  g.strokeStyle=css.getPropertyValue('--borde');g.lineWidth=1;
  for(let i=0;i<=4;i++){const y=h*i/4;g.beginPath();g.moveTo(0,y);g.lineTo(w,y);g.stroke();}
  g.fillStyle=css.getPropertyValue('--sub');g.font='10px sans-serif';
  g.fillText(max,2,10);g.fillText(min,2,h-2);
  const y=v=>h-(Math.min(Math.max(v,min),max)-min)/(max-min)*h;
  if(umbral!==undefined){g.setLineDash([5,4]);g.strokeStyle=css.getPropertyValue('--rojo');
    g.beginPath();g.moveTo(0,y(umbral));g.lineTo(w,y(umbral));g.stroke();g.setLineDash([]);}
  g.strokeStyle=color;g.lineWidth=2;g.beginPath();let dentro=false;
  datos.forEach((v,i)=>{const x=w*i/(N-1);
    if(v===null){dentro=false;return;}
    if(!dentro){g.moveTo(x,y(v));dentro=true;}else g.lineTo(x,y(v));});
  g.stroke();
}

function horaEvento(ahoraDisp,tEvento){
  return new Date(Date.now()-(ahoraDisp-tEvento)).toLocaleTimeString('es-MX');
}

function mostrar(d){
  // Estado y alarma
  const est=document.getElementById('estado');
  est.className='card ancho '+d.estado;
  document.getElementById('estadoTxt').textContent=
    d.estado==='ALARMA'?(d.panico?'¡BOTÓN DE PÁNICO!':'¡CAÍDA CONFIRMADA!'):textos[d.estado];
  let det='';
  if(d.estado==='CONFIRMACION')det='Presiona el botón si estás bien · '+d.restante+' s';
  else if(d.estado==='ALARMA')det=d.sonido?('Sonando · se detiene en '+d.restante+' s'):'Sonido detenido (60 s) · alarma sin atender';
  else if(d.estado==='ESPERA')det='Detección de caídas reanudará en '+d.restante+' s';
  else det=d.sensores;
  document.getElementById('estadoDet').textContent=det;
  document.getElementById('btnSil').disabled=!(d.estado==='ALARMA'||d.estado==='CONFIRMACION');

  // Semáforo (regla 3)
  document.getElementById('fc').textContent=d.fc>0?d.fc:'--';
  document.getElementById('spo2').textContent=d.spo2>0?d.spo2:'--';
  document.getElementById('sigFC').className='sig '+d.semFC;
  document.getElementById('sigSp').className='sig '+d.semSp;
  document.getElementById('semMsg').textContent=!d.dedo?
    'Coloca el dedo sobre el sensor para medir.':
    (d.fc>0?'FC: '+etiquetas[d.semFC]+' · SpO₂: '+etiquetas[d.semSp]:'Midiendo… mantén el dedo quieto unos segundos.');

  // Historial
  push(hFC,d.fc>0?d.fc:null);push(hSp,d.spo2>0?d.spo2:null);push(hMag,d.magPico);
  const verde=getComputedStyle(document.documentElement).getPropertyValue('--verde');
  const azul=getComputedStyle(document.documentElement).getPropertyValue('--azul');
  grafica('grafFC',hFC,40,160,'#dc2626');
  grafica('grafSp',hSp,80,100,azul);
  umbralActual=d.umbral;
  grafica('grafMag',hMag,0,6,verde,d.umbral);

  // Movimiento y sensibilidad
  document.getElementById('mag').textContent=d.mag.toFixed(2);
  document.getElementById('sens').textContent=d.sens;
  document.getElementById('barSens').style.width=d.sens+'%';
  document.getElementById('umbral').textContent=d.umbral.toFixed(2);

  // Eventos (más reciente primero)
  const ul=document.getElementById('eventos');
  ul.innerHTML=d.eventos.length?'':'<li>Sin eventos</li>';
  d.eventos.forEach(e=>{const li=document.createElement('li');
    const s=document.createElement('span');s.textContent=horaEvento(d.ahora,e.t);
    li.appendChild(s);li.appendChild(document.createTextNode(e.m));ul.appendChild(li);});
}

async function actualizar(){
  if(ocupado)return;ocupado=true;
  const conn=document.getElementById('conn');
  try{
    const r=await fetch('/datos',{cache:'no-store'});
    mostrar(await r.json());
    conn.textContent='En línea';conn.className='ok';
  }catch(e){conn.textContent='Sin conexión';conn.className='';}
  ocupado=false;
}

document.getElementById('btnSil').addEventListener('click',async()=>{
  try{await fetch('/silenciar',{method:'POST'});}catch(e){}
  actualizar();
});

setInterval(actualizar,1000);   // frecuencia de actualización: 1 segundo
actualizar();
</script>
</body>
</html>
)rawliteral";

// ----------------------------------------------------------------------------
// LED RGB
// ----------------------------------------------------------------------------
void ledColor(int r, int g, int b) {
  r = constrain(r, 0, 255); g = constrain(g, 0, 255); b = constrain(b, 0, 255);
  if (LED_ANODO_COMUN) { r = 255 - r; g = 255 - g; b = 255 - b; }
  ledcWrite(PIN_LED_R, r);
  ledcWrite(PIN_LED_G, g);
  ledcWrite(PIN_LED_B, b);
}

void ledTransicion(unsigned long periodoMs) {             // rojo <-> azul
  float fase = (millis() % periodoMs) / (float)periodoMs;
  float k = 0.5 - 0.5 * cos(fase * 2 * PI);
  ledColor(255 * k, 0, 255 * (1 - k));
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

void silenciar(bool desdeDashboard) {                     // R4 y R7
  pitidoCorto();
  if (estado == CONFIRMACION) {
    registrarEvento(desdeDashboard ? "Caída cancelada desde el dashboard" : "Caída cancelada: la persona está bien");
    cambiarEstado(ESPERA);
  } else if (estado == ALARMA) {
    registrarEvento(desdeDashboard ? "Alarma silenciada desde el dashboard" : "Alarma silenciada con el botón");
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

// Segundos que faltan para que termine el estado actual (dashboard y pantalla)
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
      if (!largaDisparada && duracion < PULSACION_CORTA_MS) silenciar(false);
    }
  }
  if (estable == LOW && !largaDisparada && millis() - tPresion >= PULSACION_LARGA_MS) {
    largaDisparada = true;
    activarPanico();
  }
}

// ----------------------------------------------------------------------------
// Potenciómetro (R6)
// ----------------------------------------------------------------------------
float potFiltrado = 0;
int sensibilidadPct = 50, sensibilidadMostrada = -1;
float umbralImpactoG = 2.0;
unsigned long tMostrarNivel = 0;
bool mostrandoNivel = false;

void leerPotenciometro() {
  static unsigned long tLectura = 0;
  if (millis() - tLectura < 20) return;
  tLectura = millis();
  potFiltrado = potFiltrado * 0.8 + analogRead(PIN_POT) * 0.2;
  sensibilidadPct = round(potFiltrado * 100.0 / 4095.0);
  umbralImpactoG = IMPACTO_MAX_G - (IMPACTO_MAX_G - IMPACTO_MIN_G) * sensibilidadPct / 100.0;

  if (sensibilidadMostrada < 0) { sensibilidadMostrada = sensibilidadPct; return; }
  if (abs(sensibilidadPct - sensibilidadMostrada) >= 3) {
    sensibilidadMostrada = sensibilidadPct;
    mostrandoNivel = true;
    tMostrarNivel = millis();
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
float pico = 0, minPrevio = 1, umbralEvento = 2.0, desvMax = 0;
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

  pico = magnitudG; tPico = ahora; umbralEvento = umbralImpactoG;
  faseCaida = FC_PICO;
}

void evaluarCaida() {
  bool f1 = minPrevio < CAIDA_LIBRE_G && (tPico - tMinPrevio) < CAIDA_LIBRE_MAX_MS;
  bool f2 = pico > umbralEvento;
  bool f3 = desvMax < TOLERANCIA_INMOVIL_G;
  float dx = nDesp ? despX / nDesp : antesX, dy = nDesp ? despY / nDesp : antesY, dz = nDesp ? despZ / nDesp : antesZ;
  float na = sqrt(antesX * antesX + antesY * antesY + antesZ * antesZ), nd = sqrt(dx * dx + dy * dy + dz * dz);
  float c = (na > 0 && nd > 0) ? (antesX * dx + antesY * dy + antesZ * dz) / (na * nd) : 1;
  float angulo = acos(constrain(c, -1.0, 1.0)) * 180.0 / PI;
  bool f4 = angulo > CAMBIO_ORIENTACION_MIN;

  Serial.printf("[%lu ms] ANÁLISIS: caída libre %.2f g %s | pico %.2f g (umbral %.2f) %s | inmóvil %.2f g %s | orientación %.0f° %s\n",
                millis(), minPrevio, f1 ? "OK" : "NO", pico, umbralEvento, f2 ? "OK" : "NO",
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

  // Pico de cada segundo para la gráfica del dashboard
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
#define N_LOTES      5         // ~8 s de datos si se cae la conexión

const char* TOPIC_DATA   = "bio/" THING_NAME "/data";
const char* TOPIC_CMD    = "bio/" THING_NAME "/cmd";
const char* TOPIC_STATUS = "bio/" THING_NAME "/status";
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
    Serial.println("[AWS] conectado");
    return;
  }
  // -2 = no hubo conexión TCP/TLS (endpoint, certificados, red)
  // -4 / desconexión inmediata = AWS lo rechazó (política o client ID)
  Serial.printf("[AWS] fallo, estado=%d", mqtt.state());
  if (mqtt.state() == -2) {
    char err[100] = "";
    int codigo = awsNet.lastError(err, sizeof(err));
    Serial.printf(" | TLS %d (-0x%04X): %s | heap libre %u", codigo, (unsigned)-codigo, err,
                  (unsigned)ESP.getFreeHeap());
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

const char* semaforoSpO2() {
  if (!hayDedo || !spo2Valida) return "gris";
  if (spo2 >= SPO2_VERDE_MIN) return "verde";
  if (spo2 >= SPO2_AMARILLO_MIN) return "amarillo";
  return "rojo";
}

// ----------------------------------------------------------------------------
// LED según prioridad: alarma > confirmación > nivel del potenciómetro > espera > sin lectura > normal
// ----------------------------------------------------------------------------
void actualizarLED() {
  if (estado == ALARMA)       { ledTransicion(600); return; }
  if (estado == CONFIRMACION) { ledTransicion(2000); return; }
  if (mostrandoNivel) {
    if (millis() - tMostrarNivel < MOSTRAR_NIVEL_POT_MS) {
      int r = 255 * sensibilidadPct / 100;
      ledColor(r, 255 - r, 0);
      return;
    }
    mostrandoNivel = false;
  }
  if (estado == ESPERA) { ledColor(0, 0, 60); return; }
  if (!hayDedo) { ledColor(0, 0, (millis() / 500) % 2 ? 120 : 0); return; }
  ledColor(0, 80, 0);
}

// ============================================================================
// PANTALLA TFT ST7789 (versión 3)
// Distribución en horizontal (320 x 240):
//   y   0- 23  Encabezado: "PulsoSeguro", clientes Wi-Fi y pico de aceleración
//   y  28- 87  Estado: color según el estado, título grande y cuenta regresiva
//   y  94-177  Dos tarjetas: FC y SpO2 con el color del semáforo
//   y 184-213  Sensibilidad y umbral de impacto, con barra
//   y 218-239  Último evento del registro
// ============================================================================
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
// Mismos colores que el dashboard HTML (tema oscuro)
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

enum { R_ENCABEZADO, R_ESTADO, R_FC, R_SPO2, R_SENS, R_EVENTO, NUM_REGIONES };
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
  int clientes = WiFi.softAPgetStationNum();
  ponerTexto(e.l1, sizeof(e.l1), "Wi-Fi %d · AWS %s", clientes, estadoAWS());
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
  calcularSigno(deseado[R_FC], semaforoFC(), fc, hayDedo && fc > 0);
  calcularSigno(deseado[R_SPO2], semaforoSpO2(), spo2, hayDedo && spo2Valida);

  // Sensibilidad (R6)
  ContenidoRegion& p = deseado[R_SENS];
  p.fondo = C_FONDO; p.color = C_TEXTO; p.valor = sensibilidadPct;
  ponerTexto(p.l1, sizeof(p.l1), "Sens. %d%%  Umbral %.2f g", sensibilidadPct, umbralImpactoG);

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

void dibujarSensibilidad(const ContenidoRegion& n, const ContenidoRegion& v, bool completo) {
  if (completo || strcmp(n.l1, v.l1)) {
    tft.fillRect(4, 184, 312, 16, n.fondo);
    texto(8, 184, n.l1, 2, n.color, n.fondo);
  }
  if (completo || n.valor != v.valor) {
    int w = 304 * constrain(n.valor, 0, 100) / 100;
    int r = 255 * constrain(n.valor, 0, 100) / 100;        // verde (poco sensible) -> rojo (muy sensible)
    tft.fillRect(8, 204, w, 10, rgb565(r, 255 - r, 0));
    tft.fillRect(8 + w, 204, 304 - w, 10, C_TARJETA);
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
    case R_SENS:       dibujarSensibilidad(n, v, completo); break;
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

// ----------------------------------------------------------------------------
// Servidor web
// ----------------------------------------------------------------------------
void enviarDatos() {
  String j;
  j.reserve(2200);
  j += "{\"estado\":\""; j += nombreEstado();
  j += "\",\"panico\":"; j += alarmaPorPanico ? "true" : "false";
  j += ",\"sonido\":";   j += alarmaSonando() ? "true" : "false";
  j += ",\"restante\":"; j += segundosRestantes();
  j += ",\"dedo\":";     j += hayDedo ? "true" : "false";
  j += ",\"fc\":";       j += hayDedo ? fc : 0;
  j += ",\"spo2\":";     j += (hayDedo && spo2Valida) ? spo2 : 0;
  j += ",\"semFC\":\"";  j += semaforoFC();
  j += "\",\"semSp\":\""; j += semaforoSpO2();
  j += "\",\"ir\":";     j += ultimoIR;
  j += ",\"mag\":";      j += String(magnitudG, 2);
  j += ",\"magPico\":";  j += String(magPicoUltimo, 2);
  j += ",\"sens\":";     j += sensibilidadPct;
  j += ",\"umbral\":";   j += String(umbralImpactoG, 2);
  j += ",\"sensores\":\"MPU6050: "; j += mpuOK ? "OK" : "sin respuesta";
  j += " · MAX30102: ";  j += oximetroOK ? "OK" : "sin respuesta";
  j += "\",\"ahora\":";  j += millis();
  j += ",\"eventos\":[";
  for (int i = 0; i < numEventos; i++) {
    int k = (idxEvento - 1 - i + MAX_EVENTOS) % MAX_EVENTOS;
    if (i) j += ",";
    j += "{\"t\":"; j += eventos[k].t; j += ",\"m\":\""; j += eventos[k].msg; j += "\"}";
  }
  j += "]}";
  server.send(200, "application/json; charset=utf-8", j);
}

void configurarServidor() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGINA); });
  server.on("/datos", HTTP_GET, enviarDatos);
  server.on("/silenciar", HTTP_POST, []() {               // R7
    silenciar(true);
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.onNotFound([]() { server.send(404, "text/plain", "No encontrado"); });
  server.begin();
}

// Resumen por el Monitor Serie cada 2 s (para las pruebas)
unsigned long cicloMaxUs = 0;

void imprimirResumen() {
  static unsigned long t = 0;
  if (millis() - t < 2000) return;
  t = millis();
  Serial.printf("[%lu ms] %s | FC %s | SpO2 %s | dedo %s (IR %lu) | |a| %.2f g | sens %d %% (umbral %.2f g) | clientes Wi-Fi %d | TFT max %.1f ms | ciclo max %.1f ms\n",
                millis(), nombreEstado(),
                fc > 0 ? String(fc).c_str() : "--",
                (hayDedo && spo2Valida) ? String(spo2).c_str() : "--",
                hayDedo ? "SI" : "NO", (unsigned long)ultimoIR,
                magnitudG, sensibilidadPct, umbralImpactoG, WiFi.softAPgetStationNum(),
                dibujoMaxUs / 1000.0, cicloMaxUs / 1000.0);
  Serial.printf("            AWS: wifi %s | mqtt %s | captura %s (%lu muestras) | lotes enviados %lu | descartadas %lu | intentos %lu | heap %u\n",
                awsWifi ? "OK" : "--", awsConectado ? "OK" : "--", capturando ? "SI" : "NO",
                (unsigned long)seqCaptura, (unsigned long)awsLotesEnviados,
                (unsigned long)awsMuestrasDescartadas, (unsigned long)awsIntentos,
                (unsigned)ESP.getFreeHeap());
  dibujoMaxUs = 0;
  cicloMaxUs = 0;
}

// ----------------------------------------------------------------------------
// setup y loop
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n===== PulsoSeguro (versión 4: pantalla TFT + AWS IoT) =====");

  pinMode(PIN_BOTON, INPUT_PULLUP);
  analogReadResolution(12);

  ledcAttach(PIN_LED_R, 5000, 8);
  ledcAttach(PIN_LED_G, 5000, 8);
  ledcAttach(PIN_LED_B, 5000, 8);
  if (BUZZER_PASIVO) ledcAttach(PIN_BUZZER, 2000, 8);
  else pinMode(PIN_BUZZER, OUTPUT);
  buzzerTono(0);

  iniciarPantalla();

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  mpuOK = iniciarMPU();
  oximetroOK = iniciarOximetro();
  Serial.printf("MPU6050: %s | MAX30102: %s\n", mpuOK ? "OK" : "ERROR", oximetroOK ? "OK" : "ERROR");

  WiFi.mode(WIFI_AP_STA);                                // AP para el dashboard + STA para AWS
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("Red Wi-Fi: %s | contraseña: %s | abrir http://", AP_SSID, AP_PASSWORD);
  Serial.println(WiFi.softAPIP());
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);                      // no bloquea: la tarea AWS espera la conexión
  Serial.printf("Conectando a la red con internet \"%s\" para AWS IoT\n", WIFI_SSID);
  configurarServidor();
  iniciarAWS();

  // Autoprueba: rojo, verde, azul y un pitido
  ledColor(255, 0, 0); delay(250);
  ledColor(0, 255, 0); delay(250);
  ledColor(0, 0, 255); delay(250);
  ledColor(0, 0, 0);
  buzzerTono(2000); delay(120); buzzerTono(0);

  potFiltrado = analogRead(PIN_POT);
  registrarEvento("Sistema iniciado");
  cambiarEstado(NORMAL);
  mostrarInterfaz();
}

void loop() {
  static unsigned long tPrev = 0;
  unsigned long us = micros();
  if (tPrev && us - tPrev > cicloMaxUs) cicloMaxUs = us - tPrev;   // duración de la vuelta anterior
  tPrev = us;

  server.handleClient();       // dashboard
  leerOximetro();              // MAX30102 (R3)
  detectarCaida();             // MPU6050 (R1)
  leerBoton();                 // R4, R5
  leerPotenciometro();         // R6
  actualizarEstados();         // R2 y espera de 10 s
  actualizarLED();
  actualizarBuzzer();
  actualizarPantalla();        // TFT: una región por vuelta, como máximo
  procesarComandosAWS();       // start / stop recibidos por MQTT
  imprimirResumen();
}