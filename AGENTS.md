# AGENTS.md: PulsoSeguro

Guía para agentes de código (Claude Code, Codex, Copilot, etc.) que trabajen en este repositorio: qué es el proyecto, en qué estado está, cómo se ejecuta y qué reglas seguir. La documentación para personas está en [`README.md`](README.md).

_Última actualización: 2026-11-14 (rama `feature/dashboard`)._

## 1. Qué es

Pulsera IoT con ESP32 que mide **frecuencia cardiaca (FC)** y **SpO₂** (MAX30102) y **detecta caídas** (MPU6050). Publica por MQTT/TLS en **AWS IoT Core**; una Rule guarda las señales crudas en **S3** para entrenar un modelo de **predicción de FC a +10 s** en Python. Un **dashboard web** en React muestra la pulsera en vivo.

```text
pulso-seguro/
├── firmware/       # Arduino/ESP32: pulso_seguro_aws (principal, v5), esp32_aws (solo captura), esp32_aws_sim (simulado)
├── herramientas/   # control.py (start/stop por MQTT), cargar_s3.py (S3 → dataset.csv)
├── modelo/         # modelo_fc.py (FC de referencia desde el PPG + HistGradientBoosting)
├── dashboard/      # SPA React 19 + TypeScript + Vite (este es el foco del trabajo reciente)
└── README.md       # documentación completa
```

## 2. Estado actual

| Componente | Estado |
|---|---|
| Firmware v7 (`firmware/pulso_seguro_aws`): features → Lambda, `pred`, `vitals` | 🟡 Compila (core esp32 3.3.12, *Huge APP*); falta validarlo en la placa |
| Captura a AWS IoT → S3 (`bio/<thing>/data`, `status`, `cmd`) | ✅ Funciona con datos reales |
| `cargar_s3.py`, `control.py` | ✅ Funcionan |
| Modelo de FC | 🟡 Pipeline listo; **no supera a la persistencia** por falta de datos con transiciones |
| **Dashboard web** | ✅ Completo en **modo demo**; login con Cognito funciona (incluido el logout); ⏳ **sin conectar a AWS IoT** |
| Tópico `bio/<thing>/vitals` en el firmware | 🟡 Implementado (al cambiar, máx. 1 Hz; mín. cada 5 s; ts por NTP) |
| Lambda de predicción → `bio/<thing>/pred` + DynamoDB (`lambda/`, `infra/`, ver `README_prediccion.md`) | 🟡 Probada con moto; falta desplegarla |
| API del historial (`GET /predicciones`, `lambda/api/`, HTTP API + JWT de Cognito) | 🟡 Probada con moto; se despliega con `-UserPoolId/-ClientId` |
| Modelo reentrenado con el extractor nuevo (`modelo/exportar_modelo.py`) | 🟡 `modelo_fc.json` exportado con datos reales; paridad C/Python OK; `usar: media3` (Ridge 8.47 vs. media3 6.99 lpm) |

### Dashboard: lo que ya está hecho

- Login OIDC con Cognito (`react-oidc-context`) y logout por `/logout` del *managed login*.
- Conexión a IoT Core: Identity Pool → credenciales temporales → URL WSS firmada con SigV4 → `mqtt.js` con reconexión propia. **Sin AWS SDK.** No se ha probado contra AWS real.
- Modo demo (`VITE_DEMO=true`) que emite los mismos payloads de texto que el ESP32 y la Lambda.
- Tarjetas: Estado general (semáforo verde/naranja/rojo, tarjeta teñida), FC (corazón que late a 60/FC s), SpO₂ (burbujas que suben), Sensor (verde/rojo según contacto).
- Paneles: tendencias (FC medida vs. predicha, SpO₂), eventos, onda PPG (opcional), sesión de captura (opcional), historial de predicciones (opcional, vía API).
- Cuadrícula editable con `react-grid-layout` v2: mover y redimensionar, contenido que se adapta (container queries), guardado por usuario en `localStorage`, «Restablecer diseño» y móvil en una columna fija.
- Tema claro/oscuro, `prefers-reduced-motion`, colores de estado siempre con ícono y texto, gráficas con tooltip y vista de tabla.

### Pendiente (en orden)

