import { useEffect, useState } from "react"

/** Hora actual que se refresca cada `intervaloMs` (para "hace X s" y ejes de tiempo). */
export function useAhora(intervaloMs = 1000) {
  const [ahora, setAhora] = useState(() => Date.now())
  useEffect(() => {
    const id = setInterval(() => setAhora(Date.now()), intervaloMs)
    return () => clearInterval(id)
  }, [intervaloMs])
  return ahora
}

const fmtHora = new Intl.DateTimeFormat("es-MX", { hour: "2-digit", minute: "2-digit", second: "2-digit" })
const fmtHoraCorta = new Intl.DateTimeFormat("es-MX", { hour: "2-digit", minute: "2-digit" })
const fmtFechaHora = new Intl.DateTimeFormat("es-MX", {
  day: "2-digit",
  month: "short",
  hour: "2-digit",
  minute: "2-digit",
})

export const horaConSegundos = (t: number) => fmtHora.format(t)
export const horaCorta = (t: number) => fmtHoraCorta.format(t)
export const fechaHora = (t: number) => fmtFechaHora.format(t)

export function haceCuanto(ms: number) {
  const s = Math.max(0, Math.round(ms / 1000))
  if (s < 60) return `hace ${s} s`
  const m = Math.round(s / 60)
  if (m < 60) return `hace ${m} min`
  return `hace ${Math.round(m / 60)} h`
}

/** Identificador de sesión con el mismo formato que control.py: 20261113_142530 */
export function idSesion(fecha = new Date()) {
  const p = (n: number) => String(n).padStart(2, "0")
  return (
    `${fecha.getFullYear()}${p(fecha.getMonth() + 1)}${p(fecha.getDate())}_` +
    `${p(fecha.getHours())}${p(fecha.getMinutes())}${p(fecha.getSeconds())}`
  )
}
