import { useEffect, useRef } from "react"
import type { MuestraPPG } from "../tipos"
import { useTamano } from "../utilidades/useTamano"

interface Props {
  muestras: MuestraPPG[]
  /** Alto fijo en px. Si se omite, la onda llena el espacio libre de su panel. */
  alto?: number
}

const ALTO_MIN = 80

/**
 * Onda PPG (canal IR) de los últimos segundos. Se invierte el IR para que el
 * pulso sistólico apunte hacia arriba (más sangre = menos luz reflejada).
 */
export function OndaPPG({ muestras, alto: altoFijo }: Props) {
  const lienzo = useRef<HTMLCanvasElement>(null)
  const [contenedor, { ancho, alto: altoMedido }] = useTamano<HTMLDivElement>()
  const llenar = altoFijo === undefined
  const alto = altoFijo ?? Math.max(ALTO_MIN, altoMedido)

  useEffect(() => {
    const canvas = lienzo.current
    if (!canvas || ancho === 0) return
    const dpr = window.devicePixelRatio || 1
    canvas.width = ancho * dpr
    canvas.height = alto * dpr
    const ctx = canvas.getContext("2d")
    if (!ctx) return
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
    ctx.clearRect(0, 0, ancho, alto)

    const estilos = getComputedStyle(canvas)
    const colorLinea = estilos.getPropertyValue("--serie-1").trim() || "#2a78d6"
    const colorRejilla = estilos.getPropertyValue("--rejilla").trim() || "#e5e4e7"

    ctx.strokeStyle = colorRejilla
    ctx.lineWidth = 1
    for (let i = 1; i < 6; i++) {
      const x = Math.round((ancho * i) / 6) + 0.5
      ctx.beginPath()
      ctx.moveTo(x, 0)
      ctx.lineTo(x, alto)
      ctx.stroke()
    }

    if (muestras.length < 2) return
    // Quita la deriva lenta restando una media móvil de ~1 s.
    const ir = muestras.map(m => -m.ir)
    const ventana = 100
    let acumulado = 0
    const filtrada = ir.map((v, i) => {
      acumulado += v
      if (i >= ventana) acumulado -= ir[i - ventana]
      return v - acumulado / Math.min(i + 1, ventana)
    })
    const visibles = filtrada.slice(Math.min(ventana, Math.floor(filtrada.length / 3)))
    const lo = Math.min(...visibles)
    const hi = Math.max(...visibles)
    const rango = hi - lo || 1
    const pad = 10
    const total = 600 // eje fijo de 6 s
    const desplazamiento = total - visibles.length

    ctx.strokeStyle = colorLinea
    ctx.lineWidth = 2
    ctx.lineJoin = "round"
    ctx.beginPath()
    visibles.forEach((v, i) => {
      const x = ((i + desplazamiento) / (total - 1)) * ancho
      const y = pad + (1 - (v - lo) / rango) * (alto - 2 * pad)
      if (i === 0) ctx.moveTo(x, y)
      else ctx.lineTo(x, y)
    })
    ctx.stroke()
  }, [muestras, ancho, alto])

  return (
    <div ref={contenedor} className={`ppg-lienzo ${llenar ? "ppg-llena" : ""}`} style={llenar ? undefined : { height: alto }}>
      <canvas
        ref={lienzo}
        style={{ width: ancho, height: alto }}
        role="img"
        aria-label="Onda de pulso (PPG, canal infrarrojo) de los últimos 6 segundos"
      />
    </div>
  )
}
