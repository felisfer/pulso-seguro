import { useCallback, useEffect, useReducer, useRef, useState } from "react"
import { config } from "../config"
import type { Cmd } from "../tipos"
import { estadoInicial, reducir } from "./estado"
import { crearTransporteDemo } from "./simulador"
import { crearTransporteIot, type EventosTransporte, type Transporte } from "./transporte"

/**
 * Conecta con los tópicos `bio/<thing>/…` y acumula su estado.
 * Monta este hook con `key={thing}` para empezar de cero al cambiar de dispositivo.
 */
export function useTelemetria(thing: string, idToken: string | undefined) {
  const [estado, despachar] = useReducer(reducir, estadoInicial)
  const [verPPG, setVerPPG] = useState(false)
  const transporte = useRef<Transporte | null>(null)
  const idTokenRef = useRef(idToken)

  useEffect(() => {
    idTokenRef.current = idToken
  }, [idToken])

  useEffect(() => {
    const eventos: EventosTransporte = {
      onMensaje: (topic, texto) => despachar({ tipo: "mensaje", topic, texto, recibido: Date.now() }),
      onEstado: (estado, detalle, identityId) => despachar({ tipo: "conexion", estado, detalle, identityId }),
    }
    const t = config.demo
      ? crearTransporteDemo(thing, eventos)
      : crearTransporteIot(() => idTokenRef.current, eventos)
    t.suscribir([`bio/${thing}/status`, `bio/${thing}/vitals`, `bio/${thing}/pred`])
    transporte.current = t
    return () => {
      t.cerrar()
      transporte.current = null
    }
  }, [thing])

  // `data` cuesta ~2,400 entregas/h de ~10 KB: solo se suscribe a pedido.
  useEffect(() => {
    if (!verPPG) return
    const topic = `bio/${thing}/data`
    transporte.current?.suscribir([topic])
    return () => {
      transporte.current?.desuscribir([topic])
      despachar({ tipo: "limpiarPPG" })
    }
  }, [thing, verPPG])

  const publicar = useCallback(
    async (cmd: Cmd) => {
      if (!transporte.current) throw new Error("Sin conexión.")
      await transporte.current.publicar(`bio/${thing}/cmd`, JSON.stringify(cmd))
    },
    [thing],
  )

  const simularCaida = useCallback(() => transporte.current?.simularCaida?.(), [])

  return { ...estado, verPPG, setVerPPG, publicar, simularCaida }
}

export type Telemetria = ReturnType<typeof useTelemetria>
