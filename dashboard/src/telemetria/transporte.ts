// Transporte MQTT hacia AWS IoT Core por WebSocket.
// La reconexión se maneja aquí (no con la de mqtt.js) para volver a firmar la URL
// y renovar las credenciales de Cognito antes de que expiren.

import type { MqttClient } from "mqtt"
import { obtenerCredenciales, type CredencialesAws } from "../aws/cognito"
import { urlIotPrefirmada } from "../aws/sigv4"
import { config } from "../config"

export type EstadoConexion = "conectando" | "conectado" | "reconectando" | "error"

export interface EventosTransporte {
  onMensaje: (topic: string, texto: string) => void
  onEstado: (estado: EstadoConexion, detalle?: string, identityId?: string) => void
}

export interface Transporte {
  suscribir: (topics: string[]) => void
  desuscribir: (topics: string[]) => void
  publicar: (topic: string, payload: string) => Promise<void>
  cerrar: () => void
  /** Solo en modo demo. */
  simularCaida?: () => void
  /** Solo en modo demo. */
  simularSinContacto?: () => void
  /** Solo en modo demo. */
  simularPanico?: () => void
}

const MARGEN_RENOVACION_MS = 5 * 60_000
const ESPERA_MAX_MS = 30_000

export function crearTransporteIot(
  obtenerIdToken: () => string | undefined,
  eventos: EventosTransporte,
): Transporte {
  const topics = new Set<string>()
  const decodificador = new TextDecoder()
  let cliente: MqttClient | null = null
  let cred: CredencialesAws | null = null
  let cerrado = false
  let intento = 0
  let ultimoError: string | undefined
  let temporizador: ReturnType<typeof setTimeout> | undefined
  let renovacion: ReturnType<typeof setTimeout> | undefined

  const programarReconexion = () => {
    if (cerrado || temporizador) return
    const espera = Math.min(ESPERA_MAX_MS, 1000 * 2 ** intento) + Math.random() * 500
    intento++
    eventos.onEstado(intento > 3 ? "error" : "reconectando", ultimoError, cred?.identityId)
    temporizador = setTimeout(() => {
      temporizador = undefined
      void conectar()
    }, espera)
  }

  const conectar = async () => {
    if (cerrado) return
    eventos.onEstado(intento === 0 ? "conectando" : "reconectando", undefined, cred?.identityId)
    try {
      const idToken = obtenerIdToken()
      if (!idToken) throw new Error("No active session (missing id_token).")
      if (!cred || cred.expiracion - Date.now() < MARGEN_RENOVACION_MS) {
        cred = await obtenerCredenciales(idToken)
      }
      const url = await urlIotPrefirmada(config.iotEndpoint, config.region, cred)
      // Carga diferida: mqtt.js pesa ~350 KB y el modo demo no lo necesita.
      const { default: mqtt } = await import("mqtt")
      if (cerrado) return

      const nuevo = mqtt.connect(url, {
        // "<identityId>-<uuid>": la política IoT del dashboard solo permite
        // client/${cognito-identity.amazonaws.com:sub}-*, así nadie puede usar el client ID
        // de otro usuario (y desconectarlo). El uuid distingue las pestañas del mismo usuario.
        clientId: `${cred.identityId}-${crypto.randomUUID()}`,
        protocolVersion: 4,
        clean: true,
        reconnectPeriod: 0,
        connectTimeout: 10_000,
        keepalive: 30,
      })
      cliente = nuevo
      let seConecto = false

      nuevo.on("connect", () => {
        seConecto = true
        intento = 0
        ultimoError = undefined
        eventos.onEstado("conectado", undefined, cred?.identityId)
        if (topics.size > 0) nuevo.subscribe([...topics], { qos: 0 })
        // Reconecta con credenciales nuevas antes de que expiren las actuales.
        clearTimeout(renovacion)
        const msHastaRenovar = (cred?.expiracion ?? 0) - Date.now() - MARGEN_RENOVACION_MS
        renovacion = setTimeout(() => nuevo.end(true), Math.max(60_000, msHastaRenovar))
      })
      nuevo.on("message", (topic, payload) => {
        eventos.onMensaje(topic, decodificador.decode(payload))
      })
      nuevo.on("error", err => {
        ultimoError = err.message
      })
      nuevo.on("close", () => {
        clearTimeout(renovacion)
        if (cliente !== nuevo) return
        cliente = null
        if (!seConecto && !ultimoError) {
          ultimoError =
            "AWS IoT closed the connection. Check that the dashboard IoT policy is attached to this identity."
        }
        programarReconexion()
      })
    } catch (err) {
      ultimoError = err instanceof Error ? err.message : String(err)
      programarReconexion()
    }
  }

  void conectar()

  return {
    suscribir(nuevos) {
      nuevos.forEach(t => topics.add(t))
      if (cliente?.connected) cliente.subscribe(nuevos, { qos: 0 })
    },
    desuscribir(viejos) {
      viejos.forEach(t => topics.delete(t))
      if (cliente?.connected) cliente.unsubscribe(viejos)
    },
    publicar(topic, payload) {
      return new Promise((resolve, reject) => {
        if (!cliente?.connected) return reject(new Error("Not connected to AWS IoT."))
        cliente.publish(topic, payload, { qos: 1 }, err => (err ? reject(err) : resolve()))
      })
    },
    cerrar() {
      cerrado = true
      clearTimeout(temporizador)
      clearTimeout(renovacion)
      cliente?.end(true)
      cliente = null
    },
  }
}
