// Credenciales temporales de AWS a partir del id_token del User Pool,
// usando la API REST de Cognito Identity (GetId + GetCredentialsForIdentity).
// Ninguna de las dos llamadas requiere firma, así que no hace falta el AWS SDK.

import { config } from "../config"

export interface CredencialesAws {
  accessKeyId: string
  secretKey: string
  sessionToken: string
  /** Epoch en ms. */
  expiracion: number
  identityId: string
}

interface RespuestaGetId {
  IdentityId: string
}

interface RespuestaCredenciales {
  IdentityId: string
  Credentials: {
    AccessKeyId: string
    SecretKey: string
    SessionToken: string
    /** Epoch en segundos. */
    Expiration: number
  }
}

async function llamar<T>(accion: string, cuerpo: unknown): Promise<T> {
  const respuesta = await fetch(`https://cognito-identity.${config.region}.amazonaws.com/`, {
    method: "POST",
    headers: {
      "Content-Type": "application/x-amz-json-1.1",
      "X-Amz-Target": `AWSCognitoIdentityService.${accion}`,
    },
    body: JSON.stringify(cuerpo),
  })
  const json = (await respuesta.json().catch(() => ({}))) as Record<string, string>
  if (!respuesta.ok) {
    const tipo = (json.__type ?? String(respuesta.status)).split("#").pop()
    throw new Error(`Cognito ${accion}: ${tipo} ${json.message ?? json.Message ?? ""}`.trim())
  }
  return json as T
}

let identityIdCache: string | undefined

export async function obtenerCredenciales(idToken: string): Promise<CredencialesAws> {
  // El proveedor es el issuer del User Pool sin "https://".
  const proveedor = config.authority.replace(/^https?:\/\//, "").replace(/\/$/, "")
  const Logins = { [proveedor]: idToken }

  if (!identityIdCache) {
    const { IdentityId } = await llamar<RespuestaGetId>("GetId", {
      IdentityPoolId: config.identityPoolId,
      Logins,
    })
    identityIdCache = IdentityId
  }

  const { IdentityId, Credentials: c } = await llamar<RespuestaCredenciales>(
    "GetCredentialsForIdentity",
    { IdentityId: identityIdCache, Logins },
  )

  return {
    accessKeyId: c.AccessKeyId,
    secretKey: c.SecretKey,
    sessionToken: c.SessionToken,
    expiracion: c.Expiration * 1000,
    identityId: IdentityId,
  }
}
