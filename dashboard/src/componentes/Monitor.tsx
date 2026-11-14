import { useMemo, useState, type ReactNode } from "react"
import { Responsive, useContainerWidth } from "react-grid-layout"
import "react-grid-layout/css/styles.css"
import { config } from "../config"
import {
  ALTO_FILA,
  borrarDiseno,
  cargarDiseno,
  claveDiseno,
  COLUMNAS,
  disenoInicial,
  guardarDiseno,
  MARGEN,
  NOMBRES_PANEL,
  PUNTOS,
  type IdPanel,
  type Punto,
} from "../diseno/diseno"
import type { Telemetria } from "../telemetria/useTelemetria"
import { useTelemetria } from "../telemetria/useTelemetria"
import { haceCuanto, useAhora } from "../utilidades/tiempo"
import { AlertaCaida } from "./AlertaCaida"
import { GraficaLinea, type Serie } from "./GraficaLinea"
import { HistorialPredicciones } from "./HistorialPredicciones"
import { ListaEventos } from "./ListaEventos"
import { OndaPPG } from "./OndaPPG"
import { PanelCaptura } from "./PanelCaptura"
import { TarjetaVital } from "./TarjetasVitales"

const VENTANAS = [
  { id: 1, nombre: "1 min" },
  { id: 5, nombre: "5 min" },
  { id: 10, nombre: "10 min" },
] as const

const PANELES: IdPanel[] = ["estado", "fc", "spo2", "sensor", "tendencias", "eventos", "ppg", "captura", "historial"]

interface Props {
  thing: string
  idToken?: string
  /** Identifica al usuario para guardar su diseño (p. ej. su correo). */
  usuario?: string
}