1. **AWS** (el usuario lo hace en la consola; los pasos están en el README, sección «Configuración de AWS para el dashboard»): Identity Pool, rol IAM autenticado, política IoT `dashboard-policy` adjunta a cada identidad, y `VITE_IDENTITY_POOL_ID` / `VITE_IOT_ENDPOINT` / `VITE_THING_NAMES`.
2. **Firmware**: probar v7 en la placa (features, `/pred`, `vitals`, heap libre). La política IoT del dispositivo debe permitir publicar en `bio/<thing>/vitals` y `bio/<thing>/features`, y suscribirse a `bio/<thing>/pred`.
3. **Lambda + API**: desplegar con `infra/desplegar.ps1` (con `-UserPoolId`/`-ClientId` crea la API del historial) y poner `UrlApiHistorial` en `VITE_HISTORY_API_URL`. El modelo real aún no supera a `media3` (329 ventanas): grabar más sesiones de `transicion` y reentrenar.
4. Probar el dashboard con la pulsera real y desplegarlo (URLs de producción en Cognito).

## 3. Reglas de trabajo con el usuario

- **Nunca hagas `git commit` ni `push`.** Al terminar, avisa qué cambiaste; el usuario revisa y decide el commit.
- **Trabaja por mini actividades.** Para tareas grandes, propón un plan con mini entregables, ejecuta **una** y detente con un resumen (qué cambió, cómo probarlo). No empieces la siguiente hasta que el usuario lo diga. Si algo se rompe, resuélvelo antes de avanzar.
- **Los cambios en AWS los hace el usuario.** Acumula lo que haga falta y dale indicaciones paso a paso al final; no lo intercales.
- **Antes de cambiar cómo se configura el build o el despliegue**, pregunta (ver §5).
- Responde en **español**.

## 4. Convenciones de código

- **Idioma**: identificadores, comentarios, textos de UI y documentación en español (`useTelemetria`, `TarjetaVital`, `disenoInicial`…). Mantén ese estilo.
- **Región AWS: `us-east-2` para todo.** Cualquier `us-east-1` en documentos viejos es un error.
- **TypeScript estricto** con `verbatimModuleSyntax` (usa `import type`) y `erasableSyntaxOnly` (sin `enum` ni *parameter properties*).
- **ESLint** con `eslint-plugin-react-hooks` v7: no leas ni escribas `ref.current` durante el render, no llames `Date.now()` en el render (usa `useAhora`) y evita `setState` síncrono dentro de efectos.
- **El dashboard no recalcula umbrales médicos**: el color del semáforo sale de `vitals.semaforo` (lo decide el ESP32).
- **CSS**: tokens en `dashboard/src/index.css` (claro, oscuro con `prefers-color-scheme` y `[data-theme]`). Los colores de estado (`--bien`, `--naranja`, `--critico`) siempre van con ícono y texto. Toda animación debe tener alternativa con `prefers-reduced-motion`.
- **Gráficas**: SVG propio en `GraficaLinea.tsx` (sin librería), con un solo eje Y, leyenda si hay ≥ 2 series, tooltip y vista de tabla. Serie 1 = azul (FC medida), serie 2 = naranja discontinua (FC predicha).
- **Dependencias**: mínimas. Las de runtime son `react`, `react-oidc-context`/`oidc-client-ts`, `mqtt` (carga diferida) y `react-grid-layout@2.3.0` (API v2: `Responsive`, `useContainerWidth`, `dragConfig`/`resizeConfig`).

## 5. Configuración y build del dashboard (importante)

- Vite **incrusta las `VITE_*` al compilar**.
  - `pnpm dev` (modo development) lee **`dashboard/.env`**.
  - El build de Docker lee **`dashboard/.env.production`**, que está en `.gitignore` pero **no** en `.dockerignore` a propósito.
- **No declares `ARG`/`ENV VITE_*` en el Dockerfile ni `args` en `compose.yaml`.** Vite da prioridad al entorno del proceso aunque los valores estén vacíos, y pisarían `.env.production`. Ya pasó una vez.
- Si con `pnpm dev` no aparece el login o el botón «Salir», lo primero es revisar que exista `dashboard/.env`.
- `VITE_LOGOUT_URI` debe coincidir **exactamente** (incluida la `/` final) con una *Allowed sign-out URL* del App client de Cognito.
- **Nunca** leas, imprimas ni subas valores reales de `.env`, `.env.production`, `secrets.h` ni certificados.

## 6. Comandos

