import { useEffect, useRef, useState, type FormEvent } from "react"
import type { Cmd } from "../tipos"
import { idSesion } from "../utilidades/tiempo"

const ETIQUETAS = ["reposo", "caminar", "correr", "escaleras", "transicion"]
// Los valores se envían tal cual (dataset y modelo); el label solo traduce la sugerencia en pantalla.
const ETIQUETAS_EN: Record<string, string> = {
  reposo: "rest",
  caminar: "walk",
  correr: "run",
  escaleras: "stairs",
  transicion: "transition",
}

interface Props {
  conectado: boolean
  publicar: (cmd: Cmd) => Promise<void>
  ahora: number
}

interface Grabacion {
  sesion: string
  label: string
  inicio: number
  fin: number | null
}

/**
 * Equivalente web de herramientas/control.py: manda `start` con sesión, etiqueta y sujeto,
 * y `stop` al pulsar el botón o al cumplirse la duración.
 */
export function PanelCaptura({ conectado, publicar, ahora }: Props) {
  const [label, setLabel] = useState("reposo")
  const [sujeto, setSujeto] = useState("s01")
  const [duracion, setDuracion] = useState(60)
  const [grabacion, setGrabacion] = useState<Grabacion | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [enviando, setEnviando] = useState(false)
  const temporizador = useRef<ReturnType<typeof setTimeout> | undefined>(undefined)

  const detener = async () => {
    clearTimeout(temporizador.current)
    setEnviando(true)
    try {
      await publicar({ cmd: "stop" })
      setGrabacion(null)
      setError(null)
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e))
    } finally {
      setEnviando(false)
    }
  }
  const detenerRef = useRef(detener)
  useEffect(() => {
    detenerRef.current = detener
  })
  useEffect(() => () => clearTimeout(temporizador.current), [])

  const iniciar = async (e: FormEvent) => {
    e.preventDefault()
    const etiqueta = label.trim().toLowerCase()
    const persona = sujeto.trim()
    if (!etiqueta || !persona) return
    const sesion = idSesion()
    setEnviando(true)
    try {
      await publicar({ cmd: "start", sesion, label: etiqueta, sujeto: persona })
      const inicio = Date.now()
      const fin = duracion > 0 ? inicio + duracion * 1000 : null
      setGrabacion({ sesion, label: etiqueta, inicio, fin })
      setError(null)
      if (fin) temporizador.current = setTimeout(() => void detenerRef.current(), duracion * 1000)
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err))
    } finally {
      setEnviando(false)
    }
  }

  const transcurrido = grabacion ? Math.max(0, Math.floor((ahora - grabacion.inicio) / 1000)) : 0

  return (
    <section className="panel" aria-labelledby="titulo-captura">
      <h2 id="titulo-captura" className="panel-titulo">Capture session</h2>
      {grabacion ? (
        <div className="grabando">
          <p>
            <span className="punto-grabando" aria-hidden="true" /> Recording <strong>{grabacion.label}</strong> · session{" "}
            <code>{grabacion.sesion}</code>
          </p>
          <p className="tarjeta-pie">
            {transcurrido} s{grabacion.fin ? ` of ${Math.round((grabacion.fin - grabacion.inicio) / 1000)} s` : ""} · the
            stop is sent from this tab; do not close it.
          </p>
          <button type="button" className="boton boton-peligro" onClick={() => void detener()} disabled={enviando}>
            Stop
          </button>
        </div>
      ) : (
        <form className="formulario" onSubmit={e => void iniciar(e)}>
          <label>
            Activity
            <input list="etiquetas" value={label} onChange={e => setLabel(e.target.value)} required />
            <datalist id="etiquetas">
              {ETIQUETAS.map(l => (
                <option key={l} value={l} label={ETIQUETAS_EN[l]} />
              ))}
            </datalist>
          </label>
          <label>
            Subject
            <input value={sujeto} onChange={e => setSujeto(e.target.value)} pattern="[A-Za-z0-9_-]+" required title="Anonymous ID, e.g. s01" />
          </label>
          <label>
            Duration (s)
            <input type="number" min={0} max={3600} value={duracion} onChange={e => setDuracion(Number(e.target.value))} />
          </label>
          <button type="submit" className="boton boton-primario" disabled={!conectado || enviando}>
            Start
          </button>
          <p className="tarjeta-pie formulario-nota">
            Use anonymous IDs (s01), never names. Duration 0 = until you press “Stop”.
          </p>
        </form>
      )}
      {error && (
        <p className="error-linea" role="alert">
          Could not send the command: {error}
        </p>
      )}
    </section>
  )
}