/** Monta con `key={thing}` para reiniciar todo al cambiar de pulsera. */
export function Monitor({ thing, idToken, usuario }: Props) {
  const tel = useTelemetria(thing, idToken)
  const ahora = useAhora(1000)
  const [minutos, setMinutos] = useState<(typeof VENTANAS)[number]["id"]>(5)

  // ---- Cuadrícula editable ----
  const visibles = useMemo(() => PANELES.filter(id => id !== "captura" || config.permitirComandos), [])
  const clave = claveDiseno(usuario)
  const [disenos, setDisenos] = useState(() => cargarDiseno(clave, visibles))
  // Al restablecer se vuelve a montar la cuadrícula para que tome el diseño inicial.
  const [version, setVersion] = useState(0)
  const [punto, setPunto] = useState<Punto>("lg")
  const [editando, setEditando] = useState(false)
  const { width, containerRef, mounted } = useContainerWidth()
  // En móvil (1 columna) el diseño es fijo.
  const puedeEditar = punto !== "sm"
  const editable = editando && puedeEditar

  // ---- Series de las gráficas ----
  const horizonte = (tel.pred?.horizonte_s ?? 10) * 1000
  const desde = ahora - minutos * 60_000
  const hasta = ahora + horizonte

  const seriesFc = useMemo<Serie[]>(
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

  const serieSpo2 = useMemo<Serie[]>(
    () => [{ id: "spo2", nombre: "SpO₂", color: "var(--serie-1)", puntos: tel.historialVitals.map(v => ({ t: v.t, v: v.spo2 })) }],
    [tel.historialVitals],
  )

  const contenido = (id: IdPanel): ReactNode => {
    switch (id) {
      case "estado":
      case "fc":
      case "spo2":
      case "sensor":
        return <TarjetaVital tipo={id} vitals={tel.vitals} pred={tel.pred} ahora={ahora} />

      case "tendencias":
        return (
          <section className="panel panel-lleno" aria-labelledby="titulo-tendencias">
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
              peso={3}
              decimales={0}
            />
            <GraficaLinea titulo="SpO₂" unidad="%" series={serieSpo2} desde={desde} hasta={hasta} yMin={90} yMax={100} peso={2} />
          </section>
        )

      case "eventos":
        return <ListaEventos eventos={tel.eventos} />

      case "ppg":
        return (
          <section className="panel panel-lleno" aria-labelledby="titulo-ppg">
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
        )

      case "captura":
        return <PanelCaptura conectado={tel.conexion === "conectado"} publicar={tel.publicar} ahora={ahora} />

      case "historial":
        return <HistorialPredicciones thing={thing} idToken={idToken} />
    }
  }

  return (
    <>
      <BarraEstado
        tel={tel}
        ahora={ahora}
        editando={editable}
        puedeEditar={puedeEditar}
        alEditar={() => setEditando(e => !e)}
        alRestablecer={() => {
          borrarDiseno(clave)
          setDisenos(disenoInicial(visibles))
          setVersion(v => v + 1)
        }}
      />
      <AvisoConexion tel={tel} />
      <AlertaCaida inicio={tel.inicioCaida} />
      {editable && (
        <p className="aviso-edicion" role="status">
          Arrastra un panel para moverlo y usa sus bordes o la esquina inferior derecha para cambiar su tamaño. Los
          cambios se guardan en este navegador. Pulsa <strong>Listo</strong> al terminar o{" "}
          <strong>Restablecer diseño</strong> para volver al original.
        </p>
      )}

      <div ref={containerRef} className={`cuadricula-paneles ${editable ? "editando" : ""}`}>
        {mounted && (
          <Responsive
            key={version}
            width={width}
            layouts={disenos}
            breakpoints={PUNTOS}
            cols={COLUMNAS}
            rowHeight={ALTO_FILA}
            margin={MARGEN}
            containerPadding={[0, 0]}
            dragConfig={{ enabled: editable }}
            resizeConfig={{ enabled: editable, handles: ["se", "e", "s"] }}
            onLayoutChange={(_, todos) => {
              const nuevos = todos as typeof disenos
              setDisenos(nuevos)
              // Solo se guarda lo que el usuario cambia en modo edición.
              if (editable) guardarDiseno(clave, nuevos)
            }}
            onBreakpointChange={nuevo => setPunto(nuevo as Punto)}
          >
            {visibles.map(id => (
              <div key={id} className="celda" aria-label={NOMBRES_PANEL[id]}>
                {editable && (
                  <span className="celda-etiqueta" aria-hidden="true">
                    ⠿ {NOMBRES_PANEL[id]}
                  </span>
                )}
                <div className="celda-contenido">{contenido(id)}</div>
              </div>
            ))}
          </Responsive>
        )}
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

interface PropsBarra {
  tel: Telemetria
  ahora: number
  editando: boolean
  puedeEditar: boolean
  alEditar: () => void
  alRestablecer: () => void
}

function BarraEstado({ tel, ahora, editando, puedeEditar, alEditar, alRestablecer }: PropsBarra) {
  const claseConexion =
    tel.conexion === "conectado" ? "estado-bien" : tel.conexion === "error" ? "estado-critico" : "estado-aviso"
  const clasePulsera = tel.online === null ? "estado-neutro" : tel.online ? "estado-bien" : "estado-critico"
  return (
    <div className="barra-estado">
      <span className={`insignia ${claseConexion}`} role="status">
        <span className="punto" aria-hidden="true" />
        {config.demo ? "Datos simulados" : TEXTO_CONEXION[tel.conexion]}
      </span>
      <span className={`insignia ${clasePulsera}`}>
        <span className="punto" aria-hidden="true" />
        Pulsera {tel.online === null ? "sin estado" : tel.online ? "en línea" : "desconectada"}
      </span>
      {tel.vitals && <span className="barra-texto">Último dato {haceCuanto(ahora - tel.vitals.recibido)}</span>}
      {puedeEditar && (
        <span className="barra-acciones">
          {editando && (
            <button type="button" className="boton barra-accion" onClick={alRestablecer}>
              Restablecer diseño
            </button>
          )}
          <button
            type="button"
            className={`boton barra-accion ${editando ? "boton-primario" : ""}`}
            aria-pressed={editando}
            onClick={alEditar}
          >
            {editando ? "Listo" : "Editar diseño"}
          </button>
        </span>
      )}
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
