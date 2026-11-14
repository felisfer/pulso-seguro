import { WebStorageStateStore } from "oidc-client-ts"
import { StrictMode } from "react"
import { createRoot } from "react-dom/client"
import { AuthProvider } from "react-oidc-context"
import App from "./App.tsx"
import { authHabilitado } from "./config"
import "./index.css"

const cognitoAuthConfig = {
  authority: import.meta.env.VITE_AUTHORITY ?? "",
  client_id: import.meta.env.VITE_CLIENT_ID ?? "",
  redirect_uri: import.meta.env.VITE_REDIRECT_URI ?? window.location.origin,
  response_type: "code",
  scope: "email openid phone",
  automaticSilentRenew: true,
  userStore: new WebStorageStateStore({ store: window.localStorage }),
  onSigninCallback: () => {
    window.history.replaceState({}, document.title, window.location.pathname)
  },
}

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    {authHabilitado ? (
      <AuthProvider {...cognitoAuthConfig}>
        <App />
      </AuthProvider>
    ) : (
      <App />
    )}
  </StrictMode>,
)
