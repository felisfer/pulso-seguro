import { useId, useMemo, useState, type PointerEvent } from "react"
import { useTamano } from "../utilidades/useTamano"
import { horaConSegundos, horaCorta } from "../utilidades/tiempo"

export interface Punto {
  t: number
  v: number | null
}

export interface Serie {
  id: string
  nombre: string
  /** Variable CSS del color (p. ej. "var(--serie-1)"). */
  color: string
  puntos: Punto[]
  discontinua?: boolean
}

interface Props {
  titulo: string
  unidad: string
  series: Serie[]
  desde: number
  hasta: number
  /** Rango mínimo visible del eje Y; se amplía si los datos lo exceden. */
  yMin?: number
  yMax?: number
  /** Alto fijo en px. Si se omite, la gráfica llena el espacio libre de su panel. */
  alto?: number
  /** En modo "llenar", proporción del espacio libre que toma frente a otras gráficas (flex-grow). */
  peso?: number
  /** Distancia máxima entre puntos para unirlos con línea. */
  huecoMaxMs?: number
  /** Marca vertical de "ahora" (útil cuando hay predicciones hacia el futuro). */
  ahora?: number
  decimales?: number
  vacio?: string
  formatoEjeX?: (t: number) => string
}

const M = { arriba: 12, derecha: 16, abajo: 26, izquierda: 40 }
const ALTO_MIN = 100

function pasoBonito(rango: number, divisiones: number) {
  const bruto = rango / divisiones
  const potencia = 10 ** Math.floor(Math.log10(bruto))
  const n = bruto / potencia
  return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * potencia
}

