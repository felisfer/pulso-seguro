// Enrutado de mensajes por tópico y estado acumulado del dashboard.

import type { LotePPG, MuestraPPG, Prediccion, Semaforo, Vitals } from "../tipos"
import type { EstadoConexion } from "./transporte"

export const VENTANA_HISTORIAL_MS = 10 * 60_000
const MUESTRAS_PPG = 600 // 6 s a 100 Hz
const MAX_EVENTOS = 50

/** Vitals con la marca de tiempo que usa la UI (ts del ESP32 o, si no es válido, la de llegada). */
export interface VitalsRecibidos extends Vitals {
  t: number
  recibido: number
}

export type Severidad = "critico" | "aviso" | "info" | "bien"

export interface Evento {
  id: number
  t: number
  severidad: Severidad
  texto: string
}

export interface EstadoTelemetria {
  conexion: EstadoConexion
  detalleConexion?: string
  identityId?: string
  online: boolean | null
  vitals: VitalsRecibidos | null
  /** Momento en que `caida` pasó a true (null si no hay caída activa). */
  inicioCaida: number | null
  /** Momento en que `panico` pasó a true (null si no hay alarma de pánico activa). */
  inicioPanico: number | null
  historialVitals: VitalsRecibidos[]
  pred: Prediccion | null
  historialPred: Prediccion[]
  ppg: MuestraPPG[]
  sesionPPG: Omit<LotePPG, "muestras"> | null
  muestrasPerdidas: number
  eventos: Evento[]
}

export const estadoInicial: EstadoTelemetria = {
  conexion: "conectando",
  online: null,
  vitals: null,
  inicioCaida: null,
  inicioPanico: null,
  historialVitals: [],
  pred: null,
  historialPred: [],
  ppg: [],
  sesionPPG: null,
  muestrasPerdidas: 0,
  eventos: [],
}

export type Accion =
  | { tipo: "mensaje"; topic: string; texto: string; recibido: number }
  | { tipo: "conexion"; estado: EstadoConexion; detalle?: string; identityId?: string }
  | { tipo: "limpiarPPG" }

// ---------- Parsers (tolerantes: un mensaje mal formado se ignora) ----------

const esNumeroONulo = (x: unknown) => x === null || (typeof x === "number" && Number.isFinite(x))
const SEMAFOROS: Semaforo[] = ["verde", "amarillo", "rojo"]

export function parseVitals(texto: string): Vitals | null {
  try {
    const v = JSON.parse(texto) as Partial<Vitals>
    if (typeof v.ts !== "number" || !esNumeroONulo(v.fc) || !esNumeroONulo(v.spo2)) return null
    return {
      ts: v.ts,
      fc: v.fc ?? null,
      spo2: v.spo2 ?? null,
      dedo: Boolean(v.dedo),
      caida: Boolean(v.caida),
      panico: Boolean(v.panico),
      semaforo: SEMAFOROS.includes(v.semaforo as Semaforo) ? (v.semaforo as Semaforo) : "amarillo",
    }
  } catch {
    return null
  }
}

/** `recibido` se usa como `ts` si la Lambda no lo manda (versiones viejas). */
export function parsePrediccion(texto: string, recibido: number): Prediccion | null {
  try {
    const p = JSON.parse(texto) as Partial<Prediccion> & { fc_prevista?: unknown }
    const fcPred = typeof p.fc_pred === "number" ? p.fc_pred : p.fc_prevista
    if (typeof fcPred !== "number" || !Number.isFinite(fcPred)) return null
    return {
      ts: typeof p.ts === "number" ? p.ts : recibido,
      fc_pred: fcPred,
      horizonte_s: typeof p.horizonte_s === "number" ? p.horizonte_s : 10,
      modelo: typeof p.modelo === "string" ? p.modelo : "?",
      ...(typeof p.fc === "number" && { fc: p.fc }),
      ...(SEMAFOROS.includes(p.semaforo as Semaforo) && { semaforo: p.semaforo }),
      ...(typeof p.usado === "string" && { usado: p.usado }),
    }
  } catch {
    return null
  }
}

export function parseLote(texto: string): LotePPG | null {
  const [encabezado, , ...filas] = texto.trim().split(/\r?\n/)
  if (!encabezado?.startsWith("#")) return null
  const [sesion = "", label = "", sujeto = ""] = encabezado.slice(1).split(",")
  const muestras: MuestraPPG[] = []
  for (const fila of filas) {
    const [seq, t_ms, red, ir] = fila.split(",").map(Number)
    if ([seq, t_ms, red, ir].every(Number.isFinite)) muestras.push({ seq, t_ms, red, ir })
  }
  return { sesion, label, sujeto, muestras }
}

