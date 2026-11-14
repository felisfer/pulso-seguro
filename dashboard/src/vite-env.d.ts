/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_AUTHORITY?: string
  readonly VITE_CLIENT_ID?: string
  readonly VITE_REDIRECT_URI?: string
  readonly VITE_LOGOUT_URI?: string
  readonly VITE_COGNITO_DOMAIN?: string
  readonly VITE_AWS_REGION?: string
  readonly VITE_IDENTITY_POOL_ID?: string
  readonly VITE_IOT_ENDPOINT?: string
  readonly VITE_THING_NAMES?: string
  readonly VITE_HISTORY_API_URL?: string
  readonly VITE_ENABLE_CMD?: string
  readonly VITE_DEMO?: string
}

interface ImportMeta {
  readonly env: ImportMetaEnv
}
