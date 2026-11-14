import type { Evento, Severidad } from "../telemetria/estado"
import { horaConSegundos } from "../utilidades/tiempo"
import { IconoAlerta, IconoInfo, IconoOk } from "./Iconos"

const ICONO: Record<Severidad, typeof IconoOk> = {
  critico: IconoAlerta,
  aviso: IconoAlerta,
  info: IconoInfo,
  bien: IconoOk,
}

const ETIQUETA: Record<Severidad, string> = {
  critico: "Crítico",
  aviso: "Aviso",
  info: "Info",
  bien: "OK",
}

export function ListaEventos({ eventos }: { eventos: Evento[] }) {
  return (
    <section className="panel" aria-labelledby="titulo-eventos">
      <h2 id="titulo-eventos" className="panel-titulo">Eventos recientes</h2>
      {eventos.length === 0 ? (
        <p className="vacio">Sin eventos en esta sesión del navegador.</p>
      ) : (
        <ol className="eventos" aria-live="polite">
          {eventos.map(e => {
            const Icono = ICONO[e.severidad]
            return (
              <li key={e.id} className={`evento evento-${e.severidad}`}>
                <span className="evento-icono" title={ETIQUETA[e.severidad]}>
                  <Icono tam={16} />
                  <span className="solo-lector">{ETIQUETA[e.severidad]}:</span>
                </span>
                <span className="evento-texto">{e.texto}</span>
                <time className="evento-hora" dateTime={new Date(e.t).toISOString()}>
                  {horaConSegundos(e.t)}
                </time>
              </li>
            )
          })}
        </ol>
      )}
    </section>
  )
}
