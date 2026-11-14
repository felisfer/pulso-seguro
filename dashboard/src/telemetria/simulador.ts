// Modo demo: emite los mismos payloads (texto) que publicarían el ESP32 y la Lambda,
// así la UI y los parsers se prueban sin AWS. Imita a firmware/esp32_aws_sim:
// la FC objetivo depende de la etiqueta del `start` (correr ~145, caminar ~100, otra ~70).

import type { Cmd, Prediccion, Semaforo, Vitals } from "../tipos"
import type { EventosTransporte, Transporte } from "./transporte"

const ruido = (amplitud: number) => (Math.random() * 2 - 1) * amplitud

function semaforoDe(fc: number | null, spo2: number | null): Semaforo {
  if (fc === null || spo2 === null) return "amarillo"
  const fcVerde = fc >= 60 && fc <= 100
  const fcAmarilla = fc >= 50 && fc <= 120
  if (fcVerde && spo2 >= 95) return "verde"
  if (fcAmarilla && spo2 >= 90) return "amarillo"
  return "rojo"
}

export function crearTransporteDemo(thing: string, eventos: EventosTransporte): Transporte {
  const base = `bio/${thing}`
  const topics = new Set<string>()
  const temporizadores: ReturnType<typeof setInterval>[] = []

  let fc = 72
  let fcObjetivo = 70
  let spo2 = 97.5
  let caidaHasta = 0
  let sinDedoHasta = 0
  let sesion = { sesion: "", label: "", sujeto: "" }
  let seq = 0
  let fase = 0
  let tMs = 0
  let ventana = 0
  const historialFc: number[] = []

  const emitir = (sub: string, texto: string) => {
    const topic = `${base}/${sub}`
    if (topics.has(topic)) eventos.onMensaje(topic, texto)
  }

  // Cambios espontáneos de actividad cuando no hay sesión de captura.
  temporizadores.push(
    setInterval(() => {
      if (!sesion.label) fcObjetivo = [68, 72, 95, 110, 75][Math.floor(Math.random() * 5)]
    }, 45_000),
  )

  // vitals a 1 Hz
  temporizadores.push(
    setInterval(() => {
      const ahora = Date.now()
      fc += (fcObjetivo - fc) * 0.08 + ruido(1.2)
      spo2 = Math.min(100, Math.max(88, spo2 + (97.5 - spo2) * 0.1 + ruido(0.4)))
      if (Math.random() < 0.004) sinDedoHasta = ahora + 4000
      const dedo = ahora >= sinDedoHasta
      const fcLeida = dedo ? Math.round(fc) : null
      const spo2Leida = dedo ? Math.round(spo2) : null
      historialFc.push(fc)
      if (historialFc.length > 10) historialFc.shift()
      const vitals: Vitals = {
        ts: ahora,
        fc: fcLeida,
        spo2: spo2Leida,
        dedo,
        caida: ahora < caidaHasta,
        semaforo: semaforoDe(fcLeida, spo2Leida),
      }
      emitir("vitals", JSON.stringify(vitals))
    }, 1000),
  )

  // pred cada 2 s: extrapola la tendencia de los últimos 10 s.
  temporizadores.push(
    setInterval(() => {
      if (historialFc.length < 3) return
      const pendiente = (historialFc[historialFc.length - 1] - historialFc[0]) / historialFc.length
      const prevista = Math.round((fc + pendiente * 10 + ruido(1.5)) * 10) / 10
      // Mismo payload que lambda/src/lambda_function.py (incluye los campos que lee el ESP32).
      const pred: Prediccion & { win: number; fc_prevista: number } = {
        ts: Date.now(),
        win: ventana++,
        fc: Math.round(fc * 10) / 10,
        fc_prevista: prevista,
        fc_pred: prevista,
        horizonte_s: 10,
        semaforo: prevista > 120 ? "amarillo" : "verde",
        modelo: "demo-v1",
        usado: "media3",
      }
      emitir("pred", JSON.stringify(pred))
    }, 2000),
  )

  // data: lotes CSV de 1.5 s a 100 Hz, solo con sesión activa (o suscripción manual).
  temporizadores.push(
    setInterval(() => {
      if (!topics.has(`${base}/data`)) return
      const filas: string[] = []
      for (let i = 0; i < 150; i++) {
        fase += (fc / 60) / 100
        const ciclo = fase % 1
        // Pulso sistólico + muesca dicrota.
        const pulso = Math.exp(-((ciclo - 0.18) ** 2) / 0.006) + 0.35 * Math.exp(-((ciclo - 0.48) ** 2) / 0.004)
        const deriva = 600 * Math.sin(tMs / 4000)
        const ir = Math.round(118_000 - 900 * pulso + deriva + ruido(60))
        const red = Math.round(88_000 - 600 * pulso + deriva * 0.7 + ruido(50))
        filas.push(`${seq},${tMs},${red},${ir},0.41,-0.10,9.77,0.001,-0.004,0.0007`)
        seq++
        tMs += 10
      }
      // Simula una pérdida de muestras ocasional.
      if (Math.random() < 0.05) seq += 20
      const etiqueta = sesion.label ? sesion : { sesion: "demo", label: "monitor", sujeto: "s00" }
      emitir(
        "data",
        `#${etiqueta.sesion},${etiqueta.label},${etiqueta.sujeto}\nseq,t_ms,red,ir,ax,ay,az,gx,gy,gz\n${filas.join("\n")}`,
      )
    }, 1500),
  )

  setTimeout(() => eventos.onEstado("conectado"), 400)

  return {
    suscribir(nuevos) {
      nuevos.forEach(t => topics.add(t))
      // `status` es retenido: se entrega al suscribirse.
      if (nuevos.includes(`${base}/status`)) setTimeout(() => emitir("status", "online"), 500)
    },
    desuscribir(viejos) {
      viejos.forEach(t => topics.delete(t))
    },
    async publicar(topic, payload) {
      if (topic !== `${base}/cmd`) return
      const cmd = JSON.parse(payload) as Cmd
      if (cmd.cmd === "start") {
        sesion = { sesion: cmd.sesion, label: cmd.label, sujeto: cmd.sujeto }
        seq = 0
        const label = cmd.label.toLowerCase()
        fcObjetivo = label.includes("correr") ? 145 : label.includes("caminar") ? 100 : 70
      } else {
        sesion = { sesion: "", label: "", sujeto: "" }
        fcObjetivo = 70
      }
    },
    cerrar() {
      temporizadores.forEach(clearInterval)
    },
    simularCaida() {
      caidaHasta = Date.now() + 15_000
    },
    simularSinContacto() {
      sinDedoHasta = Date.now() + 10_000
    },
  }
}
