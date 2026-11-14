import { useEffect, useState } from "react"
import { config } from "../config"
import type { Prediccion } from "../tipos"
import { fechaHora, horaCorta } from "../utilidades/tiempo"
import { GraficaLinea } from "./GraficaLinea"

const RANGOS = [
  { id: "1h", nombre: "1 h", ms: 3_600_000 },
  { id: "6h", nombre: "6 h", ms: 6 * 3_600_000 },
  { id: "24h", nombre: "24 h", ms: 24 * 3_600_000 },
] as const

type Resultado =
  | { tipo: "cargando" }
  | { tipo: "error"; mensaje: string }
  | { tipo: "ok"; datos: Prediccion[]; desde: number; hasta: number }

/**
 * Contrato esperado de la API (API Gateway → DynamoDB):
 *   GET {VITE_HISTORY_API_URL}/predicciones?thing=<thing>&desde=<ms>&hasta=<ms>
 *   Authorization: Bearer <id_token>
 *   → Prediccion[]  o  { items: Prediccion[] }
 */
async function cargarHistorial(thing: string, desde: number, hasta: number, idToken?: string, señal?: AbortSignal) {
  if (config.demo && !config.historialApiUrl) return historialDemo(desde, hasta)
  const url = `${config.historialApiUrl}/predicciones?thing=${encodeURIComponent(thing)}&desde=${desde}&hasta=${hasta}`
  const respuesta = await fetch(url, {
    headers: idToken ? { Authorization: `Bearer ${idToken}` } : {},
    signal: señal,
  })
  if (!respuesta.ok) throw new Error(`La API respondió ${respuesta.status}`)
  const json = (await respuesta.json()) as Prediccion[] | { items: Prediccion[] }
  const datos = Array.isArray(json) ? json : json.items
  return datos.filter(p => typeof p.ts === "number" && typeof p.fc_pred === "number").sort((a, b) => a.ts - b.ts)
}

function historialDemo(desde: number, hasta: number): Prediccion[] {
  const datos: Prediccion[] = []
  const paso = Math.max(2000, Math.floor((hasta - desde) / 600))
  for (let t = desde; t <= hasta; t += paso) {
    const actividad = Math.sin(t / 1_800_000) * 18 + Math.sin(t / 240_000) * 6
    datos.push({ ts: t, fc_pred: Math.round((76 + actividad + Math.random() * 3) * 10) / 10, horizonte_s: 10, modelo: "demo-v1" })
  }
  return datos
}

export function HistorialPredicciones({ thing, idToken }: { thing: string; idToken?: string }) {
  const [rango, setRango] = useState<(typeof RANGOS)[number]["id"]>("1h")
  const [recarga, setRecarga] = useState(0)
  const [resultado, setResultado] = useState<Resultado>({ tipo: "cargando" })
  const disponible = Boolean(config.historialApiUrl) || config.demo

  useEffect(() => {
    if (!disponible) return
    const control = new AbortController()
    const hasta = Date.now()
    const desde = hasta - RANGOS.find(r => r.id === rango)!.ms
    cargarHistorial(thing, desde, hasta, idToken, control.signal)
      .then(datos => setResultado({ tipo: "ok", datos, desde, hasta }))
      .catch(err => {
        if (!control.signal.aborted) setResultado({ tipo: "error", mensaje: err instanceof Error ? err.message : String(err) })
      })
    return () => control.abort()
    // El id_token se renueva solo; no hace falta recargar por eso.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [thing, rango, recarga, disponible])

  const cambiarRango = (id: typeof rango) => {
    setResultado({ tipo: "cargando" })
    setRango(id)
  }

  return (
    <section className="panel" aria-labelledby="titulo-historial">
      <div className="panel-encabezado">
        <h2 id="titulo-historial" className="panel-titulo">Historial de predicciones</h2>
        {disponible && (
          <div className="segmentado" role="group" aria-label="Rango de tiempo">
            {RANGOS.map(r => (
              <button key={r.id} type="button" aria-pressed={rango === r.id} onClick={() => cambiarRango(r.id)}>
                {r.nombre}
              </button>
            ))}
            <button
              type="button"
              onClick={() => {
                setResultado({ tipo: "cargando" })
                setRecarga(n => n + 1)
              }}
            >
              Actualizar
            </button>
          </div>
        )}
      </div>

      {!disponible ? (
        <p className="vacio">
          Configura <code>VITE_HISTORY_API_URL</code> (API Gateway → DynamoDB) para ver predicciones anteriores.
        </p>
      ) : resultado.tipo === "cargando" ? (
        <p className="vacio">Cargando…</p>
      ) : resultado.tipo === "error" ? (
        <p className="error-linea" role="alert">No se pudo cargar el historial: {resultado.mensaje}</p>
      ) : (
        <>
          <GraficaLinea
            titulo="FC predicha"
            unidad="lpm"
            series={[{ id: "pred", nombre: "FC predicha", color: "var(--serie-2)", puntos: resultado.datos.map(p => ({ t: p.ts, v: p.fc_pred })) }]}
            desde={resultado.desde}
            hasta={resultado.hasta}
            yMin={50}
            yMax={120}
            decimales={1}
            huecoMaxMs={60_000}
            formatoEjeX={rango === "1h" ? horaCorta : fechaHora}
            vacio="No hay predicciones en este rango."
          />
          <p className="tarjeta-pie">
            {resultado.datos.length.toLocaleString("es-MX")} predicciones
            {resultado.datos.length > 0 && ` · modelo ${resultado.datos[resultado.datos.length - 1].modelo}`}
          </p>
        </>
      )}
    </section>
  )
}
