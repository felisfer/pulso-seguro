import type { ReactNode } from "react"
import type { VitalsRecibidos } from "../telemetria/estado"
import type { Prediccion, Semaforo } from "../tipos"
import { haceCuanto } from "../utilidades/tiempo"
import { BurbujasOxigeno } from "./BurbujasOxigeno"
import { CorazonLatiendo } from "./CorazonLatiendo"
import { IconoAlerta, IconoCorazon, IconoGota, IconoInfo, IconoMano, IconoOk } from "./Iconos"

const VITALS_VIEJOS_MS = 5000

const SEMAFORO: Record<Semaforo, { texto: string; detalle: string; clase: string; icono: ReactNode }> = {
  verde: { texto: "Normal", detalle: "Vital signs in range", clase: "estado-bien", icono: <IconoOk tam={22} /> },
  amarillo: {
    texto: "Caution",
    detalle: "HR or SpO₂ slightly out of the usual range",
    clase: "estado-precaucion",
    icono: <IconoAlerta tam={22} />,
  },
  rojo: { texto: "Alert", detalle: "HR or SpO₂ out of range", clase: "estado-critico", icono: <IconoAlerta tam={22} /> },
}

export type TipoTarjeta = "estado" | "fc" | "spo2" | "sensor"

interface Props {
  tipo: TipoTarjeta
  vitals: VitalsRecibidos | null
  pred: Prediccion | null
  ahora: number
}

/** Una de las cuatro tarjetas de signos vitales (cada una es un panel independiente de la cuadrícula). */
export function TarjetaVital({ tipo, vitals, pred, ahora }: Props) {
  const viejo = !vitals || ahora - vitals.recibido > VITALS_VIEJOS_MS
  const pie = vitals ? (viejo ? `No new data · ${haceCuanto(ahora - vitals.recibido)}` : "Live") : "Waiting for vitals…"

  switch (tipo) {
    case "estado": {
      const sem = vitals ? SEMAFORO[vitals.semaforo] : null
      return (
        <article className={`tarjeta tarjeta-tono tarjeta-estado ${sem && !viejo ? sem.clase : "estado-neutro"}`}>
          <h2 className="tarjeta-titulo">Overall status</h2>
          <div className="estado-valor">
            {sem ? sem.icono : <IconoInfo tam={22} />}
            <span>{sem ? sem.texto : "No data"}</span>
          </div>
          <p className="tarjeta-pie">{sem ? sem.detalle : pie}</p>
        </article>
      )
    }

    case "fc": {
      // Solo muestra la predicción si es reciente (la Lambda publica cada 2 s).
      const predVigente = pred && ahora - pred.ts < 15_000 ? pred : null
      const delta = predVigente && vitals?.fc != null ? predVigente.fc_pred - vitals.fc : null
      return (
        <article className={`tarjeta tarjeta-ilustrada ${viejo ? "vieja" : ""}`}>
          <h2 className="tarjeta-titulo">
            <IconoCorazon /> Heart rate
          </h2>
          <p className="valor">
            <span className="numero">{vitals?.fc ?? "—"}</span>
            <span className="unidad">bpm</span>
          </p>
          <div className="tarjeta-ilustracion">
            <CorazonLatiendo fc={viejo ? null : (vitals?.fc ?? null)} />
          </div>
          <p className="tarjeta-pie">
            {predVigente ? (
              <>
                In {predVigente.horizonte_s} s: <strong>{Math.round(predVigente.fc_pred)} bpm</strong>
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
      )
    }

    case "spo2":
      return (
        <article className={`tarjeta tarjeta-ilustrada ${viejo ? "vieja" : ""}`}>
          <h2 className="tarjeta-titulo">
            <IconoGota /> SpO₂
          </h2>
          <p className="valor">
            <span className="numero">{vitals?.spo2 ?? "—"}</span>
            <span className="unidad">%</span>
          </p>
          <div className="tarjeta-ilustracion">
            <BurbujasOxigeno activo={!viejo && vitals?.spo2 != null} />
          </div>
          <p className="tarjeta-pie">{pie}</p>
        </article>
      )

    case "sensor": {
      // Verde con contacto, rojo sin contacto; neutro si no hay datos recientes.
      const clase = !vitals || viejo ? "estado-neutro" : vitals.dedo ? "estado-bien" : "estado-critico"
      return (
        <article className={`tarjeta tarjeta-tono ${clase}`}>
          <h2 className="tarjeta-titulo">
            <IconoMano /> Sensor
          </h2>
          <p className="valor valor-texto">
            {vitals && !viejo && (vitals.dedo ? <IconoOk tam={22} /> : <IconoAlerta tam={22} />)}
            {vitals ? (vitals.dedo ? "In contact" : "No contact") : "—"}
          </p>
          <p className="tarjeta-pie">{vitals && !vitals.dedo ? "Adjust the wristband: skin not detected" : pie}</p>
        </article>
      )
    }
  }
}
