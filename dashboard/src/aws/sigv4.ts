// URL prefirmada (SigV4) para MQTT sobre WebSocket en AWS IoT Core.
// https://docs.aws.amazon.com/iot/latest/developerguide/protocols.html#mqtt-ws

import type { CredencialesAws } from "./cognito"

const codificador = new TextEncoder()

const hex = (buffer: ArrayBuffer) =>
  Array.from(new Uint8Array(buffer), b => b.toString(16).padStart(2, "0")).join("")

async function sha256(texto: string) {
  return hex(await crypto.subtle.digest("SHA-256", codificador.encode(texto)))
}

async function hmac(llave: BufferSource, mensaje: string) {
  const k = await crypto.subtle.importKey("raw", llave, { name: "HMAC", hash: "SHA-256" }, false, ["sign"])
  return crypto.subtle.sign("HMAC", k, codificador.encode(mensaje))
}

export async function urlIotPrefirmada(endpoint: string, region: string, cred: CredencialesAws) {
  const servicio = "iotdevicegateway"
  const amzDate = new Date().toISOString().replace(/[:-]|\.\d{3}/g, "") // 20261113T120000Z
  const fecha = amzDate.slice(0, 8)
  const alcance = `${fecha}/${region}/${servicio}/aws4_request`

  // Parámetros en orden alfabético (requisito de la petición canónica).
  const query = [
    ["X-Amz-Algorithm", "AWS4-HMAC-SHA256"],
    ["X-Amz-Credential", `${cred.accessKeyId}/${alcance}`],
    ["X-Amz-Date", amzDate],
    ["X-Amz-SignedHeaders", "host"],
  ]
    .map(([k, v]) => `${k}=${encodeURIComponent(v)}`)
    .join("&")

  const peticionCanonica = ["GET", "/mqtt", query, `host:${endpoint}`, "", "host", await sha256("")].join("\n")
  const textoAFirmar = ["AWS4-HMAC-SHA256", amzDate, alcance, await sha256(peticionCanonica)].join("\n")

  let llave = await hmac(codificador.encode(`AWS4${cred.secretKey}`), fecha)
  llave = await hmac(llave, region)
  llave = await hmac(llave, servicio)
  llave = await hmac(llave, "aws4_request")
  const firma = hex(await hmac(llave, textoAFirmar))

  // IoT Core espera el token de sesión fuera de la firma.
  return (
    `wss://${endpoint}/mqtt?${query}&X-Amz-Signature=${firma}` +
    `&X-Amz-Security-Token=${encodeURIComponent(cred.sessionToken)}`
  )
}
