import { horaConSegundos } from "../utilidades/tiempo"
import { IconoCaida } from "./Iconos"

/** Aviso destacado mientras el ESP32 reporte `caida: true`. */
export function AlertaCaida({ inicio }: { inicio: number | null }) {
  if (inicio === null) return null
  return (
    <div className="alerta-caida" role="alert">
      <IconoCaida tam={32} />
      <div>
        <strong>Caída detectada</strong>
        <p>
          Reportada por la pulsera ({horaConSegundos(inicio)}). La persona tiene 15 s para cancelarla con el botón;
          después suena la alarma completa.
        </p>
      </div>
    </div>
  )
}
