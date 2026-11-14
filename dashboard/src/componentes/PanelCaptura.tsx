import { useEffect, useRef, useState, type FormEvent } from "react"
import type { Cmd } from "../tipos"
import { idSesion } from "../utilidades/tiempo"

const ETIQUETAS = ["reposo", "caminar", "correr", "escaleras", "transicion"]

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
      <h2 id="titulo-captura" className="panel-titulo">Sesión de captura</h2>
      {grabacion ? (
        <div className="grabando">
          <p>
            <span className="punto-grabando" aria-hidden="true" /> Grabando <strong>{grabacion.label}</strong> · sesión{" "}
            <code>{grabacion.sesion}</code>
          </p>
          <p className="tarjeta-pie">
            {transcurrido} s{grabacion.fin ? ` de ${Math.round((grabacion.fin - grabacion.inicio) / 1000)} s` : ""} · el
            stop se envía desde esta pestaña; no la cierres.
          </p>
          <button type="button" className="boton boton-peligro" onClick={() => void detener()} disabled={enviando}>
            Detener (stop)
          </button>
        </div>
      ) : (
        <form className="formulario" onSubmit={e => void iniciar(e)}>
          <label>
            Actividad
            <input list="etiquetas" value={label} onChange={e => setLabel(e.target.value)} required />
            <datalist id="etiquetas">
              {ETIQUETAS.map(l => (
                <option key={l} value={l} />
              ))}
            </datalist>
          </label>
          <label>
            Sujeto
            <input value={sujeto} onChange={e => setSujeto(e.target.value)} pattern="[A-Za-z0-9_-]+" required title="Identificador anónimo, p. ej. s01" />
          </label>
          <label>
            Duración (s)
            <input type="number" min={0} max={3600} value={duracion} onChange={e => setDuracion(Number(e.target.value))} />
          </label>
          <button type="submit" className="boton boton-primario" disabled={!conectado || enviando}>
            Iniciar (start)
          </button>
          <p className="tarjeta-pie formulario-nota">
            Usa identificadores anónimos (s01), nunca nombres. Duración 0 = hasta pulsar «Detener».
          </p>
        </form>
      )}
      {error && (
        <p className="error-linea" role="alert">
          No se pudo enviar el comando: {error}
        </p>
      )}
    </section>
  )
}
