import { useMemo, useState } from "react"
import { config } from "../config"
import type { Telemetria } from "../telemetria/useTelemetria"
import { useTelemetria } from "../telemetria/useTelemetria"
import { haceCuanto, useAhora } from "../utilidades/tiempo"
import { AlertaCaida } from "./AlertaCaida"
import { GraficaLinea } from "./GraficaLinea"
import { HistorialPredicciones } from "./HistorialPredicciones"
import { ListaEventos } from "./ListaEventos"
import { OndaPPG } from "./OndaPPG"
import { PanelCaptura } from "./PanelCaptura"
import { TarjetasVitales } from "./TarjetasVitales"

const VENTANAS = [
  { id: 1, nombre: "1 min" },
  { id: 5, nombre: "5 min" },
  { id: 10, nombre: "10 min" },
] as const

interface Props {
  thing: string
  idToken?: string
}

/** Monta con `key={thing}` para reiniciar todo al cambiar de pulsera. */
export function Monitor({ thing, idToken }: Props) {
  const tel = useTelemetria(thing, idToken)
  const ahora = useAhora(1000)
  const [minutos, setMinutos] = useState<(typeof VENTANAS)[number]["id"]>(5)

  const horizonte = (tel.pred?.horizonte_s ?? 10) * 1000
  const desde = ahora - minutos * 60_000
  const hasta = ahora + horizonte

  const seriesFc = useMemo(
    () => [
      {
        id: "fc",
        nombre: "FC medida",
        color: "var(--serie-1)",
        puntos: tel.historialVitals.map(v => ({ t: v.t, v: v.fc })),
      },
      {
        id: "pred",
        nombre: "FC predicha",
        color: "var(--serie-2)",
        discontinua: true,
        // Cada predicción se dibuja en el instante que predice (ts + horizonte).
        puntos: tel.historialPred.map(p => ({ t: p.ts + p.horizonte_s * 1000, v: p.fc_pred })),
      },
    ],
    [tel.historialVitals, tel.historialPred],
  )

  const serieSpo2 = useMemo(
    () => [{ id: "spo2", nombre: "SpO₂", color: "var(--serie-1)", puntos: tel.historialVitals.map(v => ({ t: v.t, v: v.spo2 })) }],
    [tel.historialVitals],
  )

  return (
    <>
      <BarraEstado tel={tel} ahora={ahora} />
      <AvisoConexion tel={tel} />
      <AlertaCaida inicio={tel.inicioCaida} />
      <TarjetasVitales vitals={tel.vitals} pred={tel.pred} ahora={ahora} />

      <div className="cuadricula">
        <section className="panel panel-ancho" aria-labelledby="titulo-tendencias">
          <div className="panel-encabezado">
            <h2 id="titulo-tendencias" className="panel-titulo">Tendencias en vivo</h2>
            <div className="segmentado" role="group" aria-label="Ventana de tiempo">
              {VENTANAS.map(v => (
                <button key={v.id} type="button" aria-pressed={minutos === v.id} onClick={() => setMinutos(v.id)}>
                  {v.nombre}
                </button>
              ))}
            </div>
          </div>
          <GraficaLinea
            titulo="Frecuencia cardiaca"
            unidad="lpm"
            series={seriesFc}
            desde={desde}
            hasta={hasta}
            ahora={ahora}
            yMin={60}
            yMax={100}
            alto={240}
            decimales={0}
          />
          <GraficaLinea titulo="SpO₂" unidad="%" series={serieSpo2} desde={desde} hasta={hasta} yMin={90} yMax={100} alto={160} />
        </section>

        <ListaEventos eventos={tel.eventos} />
      </div>

      <section className="panel" aria-labelledby="titulo-ppg">
        <div className="panel-encabezado">
          <h2 id="titulo-ppg" className="panel-titulo">Onda de pulso (PPG)</h2>
          <label className="interruptor">
            <input type="checkbox" checked={tel.verPPG} onChange={e => tel.setVerPPG(e.target.checked)} />
            <span>Recibir señal cruda</span>
          </label>
        </div>
        {tel.verPPG ? (
          <>
            <OndaPPG muestras={tel.ppg} />
            <p className="tarjeta-pie">
              {tel.sesionPPG
                ? `Sesión ${tel.sesionPPG.sesion} · ${tel.sesionPPG.label} · ${tel.sesionPPG.sujeto} · ${tel.muestrasPerdidas} muestras perdidas`
                : "Esperando lotes… la pulsera solo envía la señal cruda durante una sesión de captura (entre start y stop)."}
            </p>
          </>
        ) : (
          <p className="vacio">
            Desactivado por defecto: suscribirse a <code>data</code> suma ~2,400 mensajes/h de ~10 KB en AWS IoT.
          </p>
        )}
      </section>

      <div className="cuadricula cuadricula-pareja">
        {config.permitirComandos && (
          <PanelCaptura conectado={tel.conexion === "conectado"} publicar={tel.publicar} ahora={ahora} />
        )}
        <HistorialPredicciones thing={thing} idToken={idToken} />
      </div>

      {config.demo && (
        <section className="panel panel-demo" aria-label="Controles del modo demo">
          <p>
            <strong>Modo demo:</strong> los datos son simulados en el navegador con el mismo formato que los tópicos{" "}
            <code>vitals</code>, <code>pred</code> y <code>data</code>.
          </p>
          <div className="acciones-demo">
            <button type="button" className="boton" onClick={tel.simularSinContacto}>
              Simular sin contacto
            </button>
            <button type="button" className="boton" onClick={tel.simularCaida}>
              Simular caída
            </button>
          </div>
        </section>
      )}
    </>
  )
}

const TEXTO_CONEXION: Record<Telemetria["conexion"], string> = {
  conectando: "Conectando…",
  conectado: "Conectado a AWS IoT",
  reconectando: "Reconectando…",
  error: "Sin conexión",
}

function BarraEstado({ tel, ahora }: { tel: Telemetria; ahora: number }) {
  const claseConexion =
    tel.conexion === "conectado" ? "estado-bien" : tel.conexion === "error" ? "estado-critico" : "estado-aviso"
  const clasePulsera = tel.online === null ? "estado-neutro" : tel.online ? "estado-bien" : "estado-critico"
  return (
    <div className="barra-estado" role="status">
      <span className={`insignia ${claseConexion}`}>
        <span className="punto" aria-hidden="true" />
        {config.demo ? "Datos simulados" : TEXTO_CONEXION[tel.conexion]}
      </span>
      <span className={`insignia ${clasePulsera}`}>
        <span className="punto" aria-hidden="true" />
        Pulsera {tel.online === null ? "sin estado" : tel.online ? "en línea" : "desconectada"}
      </span>
      {tel.vitals && <span className="barra-texto">Último dato {haceCuanto(ahora - tel.vitals.recibido)}</span>}
    </div>
  )
}

function AvisoConexion({ tel }: { tel: Telemetria }) {
  if (tel.conexion !== "error" && !(tel.conexion === "reconectando" && tel.detalleConexion)) return null
  return (
    <div className={`aviso ${tel.conexion === "error" ? "aviso-error" : ""}`}>
      <strong>{tel.conexion === "error" ? "No se pudo conectar con AWS IoT." : "Reconectando con AWS IoT…"}</strong>{" "}
      {tel.detalleConexion}
      {tel.identityId && (
        <>
          {" "}Identidad de Cognito: <code>{tel.identityId}</code>
        </>
      )}
    </div>
  )
}
