import { useState } from "react"
import { config, faltantesIot } from "../config"
import { IconoCorazon, IconoSalir } from "./Iconos"
import { Monitor } from "./Monitor"

const CLAVE_THING = "pulso-seguro:thing"

function thingGuardado() {
  try {
    const guardado = localStorage.getItem(CLAVE_THING)
    if (guardado && config.things.includes(guardado)) return guardado
  } catch {
    /* almacenamiento no disponible */
  }
  return config.things[0] ?? ""
}

interface Props {
  usuario?: string
  idToken?: string
  onSalir?: () => void
}

export function Tablero({ usuario, idToken, onSalir }: Props) {
  const [thing, setThing] = useState(thingGuardado)
  const faltan = faltantesIot()

  const elegir = (nuevo: string) => {
    setThing(nuevo)
    try {
      localStorage.setItem(CLAVE_THING, nuevo)
    } catch {
      /* almacenamiento no disponible */
    }
  }

  return (
    <div className="app">
      <header className="encabezado">
        <div className="marca">
          <span className="marca-logo" aria-hidden="true">
            <IconoCorazon tam={20} />
          </span>
          <span>PulsoSeguro</span>
        </div>

        {config.things.length > 1 ? (
          <label className="selector-thing">
            <span className="solo-lector">Pulsera</span>
            <select value={thing} onChange={e => elegir(e.target.value)}>
              {config.things.map(t => (
                <option key={t} value={t}>
                  {t}
                </option>
              ))}
            </select>
          </label>
        ) : (
          thing && <code className="thing-unico">{thing}</code>
        )}

        <div className="usuario">
          {usuario && <span className="usuario-email">{usuario}</span>}
          {onSalir && (
            <button type="button" className="boton boton-texto-icono" onClick={onSalir}>
              <IconoSalir /> Salir
            </button>
          )}
        </div>
      </header>

      <main className="contenido">
        {faltan.length > 0 ? (
          <div className="aviso aviso-error" role="alert">
            <strong>Falta configuración para conectarse a AWS IoT:</strong>{" "}
            {faltan.join(", ")}. Agrégalas (ver <code>.env.example</code>) a <code>.env</code> en desarrollo o a{" "}
            <code>.env.production</code> en Docker y vuelve a compilar, o usa{" "}
            <code>VITE_DEMO=true</code> para ver la interfaz con datos simulados.
          </div>
        ) : (
          <Monitor key={thing} thing={thing} idToken={idToken} usuario={usuario} />
        )}
      </main>
    </div>
  )
}
