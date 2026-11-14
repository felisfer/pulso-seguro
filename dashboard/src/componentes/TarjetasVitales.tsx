import type { ReactNode } from "react"
import type { VitalsRecibidos } from "../telemetria/estado"
import type { Prediccion, Semaforo } from "../tipos"
import { haceCuanto } from "../utilidades/tiempo"
import { IconoAlerta, IconoCorazon, IconoGota, IconoInfo, IconoMano, IconoOk } from "./Iconos"

const VITALS_VIEJOS_MS = 5000

const SEMAFORO: Record<Semaforo, { texto: string; detalle: string; clase: string; icono: ReactNode }> = {
  verde: { texto: "Normal", detalle: "Signos vitales en rango", clase: "estado-bien", icono: <IconoOk tam={22} /> },
  amarillo: {
    texto: "Precaución",
    detalle: "FC o SpO₂ levemente fuera del promedio",
    clase: "estado-precaucion",
    icono: <IconoAlerta tam={22} />,
  },
  rojo: { texto: "Alerta", detalle: "FC o SpO₂ fuera de rango", clase: "estado-critico", icono: <IconoAlerta tam={22} /> },
}

interface Props {
  vitals: VitalsRecibidos | null
  pred: Prediccion | null
  ahora: number
}

export function TarjetasVitales({ vitals, pred, ahora }: Props) {
  const viejo = !vitals || ahora - vitals.recibido > VITALS_VIEJOS_MS
  const pie = vitals ? (viejo ? `Sin datos nuevos · ${haceCuanto(ahora - vitals.recibido)}` : "En vivo") : "Esperando vitals…"
  const sem = vitals ? SEMAFORO[vitals.semaforo] : null
  // Solo muestra la predicción si es reciente (la Lambda publica cada 2 s).
  const predVigente = pred && ahora - pred.ts < 15_000 ? pred : null
  // Verde con contacto, rojo sin contacto; neutro si no hay datos recientes.
  const claseSensor = !vitals || viejo ? "estado-neutro" : vitals.dedo ? "estado-bien" : "estado-critico"
  const delta = predVigente && vitals?.fc != null ? predVigente.fc_pred - vitals.fc : null

  return (
    <section className="tarjetas" aria-label="Signos vitales actuales">
      <article className={`tarjeta tarjeta-tono tarjeta-estado ${sem && !viejo ? sem.clase : "estado-neutro"}`}>
        <h2 className="tarjeta-titulo">Estado general</h2>
        <div className="estado-valor">
          {sem ? sem.icono : <IconoInfo tam={22} />}
          <span>{sem ? sem.texto : "Sin datos"}</span>
        </div>
        <p className="tarjeta-pie">{sem ? sem.detalle : pie}</p>
      </article>

      <article className={`tarjeta ${viejo ? "vieja" : ""}`}>
        <h2 className="tarjeta-titulo">
          <IconoCorazon /> Frecuencia cardiaca
        </h2>
        <p className="valor">
          <span className="numero">{vitals?.fc ?? "—"}</span>
          <span className="unidad">lpm</span>
        </p>
        <p className="tarjeta-pie">
          {predVigente ? (
            <>
              En {predVigente.horizonte_s} s: <strong>{Math.round(predVigente.fc_pred)} lpm</strong>
              {delta !== null && Math.abs(delta) >= 1 && (
                <span className="tendencia">
                  {" "}
                  ({delta > 0 ? "↑" : "↓"} {Math.abs(Math.round(delta))})
                </span>
              )}
            </>
          ) : (
            pie
          )}
        </p>
      </article>

      <article className={`tarjeta ${viejo ? "vieja" : ""}`}>
        <h2 className="tarjeta-titulo">
          <IconoGota /> SpO₂
        </h2>
        <p className="valor">
          <span className="numero">{vitals?.spo2 ?? "—"}</span>
          <span className="unidad">%</span>
        </p>
        <p className="tarjeta-pie">{pie}</p>
      </article>

      <article className={`tarjeta tarjeta-tono ${claseSensor}`}>
        <h2 className="tarjeta-titulo">
          <IconoMano /> Sensor
        </h2>
        <p className="valor valor-texto">
          {vitals && !viejo && (vitals.dedo ? <IconoOk tam={22} /> : <IconoAlerta tam={22} />)}
          {vitals ? (vitals.dedo ? "Con contacto" : "Sin contacto") : "—"}
        </p>
        <p className="tarjeta-pie">{vitals && !vitals.dedo ? "Ajusta la pulsera: no detecta la piel" : pie}</p>
      </article>
    </section>
  )
}

