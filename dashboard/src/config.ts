// Configuración leída de las variables VITE_* (se fijan al compilar).

const env = import.meta.env

const lista = (valor?: string) =>
  (valor ?? "")
    .split(",")
    .map(x => x.trim())
    .filter(Boolean)

const demo = env.VITE_DEMO === "true"
const things = lista(env.VITE_THING_NAMES)

export const config = {
  /** Datos simulados en el navegador; no se conecta a AWS IoT. */
  demo,
  /** Issuer OIDC del User Pool: https://cognito-idp.<region>.amazonaws.com/<userPoolId> */
  authority: env.VITE_AUTHORITY ?? "",
  region: env.VITE_AWS_REGION || "us-east-2",
  identityPoolId: env.VITE_IDENTITY_POOL_ID ?? "",
  /** Host `<prefijo>-ats.iot.<region>.amazonaws.com` (sin protocolo ni ruta). */
  iotEndpoint: (env.VITE_IOT_ENDPOINT ?? "").replace(/^\w+:\/\//, "").replace(/\/.*$/, ""),
  things: things.length > 0 ? things : demo ? ["pulsera-demo"] : [],
  /** API (API Gateway → DynamoDB) con el historial de predicciones. Opcional. */
  historialApiUrl: (env.VITE_HISTORY_API_URL ?? "").replace(/\/$/, ""),
  /** Muestra los botones start/stop (publica en `bio/<thing>/cmd`). */
  permitirComandos: demo || env.VITE_ENABLE_CMD === "true",
}

export const authHabilitado = Boolean(config.authority)

/** Variables que faltan para conectarse a AWS IoT (vacío = todo listo). */
export function faltantesIot(): string[] {
  if (config.demo) return []
  const faltan: string[] = []
  if (!config.authority) faltan.push("VITE_AUTHORITY")
  if (!config.identityPoolId) faltan.push("VITE_IDENTITY_POOL_ID")
  if (!config.iotEndpoint) faltan.push("VITE_IOT_ENDPOINT")
  if (config.things.length === 0) faltan.push("VITE_THING_NAMES")
  return faltan
}
