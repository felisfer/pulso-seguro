// Contratos de los tópicos MQTT `bio/<THING_NAME>/…` (ver docs de tópicos del dashboard).

export type Status = "online" | "offline"

export type Semaforo = "verde" | "amarillo" | "rojo"

/** `bio/<thing>/vitals` · JSON a 1 Hz, publicado por el ESP32. */
export interface Vitals {
  /** Epoch en ms (NTP en el ESP32). */
  ts: number
  /** lpm; `null` si no hay dedo o la lectura no es válida. */
  fc: number | null
  /** %; `null` si no es válida. */
  spo2: number | null
  /** IR > 50 000. */
  dedo: boolean
  /** Caída detectada por el MPU6050. */
  caida: boolean
  /** Lo decide el ESP32; el dashboard no recalcula umbrales. */
  semaforo: Semaforo
}

/** `bio/<thing>/pred` · JSON cada 2 s, publicado por la Lambda. */
export interface Prediccion {
  ts: number
  fc_pred: number
  horizonte_s: number
  modelo: string
}

/** `bio/<thing>/cmd` · JSON publicado por el dashboard (opcional). */
export type Cmd =
  | { cmd: "start"; sesion: string; label: string; sujeto: string }
  | { cmd: "stop" }

/** Una fila de `bio/<thing>/data` (solo las columnas que se grafican). */
export interface MuestraPPG {
  seq: number
  t_ms: number
  red: number
  ir: number
}

export interface LotePPG {
  sesion: string
  label: string
  sujeto: string
  muestras: MuestraPPG[]
}
