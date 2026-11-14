// Diseño (posición y tamaño) de los paneles del monitor en la cuadrícula editable.
// Unidades: columnas (x, w) y filas de ALTO_FILA px (y, h). Entre filas hay MARGEN px.

import type { Layout, LayoutItem, ResponsiveLayouts } from "react-grid-layout"

export const ALTO_FILA = 20
export const MARGEN: [number, number] = [16, 16]

export type Punto = "lg" | "md" | "sm"
export const PUNTOS: Record<Punto, number> = { lg: 1100, md: 700, sm: 0 }
export const COLUMNAS: Record<Punto, number> = { lg: 12, md: 6, sm: 1 }

export type IdPanel =
  | "estado"
  | "fc"
  | "spo2"
  | "sensor"
  | "tendencias"
  | "eventos"
  | "ppg"
  | "captura"
  | "historial"

export const NOMBRES_PANEL: Record<IdPanel, string> = {
  estado: "Overall status",
  fc: "Heart rate",
  spo2: "SpO₂",
  sensor: "Sensor",
  tendencias: "Live trends",
  eventos: "Recent events",
  ppg: "Pulse wave",
  captura: "Capture session",
  historial: "Prediction history",
}

const TARJETAS: IdPanel[] = ["estado", "fc", "spo2", "sensor"]
const MIN_TARJETA = { minW: 2, minH: 4 }
const MIN_PANEL = { minW: 3, minH: 6 }

const item = (i: IdPanel, x: number, y: number, w: number, h: number): LayoutItem => ({
  i,
  x,
  y,
  w,
  h,
  ...(TARJETAS.includes(i) ? MIN_TARJETA : MIN_PANEL),
})

/** En 6 o 1 columnas los mínimos de ancho no pueden pasar del total. */
const ajustarMin = (items: LayoutItem[], cols: number) =>
  items.map(it => ({ ...it, minW: Math.min(it.minW ?? 1, cols) }))

const LG: Layout = [
  item("estado", 0, 0, 3, 4),
  item("fc", 3, 0, 3, 4),
  item("spo2", 6, 0, 3, 4),
  item("sensor", 9, 0, 3, 4),
  item("tendencias", 0, 4, 9, 15),
  item("eventos", 9, 4, 3, 15),
  item("ppg", 0, 19, 12, 8),
  item("captura", 0, 27, 6, 11),
  item("historial", 6, 27, 6, 11),
]

const MD: Layout = ajustarMin(
  [
    item("estado", 0, 0, 3, 4),
    item("fc", 3, 0, 3, 4),
    item("spo2", 0, 4, 3, 4),
    item("sensor", 3, 4, 3, 4),
    item("tendencias", 0, 8, 6, 15),
    item("eventos", 0, 23, 6, 10),
    item("ppg", 0, 33, 6, 8),
    item("captura", 0, 41, 6, 11),
    item("historial", 0, 52, 6, 11),
  ],
  6,
)

// Móvil: una columna; el orden de arriba abajo es el de la lista.
const SM: Layout = ajustarMin(
  (
    [
      ["estado", 4],
      ["fc", 4],
      ["spo2", 4],
      ["sensor", 4],
      ["tendencias", 16],
      ["eventos", 10],
      ["ppg", 9],
      ["captura", 14],
      ["historial", 12],
    ] as [IdPanel, number][]
  ).reduce<LayoutItem[]>((acc, [i, h]) => {
    const y = acc.length ? acc[acc.length - 1].y + acc[acc.length - 1].h : 0
    return [...acc, item(i, 0, y, 1, h)]
  }, []),
  1,
)

/** Diseño inicial, sin los paneles que no se muestran (p. ej. captura sin VITE_ENABLE_CMD). */
export function disenoInicial(visibles: IdPanel[]): ResponsiveLayouts<Punto> {
  const filtrar = (l: Layout) => l.filter(it => visibles.includes(it.i as IdPanel))
  return { lg: filtrar(LG), md: filtrar(MD), sm: filtrar(SM) }
}

// ---------- Guardado en el navegador ----------
// Se guarda por usuario en localStorage. Solo las anchuras editables (lg y md);
// en móvil (sm) siempre se usa el diseño fijo de una columna.
// Si cambias los paneles o sus tamaños por defecto de forma incompatible, sube VERSION.

const VERSION = 1
const PUNTOS_GUARDADOS: Punto[] = ["lg", "md"]

export const claveDiseno = (usuario?: string) => `pulso-seguro:diseno:v${VERSION}:${usuario || "anonimo"}`

const esItemValido = (x: unknown): x is LayoutItem => {
  const it = x as Partial<LayoutItem>
  return (
    typeof it?.i === "string" &&
    [it.x, it.y, it.w, it.h].every(n => typeof n === "number" && Number.isFinite(n) && n >= 0) &&
    (it.w ?? 0) > 0 &&
    (it.h ?? 0) > 0
  )
}

/**
 * Diseño guardado del usuario, combinado con el inicial: se ignoran paneles que ya no
 * se muestran y se agregan con su posición inicial los que falten. Respeta los mínimos actuales.
 */
export function cargarDiseno(clave: string, visibles: IdPanel[]): ResponsiveLayouts<Punto> {
  const inicial = disenoInicial(visibles)
  let guardado: Partial<Record<Punto, unknown>>
  try {
    guardado = JSON.parse(localStorage.getItem(clave) ?? "{}") ?? {}
  } catch {
    return inicial
  }
  const resultado = { ...inicial }
  for (const p of PUNTOS_GUARDADOS) {
    const lista = Array.isArray(guardado[p]) ? (guardado[p] as unknown[]).filter(esItemValido) : []
    if (lista.length === 0) continue
    const cols = COLUMNAS[p]
    resultado[p] = (inicial[p] ?? []).map(base => {
      const g = lista.find(it => it.i === base.i)
      if (!g) return base
      const w = Math.min(Math.max(g.w, base.minW ?? 1), cols)
      return { ...base, x: Math.min(g.x, cols - w), y: g.y, w, h: Math.max(g.h, base.minH ?? 1) }
    })
  }
  return resultado
}

export function guardarDiseno(clave: string, disenos: ResponsiveLayouts<Punto>) {
  const datos: Partial<Record<Punto, { i: string; x: number; y: number; w: number; h: number }[]>> = {}
  for (const p of PUNTOS_GUARDADOS) {
    datos[p] = (disenos[p] ?? []).map(({ i, x, y, w, h }) => ({ i, x, y, w, h }))
  }
  try {
    localStorage.setItem(clave, JSON.stringify(datos))
  } catch {
    /* almacenamiento lleno o bloqueado: el diseño solo dura esta sesión */
  }
}

export function borrarDiseno(clave: string) {
  try {
    localStorage.removeItem(clave)
  } catch {
    /* almacenamiento bloqueado */
  }
}