export function GraficaLinea({
  titulo,
  unidad,
  series,
  desde,
  hasta,
  yMin,
  yMax,
  alto,
  peso = 1,
  huecoMaxMs = 5000,
  ahora,
  decimales = 0,
  vacio = "Waiting for data…",
  formatoEjeX,
}: Props) {
  const [contenedor, { ancho, alto: altoMedido }] = useTamano<HTMLDivElement>()
  const llenar = alto === undefined
  // En modo "llenar" el alto lo da el panel (con un mínimo para que siga siendo legible).
  const altoReal = alto ?? Math.max(ALTO_MIN, altoMedido)
  const [cursorT, setCursorT] = useState<number | null>(null)
  const [verTabla, setVerTabla] = useState(false)
  const idTitulo = useId()

  const conDatos = series.some(s => s.puntos.some(p => p.v !== null && p.t >= desde && p.t <= hasta))

  const escala = useMemo(() => {
    const valores = series.flatMap(s => s.puntos.filter(p => p.t >= desde && p.t <= hasta && p.v !== null).map(p => p.v as number))
    let lo = Math.min(...valores, yMin ?? Infinity)
    let hi = Math.max(...valores, yMax ?? -Infinity)
    if (!Number.isFinite(lo) || !Number.isFinite(hi)) {
      lo = yMin ?? 0
      hi = yMax ?? 1
    }
    if (hi - lo < 1e-9) {
      lo -= 1
      hi += 1
    }
    const paso = pasoBonito(hi - lo, 4)
    lo = Math.floor(lo / paso) * paso
    hi = Math.ceil(hi / paso) * paso
    const ticks: number[] = []
    for (let v = lo; v <= hi + paso / 2; v += paso) ticks.push(Number(v.toFixed(6)))
    return { lo, hi, ticks }
  }, [series, desde, hasta, yMin, yMax])

  const anchoPlot = Math.max(10, ancho - M.izquierda - M.derecha)
  const altoPlot = altoReal - M.arriba - M.abajo
  const x = (t: number) => M.izquierda + ((t - desde) / (hasta - desde)) * anchoPlot
  const y = (v: number) => M.arriba + (1 - (v - escala.lo) / (escala.hi - escala.lo)) * altoPlot

  const caminos = series.map(s => {
    let d = ""
    let previo: Punto | null = null
    for (const p of s.puntos) {
      if (p.t < desde || p.t > hasta || p.v === null) {
        previo = null
        continue
      }
      const unir = previo && p.t - previo.t <= huecoMaxMs
      d += `${unir ? "L" : "M"}${x(p.t).toFixed(1)},${y(p.v).toFixed(1)}`
      previo = p
    }
    return d
  })

  const ticksX = useMemo(() => {
    const n = Math.max(2, Math.min(6, Math.floor(anchoPlot / 110)))
    return Array.from({ length: n + 1 }, (_, i) => desde + ((hasta - desde) * i) / n)
  }, [desde, hasta, anchoPlot])
  const fmtX = formatoEjeX ?? (hasta - desde <= 10 * 60_000 ? horaConSegundos : horaCorta)

  // Punto más cercano al cursor por serie.
  const cercano = (s: Serie, t: number) => {
    let mejor: Punto | null = null
    for (const p of s.puntos) {
      if (p.v === null || p.t < desde || p.t > hasta) continue
      if (!mejor || Math.abs(p.t - t) < Math.abs(mejor.t - t)) mejor = p
    }
    return mejor && Math.abs(mejor.t - t) <= huecoMaxMs ? mejor : null
  }
  const enCursor = cursorT === null ? [] : series.map(s => ({ s, p: cercano(s, cursorT) }))
  const hayEnCursor = enCursor.some(e => e.p)

  const alMover = (e: PointerEvent<SVGRectElement>) => {
    const caja = e.currentTarget.getBoundingClientRect()
    const fraccion = (e.clientX - caja.left) / caja.width
    setCursorT(desde + Math.min(1, Math.max(0, fraccion)) * (hasta - desde))
  }

  const fmtV = (v: number) => v.toFixed(decimales)

  const filasTabla = useMemo(() => {
    if (!verTabla || series.length === 0) return []
    const tiempos = [...new Set(series[0].puntos.filter(p => p.t >= desde && p.t <= hasta).map(p => p.t))].slice(-30).reverse()
    return tiempos.map(t => ({ t, valores: series.map(s => cercano(s, t)?.v ?? null) }))
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [verTabla, series, desde, hasta])

  return (
    <figure
      className={`grafica ${llenar ? "grafica-llena" : ""}`}
      style={llenar ? { flexGrow: peso } : undefined}
      aria-labelledby={idTitulo}
    >
      <figcaption className="grafica-encabezado">
        <h3 id={idTitulo}>
          {titulo} <span className="unidad">({unidad})</span>
        </h3>
        {series.length > 1 && (
          <ul className="leyenda">
            {series.map(s => (
              <li key={s.id}>
                <svg width="18" height="8" aria-hidden="true">
                  <line x1="1" y1="4" x2="17" y2="4" stroke={s.color} strokeWidth="2" strokeLinecap="round" strokeDasharray={s.discontinua ? "4 3" : undefined} />
                </svg>
                {s.nombre}
              </li>
            ))}
          </ul>
        )}
        <button type="button" className="boton-texto" onClick={() => setVerTabla(v => !v)} aria-pressed={verTabla}>
          {verTabla ? "Show chart" : "Show table"}
        </button>
      </figcaption>

      {verTabla ? (
        <div className="tabla-scroll" style={llenar ? undefined : { maxHeight: alto }}>
          <table className="tabla">
            <thead>
              <tr>
                <th scope="col">Time</th>
                {series.map(s => (
                  <th scope="col" key={s.id}>{s.nombre}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {filasTabla.map(f => (
                <tr key={f.t}>
                  <td>{horaConSegundos(f.t)}</td>
                  {f.valores.map((v, i) => (
                    <td key={series[i].id} className="num">{v === null ? "—" : fmtV(v)}</td>
                  ))}
                </tr>
              ))}
            </tbody>
          </table>
          {filasTabla.length === 0 && <p className="vacio">{vacio}</p>}
        </div>
      ) : (
        <div ref={contenedor} className="grafica-lienzo" style={llenar ? undefined : { height: alto }}>
          {ancho > 0 && (
            <svg width={ancho} height={altoReal} role="img" aria-label={`${titulo} over time`}>
              {escala.ticks.map(v => (
                <g key={v}>
                  <line className="rejilla" x1={M.izquierda} x2={ancho - M.derecha} y1={y(v)} y2={y(v)} />
                  <text className="eje" x={M.izquierda - 6} y={y(v)} dy="0.32em" textAnchor="end">
                    {v}
                  </text>
                </g>
              ))}
              {ticksX.map(t => (
                <text key={t} className="eje" x={x(t)} y={altoReal - 6} textAnchor="middle">
                  {fmtX(t)}
                </text>
              ))}
              {ahora !== undefined && ahora > desde && ahora < hasta && (
                <g>
                  <line className="marca-ahora" x1={x(ahora)} x2={x(ahora)} y1={M.arriba} y2={M.arriba + altoPlot} />
                  <text className="eje" x={x(ahora) - 4} y={M.arriba + 10} textAnchor="end">now</text>
                </g>
              )}
              {series.map((s, i) => (
                <path
                  key={s.id}
                  d={caminos[i]}
                  fill="none"
                  stroke={s.color}
                  strokeWidth="2"
                  strokeLinejoin="round"
                  strokeLinecap="round"
                  strokeDasharray={s.discontinua ? "5 4" : undefined}
                />
              ))}
              {cursorT !== null && hayEnCursor && (
                <g pointerEvents="none">
                  <line className="cursor" x1={x(cursorT)} x2={x(cursorT)} y1={M.arriba} y2={M.arriba + altoPlot} />
                  {enCursor.map(({ s, p }) =>
                    p ? <circle key={s.id} cx={x(p.t)} cy={y(p.v as number)} r="4.5" fill={s.color} className="punto-cursor" /> : null,
                  )}
                </g>
              )}
              <rect
                x={M.izquierda}
                y={M.arriba}
                width={anchoPlot}
                height={altoPlot}
                fill="transparent"
                onPointerMove={alMover}
                onPointerDown={alMover}
                onPointerLeave={() => setCursorT(null)}
              />
            </svg>
          )}
          {!conDatos && <p className="vacio superpuesto">{vacio}</p>}
          {cursorT !== null && hayEnCursor && (
            <div
              className="tooltip"
              style={{
                left: Math.min(Math.max(x(cursorT), 70), ancho - 70),
                top: M.arriba,
              }}
            >
              <div className="tooltip-hora">{horaConSegundos(cursorT)}</div>
              {enCursor.map(({ s, p }) => (
                <div key={s.id} className="tooltip-fila">
                  <span className="muestra" style={{ background: s.color }} aria-hidden="true" />
                  <span>{s.nombre}</span>
                  <strong>{p ? `${fmtV(p.v as number)} ${unidad}` : "—"}</strong>
                </div>
              ))}
            </div>
          )}
        </div>
      )}
    </figure>
  )
}
