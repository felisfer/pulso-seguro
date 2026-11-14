import type { CSSProperties } from "react"

interface Props {
  /** FC en lpm; `null` detiene el latido (sin lectura o datos viejos). */
  fc: number | null
  /** Tamaño fijo en px; si se omite, llena su contenedor. */
  tam?: number
}

/**
 * Corazón que late al ritmo de la FC medida: un ciclo "lub-dub" cada 60/FC segundos.
 * La duración se redondea a 50 ms para no reiniciar la animación con cada variación mínima.
 */
export function CorazonLatiendo({ fc, tam }: Props) {
  const late = fc !== null && fc >= 20 && fc <= 250
  const duracion = late ? Math.round(60_000 / fc / 50) * 50 : 1000
  const estilo = { "--duracion-latido": `${duracion}ms`, ...(tam ? { width: tam, height: tam } : {}) } as CSSProperties

  return (
    <div className={`corazon ${late ? "latiendo" : "detenido"}`} style={estilo} aria-hidden="true">
      <span className="corazon-onda" />
      <svg viewBox="0 0 64 64" className="corazon-svg">
        <defs>
          <linearGradient id="corazon-relleno" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stopColor="#f0605d" />
            <stop offset="1" stopColor="#c42d2d" />
          </linearGradient>
        </defs>
        <path
          d="M32 56 C 14 43, 4 33, 4 21 A 13 13 0 0 1 32 15 A 13 13 0 0 1 60 21 C 60 33, 50 43, 32 56 Z"
          fill="url(#corazon-relleno)"
        />
        {/* Brillo */}
        <path d="M14 18 a 8 8 0 0 1 9 -5" fill="none" stroke="#fff" strokeOpacity="0.55" strokeWidth="3.5" strokeLinecap="round" />
        {/* Trazo de ECG */}
        <path
          d="M10 33 h 10 l 4 -8 l 5 15 l 5 -12 l 3 5 h 17"
          fill="none"
          stroke="#fff"
          strokeWidth="2.6"
          strokeLinecap="round"
          strokeLinejoin="round"
        />
      </svg>
    </div>
  )
}
