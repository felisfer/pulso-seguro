import { horaConSegundos } from "../utilidades/tiempo"
import { IconoAlerta, IconoCaida } from "./Iconos"

const TEXTOS = {
  caida: {
    titulo: "Caída detectada",
    detalle: "La persona tiene 15 s para cancelarla con el botón; después suena la alarma completa.",
  },
  panico: {
    titulo: "Botón de pánico activado",
    detalle: "La persona pidió ayuda manteniendo el botón 3 s. La alarma sigue activa hasta que se silencie en la pulsera.",
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
          Reportada por la pulsera ({horaConSegundos(inicio)}). {detalle}
        </p>
      </div>
    </div>
  )
}