```bash
# Dashboard
cd dashboard
pnpm install
pnpm dev                       # http://localhost:5173
pnpm exec tsc -b && pnpm lint  # verificación rápida (debe quedar sin errores)
pnpm build

# Modo demo sin AWS ni login (PowerShell)
$env:VITE_DEMO="true"; $env:VITE_AUTHORITY=""; pnpm dev

# Docker
docker compose up --build dashboard       # nginx en :8080 (usa .env.production)
docker compose up --build dashboard-dev   # Vite en :5173 (usa .env)

# Python
pip install -r requirements.txt && pip install paho-mqtt boto3
python herramientas/control.py --label reposo --sujeto s01 --duracion 60 --monitor
python herramientas/cargar_s3.py
python modelo/modelo_fc.py --demo
```

No hay pruebas automatizadas. Para verificar la UI se usa el modo demo:
- En la sesión de captura, la actividad `correr` sube la FC a ~145 lpm y el semáforo pasa por verde → naranja → rojo.
- «Simular sin contacto» y «Simular caída» prueban esos estados.
- Sin extensión de navegador se puede automatizar con Chrome headless por CDP (`--remote-debugging-port`). Ten en cuenta que en este equipo Chrome reporta `prefers-reduced-motion: reduce`.

## 7. Mapa del dashboard (`dashboard/src/`)

| Archivo | Responsabilidad |
|---|---|
| `main.tsx`, `App.tsx` | Arranque, `AuthProvider` (solo si hay `VITE_AUTHORITY`), pantallas de login/error y logout |
| `config.ts`, `vite-env.d.ts` | Lectura y tipos de las `VITE_*`; `faltantesIot()` |
| `tipos.ts` | Contratos de `status`, `vitals`, `pred`, `cmd`, `data` |
| `aws/cognito.ts` | `GetId` + `GetCredentialsForIdentity` por REST |
| `aws/sigv4.ts` | URL WSS prefirmada para IoT (token de sesión fuera de la firma) |
| `telemetria/transporte.ts` | Cliente MQTT, reconexión con espera exponencial y renovación de credenciales |
| `telemetria/simulador.ts` | Transporte del modo demo (mismos payloads) |
| `telemetria/estado.ts` | Parsers tolerantes, reducer, eventos e historial de 10 min |
| `telemetria/useTelemetria.ts` | Hook que une transporte y estado; suscribe a `data` solo a pedido |
| `componentes/Tablero.tsx` | Encabezado, selector de pulsera, usuario y salir |
| `componentes/Monitor.tsx` | Cuadrícula editable, contenido de cada panel, barra de estado y modo demo |
| `componentes/TarjetasVitales.tsx` | `TarjetaVital` (estado, fc, spo2, sensor) |
| `componentes/CorazonLatiendo.tsx`, `BurbujasOxigeno.tsx` | Ilustraciones animadas |
| `componentes/GraficaLinea.tsx`, `OndaPPG.tsx` | Gráfica SVG y onda en canvas; ambas llenan su panel si no reciben `alto` |
| `componentes/PanelCaptura.tsx`, `HistorialPredicciones.tsx`, `ListaEventos.tsx`, `AlertaCaida.tsx` | Paneles |
| `diseno/diseno.ts` | Diseño inicial por anchura (lg 12 col, md 6, sm 1), carga y guardado por usuario (`VERSION` para invalidar) |
| `utilidades/tiempo.ts`, `utilidades/useTamano.ts` | Formato de hora, `useAhora` y medición de tamaño con ResizeObserver |
| `index.css`, `App.css` | Tokens de diseño y estilos |

## 8. Problemas conocidos y detalles

- Al soltar un panel sobre otro, `react-grid-layout` empuja el otro hacia abajo (compactación vertical) en lugar de intercambiarlos.
- Si al redimensionar se lleva el mouse al borde inferior, el navegador desplaza la página y el panel sigue creciendo.
- En el simulador, sin contacto el semáforo se pone en «amarillo»; con la pulsera real lo decide el ESP32.
- Los `secrets.h` locales del firmware todavía tienen el endpoint de ejemplo; se configuran a mano con los valores reales (no versionados).
- El bundle principal pesa ~410 KB (~123 KB gzip); `mqtt.js` (~330 KB) va en un chunk aparte que se carga solo al conectar a AWS.