// ---------- Reducer ----------

/** El ts del ESP32 solo es confiable si ya sincronizó NTP (después de 2020). */
const tiempoValido = (ts: number, recibido: number) => (ts > 1.6e12 ? ts : recibido)

let siguienteId = 1
function conEvento(estado: EstadoTelemetria, t: number, severidad: Severidad, texto: string): Evento[] {
  return [{ id: siguienteId++, t, severidad, texto }, ...estado.eventos].slice(0, MAX_EVENTOS)
}

export function reducir(estado: EstadoTelemetria, accion: Accion): EstadoTelemetria {
  switch (accion.tipo) {
    case "conexion":
      return {
        ...estado,
        conexion: accion.estado,
        detalleConexion: accion.detalle,
        identityId: accion.identityId ?? estado.identityId,
      }
    case "limpiarPPG":
      return { ...estado, ppg: [], sesionPPG: null, muestrasPerdidas: 0 }
    case "mensaje":
      return reducirMensaje(estado, accion.topic, accion.texto, accion.recibido)
  }
}

function reducirMensaje(estado: EstadoTelemetria, topic: string, texto: string, recibido: number): EstadoTelemetria {
  switch (topic.split("/")[2]) {
    case "status": {
      const online = texto.trim() === "online"
      if (online === estado.online) return estado
      const eventos =
        estado.online === null && online
          ? estado.eventos
          : conEvento(estado, recibido, online ? "bien" : "aviso", online ? "Pulsera en línea" : "Pulsera desconectada")
      return { ...estado, online, eventos }
    }

    case "vitals": {
      const v = parseVitals(texto)
      if (!v) return estado
      const nuevo: VitalsRecibidos = { ...v, t: tiempoValido(v.ts, recibido), recibido }
      const previo = estado.vitals
      let eventos = estado.eventos
      if (nuevo.caida && !previo?.caida) {
        eventos = conEvento({ ...estado, eventos }, nuevo.t, "critico", "Caída detectada")
      }
      if (nuevo.panico && !previo?.panico) {
        eventos = conEvento({ ...estado, eventos }, nuevo.t, "critico", "Botón de pánico activado")
      } else if (!nuevo.panico && previo?.panico) {
        eventos = conEvento({ ...estado, eventos }, nuevo.t, "info", "Alarma de pánico silenciada en la pulsera")
      }
      if (nuevo.semaforo === "rojo" && previo?.semaforo !== "rojo") {
        eventos = conEvento({ ...estado, eventos }, nuevo.t, "critico", "Signos vitales fuera de rango (rojo)")
      }
      if (!nuevo.dedo && previo?.dedo) {
        eventos = conEvento({ ...estado, eventos }, nuevo.t, "info", "Sensor sin contacto con la piel")
      }
      const limite = recibido - VENTANA_HISTORIAL_MS
      return {
        ...estado,
        // Recibir vitals implica que la pulsera está en línea.
        online: true,
        vitals: nuevo,
        inicioCaida: nuevo.caida ? (previo?.caida ? estado.inicioCaida : nuevo.t) : null,
        inicioPanico: nuevo.panico ? (previo?.panico ? estado.inicioPanico : nuevo.t) : null,
        historialVitals: [...estado.historialVitals.filter(x => x.t >= limite), nuevo],
        eventos,
      }
    }

    case "pred": {
      const p = parsePrediccion(texto, recibido)
      if (!p) return estado
      const limite = recibido - VENTANA_HISTORIAL_MS
      return {
        ...estado,
        pred: p,
        historialPred: [...estado.historialPred.filter(x => x.ts >= limite), p],
      }
    }

    case "data": {
      const lote = parseLote(texto)
      if (!lote || lote.muestras.length === 0) return estado
      const { muestras, ...sesionPPG } = lote
      const mismaSesion = estado.sesionPPG?.sesion === sesionPPG.sesion
      const previas = mismaSesion ? estado.ppg : []
      const ultimo = previas[previas.length - 1]
      const hueco = ultimo ? Math.max(0, muestras[0].seq - ultimo.seq - 1) : 0
      return {
        ...estado,
        ppg: [...previas, ...muestras].slice(-MUESTRAS_PPG),
        sesionPPG,
        muestrasPerdidas: (mismaSesion ? estado.muestrasPerdidas : 0) + hueco,
      }
    }

    default:
      return estado
  }
}
