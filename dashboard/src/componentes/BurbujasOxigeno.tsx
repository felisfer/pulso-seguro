import type { CSSProperties } from "react"

interface Props {
  /** `false` detiene las burbujas (sin lectura o datos viejos). */
  activo: boolean
  tam?: number
}

// Cada burbuja: posición x, radio, duración y retraso del ascenso (s), y la altura
// fija (unidades SVG) donde se queda si el sistema pide reducir movimiento.
const BURBUJAS = [
  { x: 20, r: 3.2, dur: 3.2, retraso: 0, quieta: 12 },
  { x: 32, r: 4.5, dur: 3.8, retraso: 1.1, quieta: 30 },
  { x: 44, r: 2.6, dur: 2.9, retraso: 0.5, quieta: 20 },
  { x: 26, r: 2.2, dur: 2.6, retraso: 2.0, quieta: 36 },
  { x: 39, r: 3.4, dur: 3.4, retraso: 2.6, quieta: 8 },
  { x: 49, r: 2, dur: 2.4, retraso: 1.6, quieta: 28 },
]

/**
 * Agua con burbujas que suben hasta la superficie, como metáfora de la oxigenación.
 */
export function BurbujasOxigeno({ activo, tam = 64 }: Props) {
  return (
    <div className={`burbujas ${activo ? "activas" : "detenidas"}`} style={{ width: tam, height: tam }} aria-hidden="true">
      <svg viewBox="0 0 64 64">
        <defs>
          <clipPath id="burbujas-recorte">
            <circle cx="32" cy="32" r="29" />
          </clipPath>
          <linearGradient id="burbujas-agua" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stopColor="#6db7f2" />
            <stop offset="1" stopColor="#1c5cab" />
          </linearGradient>
        </defs>

        <g clipPath="url(#burbujas-recorte)">
          {/* Cielo sobre la superficie */}
          <rect x="0" y="0" width="64" height="64" className="burbujas-cielo" />
          {/* Agua con superficie ondulada que se desplaza */}
          <g className="burbujas-ola">
            <path
              d="M-64 16 q 8 -4 16 0 t 16 0 t 16 0 t 16 0 t 16 0 t 16 0 t 16 0 t 16 0 V 64 H -64 Z"
              fill="url(#burbujas-agua)"
            />
          </g>
          {BURBUJAS.map((b, i) => (
            <circle
              key={i}
              className="burbuja"
              cx={b.x}
              cy={60}
              r={b.r}
              style={{ "--dur": `${b.dur}s`, "--retraso": `${b.retraso}s`, "--quieta": `${-b.quieta}px` } as CSSProperties}
            />
          ))}
        </g>
        <circle cx="32" cy="32" r="29" className="burbujas-borde" />
      </svg>
    </div>
  )
}
