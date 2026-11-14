import { horaConSegundos } from "../utilidades/tiempo"
import { IconoAlerta, IconoCaida } from "./Iconos"

const TEXTOS = {
  caida: {
    titulo: "Fall detected",
    detalle: "The person has 15 s to cancel it with the button; after that the full alarm sounds.",
  },
  panico: {
    titulo: "Panic button pressed",
    detalle: "The person asked for help by holding the button for 3 s. The alarm stays active until it is silenced on the wristband.",
  },
} as const

/** Aviso destacado mientras el ESP32 reporte `caida: true` o `panico: true`. */
export function AlertaCaida({ inicio, tipo = "caida" }: { inicio: number | null; tipo?: keyof typeof TEXTOS }) {
  if (inicio === null) return null
  const { titulo, detalle } = TEXTOS[tipo]
  return (
    <div className="alerta-caida" role="alert">
      {tipo === "panico" ? <IconoAlerta tam={32} /> : <IconoCaida tam={32} />}
      <div>
        <strong>{titulo}</strong>
        <p>
          Reported by the wristband ({horaConSegundos(inicio)}). {detalle}
        </p>
      </div>
    </div>
  )
}
