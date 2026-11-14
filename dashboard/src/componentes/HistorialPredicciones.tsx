import { useEffect, useMemo, useState } from "react"
import { config } from "../config"
import type { Prediccion } from "../tipos"
import { fechaHora, horaCorta } from "../utilidades/tiempo"
import { GraficaLinea, type Serie } from "./GraficaLinea"

const RANGOS = [
  { id: "1h", nombre: "1 h", ms: 3_600_000 },
  { id: "6h", nombre: "6 h", ms: 6 * 3_600_000 },
  { id: "24h", nombre: "24 h", ms: 24 * 3_600_000 },
] as const

/** Un punto del historial: la predicción y, si ya pasaron los 10 s, la FC que de verdad se midió. */
type PuntoHistorial = Prediccion & { fc_real?: number }

interface Historial {
  datos: PuntoHistorial[]
  /** La API leyó su máximo de ítems: solo llegan los más recientes del rango. */
  truncado: boolean
}

type Resultado =
  | { tipo: "cargando" }
  | { tipo: "error"; mensaje: string }
  | ({ tipo: "ok"; desde: number; hasta: number } & Historial)

/**
 * Contrato de la API (lambda/api/api_historial.py, HTTP API con autorizador JWT de Cognito):
 *   GET {VITE_HISTORY_API_URL}/predicciones?thing=<thing>&desde=<ms>&hasta=<ms>
 *   Authorization: Bearer <id_token>
 *   → { items: [{ ts, fc_pred, horizonte_s, modelo, fc, fc_real?, usado }], total, truncado }
 */
async function cargarHistorial(
  thing: string,
  desde: number,
  hasta: number,
  idToken?: string,
  señal?: AbortSignal,
): Promise<Historial> {
  if (config.demo && !config.historialApiUrl) return { datos: historialDemo(desde, hasta), truncado: false }
  const url = `${config.historialApiUrl}/predicciones?thing=${encodeURIComponent(thing)}&desde=${desde}&hasta=${hasta}`
  const respuesta = await fetch(url, {
    headers: idToken ? { Authorization: `Bearer ${idToken}` } : {},
    signal: señal,
  })
  if (respuesta.status === 401) throw new Error("la sesión expiró; vuelve a iniciar sesión")
  if (!respuesta.ok) throw new Error(`La API respondió ${respuesta.status}`)
  const json = (await respuesta.json()) as PuntoHistorial[] | { items: PuntoHistorial[]; truncado?: boolean }
  const items = Array.isArray(json) ? json : json.items
  return {
    datos: items.filter(p => typeof p.ts === "number" && typeof p.fc_pred === "number").sort((a, b) => a.ts - b.ts),
    truncado: !Array.isArray(json) && Boolean(json.truncado),
  }
}

function historialDemo(desde: number, hasta: number): PuntoHistorial[] {
  const datos: PuntoHistorial[] = []
  const paso = Math.max(2000, Math.floor((hasta - desde) / 600))
  const fcEn = (t: number) => 76 + Math.sin(t / 1_800_000) * 18 + Math.sin(t / 240_000) * 6
  for (let t = desde; t <= hasta; t += paso) {
    const prevista = Math.round((fcEn(t + 10_000) + (Math.random() * 2 - 1) * 3) * 10) / 10
    const punto: PuntoHistorial = { ts: t, fc_pred: prevista, horizonte_s: 10, modelo: "demo-v1" }
    // Como en la tabla real: las predicciones de los últimos 10 s aún no tienen FC real.
    if (t + 10_000 <= hasta) punto.fc_real = Math.round((fcEn(t + 10_000) + (Math.random() * 2 - 1)) * 10) / 10
    datos.push(punto)
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
      .then(h => setResultado({ tipo: "ok", ...h, desde, hasta }))
      .catch(err => {
        if (!control.signal.aborted) setResultado({ tipo: "error", mensaje: err instanceof Error ? err.message : String(err) })
      })
    return () => control.abort()
    // El id_token se renueva solo; no hace falta recargar por eso.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [thing, rango, recarga, disponible])

  // Ambas series en el instante que se predijo (ts + horizonte), como en «Tendencias»:
  // donde se separan las líneas está el error del modelo.
  const series = useMemo<Serie[]>(() => {
    if (resultado.tipo !== "ok") return []
    const objetivo = (p: PuntoHistorial) => p.ts + p.horizonte_s * 1000
    return [
      {
        id: "real",
        nombre: "FC medida",
        color: "var(--serie-1)",
        puntos: resultado.datos.filter(p => p.fc_real !== undefined).map(p => ({ t: objetivo(p), v: p.fc_real! })),
      },
      {
        id: "pred",
        nombre: "FC predicha",
        color: "var(--serie-2)",
        discontinua: true,
        puntos: resultado.datos.map(p => ({ t: objetivo(p), v: p.fc_pred })),
      },
    ]
  }, [resultado])

  const cambiarRango = (id: typeof rango) => {
    setResultado({ tipo: "cargando" })
    setRango(id)
  }

  return (
    <section className="panel panel-lleno" aria-labelledby="titulo-historial">
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
            titulo="FC medida vs. predicha"
            unidad="lpm"
            series={series}
            desde={resultado.desde}
            hasta={resultado.hasta + 10_000}
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
            {resultado.truncado && " · rango recortado: se muestran solo las más recientes"}
          </p>
        </>
      )}
    </section>
  )
}
