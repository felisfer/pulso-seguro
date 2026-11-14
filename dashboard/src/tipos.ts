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
  /** Caída detectada por el MPU6050 (en confirmación o con la alarma activa). */
  caida: boolean
  /** Alarma por el botón de pánico (pulsación de 3 s) activa. */
  panico: boolean
  /** Lo decide el ESP32; el dashboard no recalcula umbrales. */
  semaforo: Semaforo
}

/**
 * `bio/<thing>/pred` · JSON cada 2 s, publicado por la Lambda (lambda/src/lambda_function.py).
 * El mismo mensaje lo lee el ESP32 (`win`, `fc_prevista`, `semaforo`).
 */
export interface Prediccion {
  /** `timestamp()` de la IoT Rule en ms (cuándo llegaron las features). */
  ts: number
  /** FC prevista a `horizonte_s` (igual a `fc_prevista`). */
  fc_pred: number
  horizonte_s: number
  /** Versión del modelo. */
  modelo: string
  /** FC de la ventana con la que se predijo. */
  fc?: number
  /** Solo por FC prevista: "amarillo" si pasa de 120, si no "verde". */
  semaforo?: Semaforo
  /** "ridge" o "media3" (línea base). */
  usado?: string
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
