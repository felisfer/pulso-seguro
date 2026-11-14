import { useAuth } from "react-oidc-context"
import "./App.css"
import { IconoCorazon } from "./componentes/Iconos"
import { Tablero } from "./componentes/Tablero"
import { authHabilitado, config } from "./config"

export default function App() {
  if (authHabilitado) return <AppConLogin />
  if (config.demo) return <Tablero usuario="Demo mode" />
  return (
    <PantallaCentrada>
      <h1>Missing configuration</h1>
      <p>
        <code>VITE_AUTHORITY</code> was not found. In <code>{import.meta.env.MODE}</code> mode Vite reads{" "}
        <code>{import.meta.env.DEV ? ".env" : ".env.production"}</code>: add the variables from{" "}
        <code>.env.example</code> there and restart, or use <code>VITE_DEMO=true</code> to see the interface with simulated data.
      </p>
    </PantallaCentrada>
  )
}

function AppConLogin() {
  const auth = useAuth()

  // Cierra la sesión local y luego la de Cognito (managed login).
  // IMPORTANTE: `logout_uri` debe coincidir EXACTAMENTE (incluida la "/" final) con una de las
  // "Allowed sign-out URLs" del App client; si no, Cognito redirige a /login y muestra "Invalid request".
  const salir = async () => {
    await auth.removeUser()
    const clientId = import.meta.env.VITE_CLIENT_ID
    const dominio = import.meta.env.VITE_COGNITO_DOMAIN?.trim().replace(/\/+$/, "")
    const logoutUri = import.meta.env.VITE_LOGOUT_URI?.trim() || `${window.location.origin}/`
    if (!dominio || !clientId) return // sin dominio: basta con la sesión local (vuelve a la pantalla de login)
    window.location.assign(`${dominio}/logout?client_id=${encodeURIComponent(clientId)}&logout_uri=${encodeURIComponent(logoutUri)}`)
  }

  if (auth.isLoading) {
    return (
      <PantallaCentrada>
        <p>Loading session…</p>
      </PantallaCentrada>
    )
  }

  if (auth.error) {
    return (
      <PantallaCentrada>
        <h1>Sign-in failed</h1>
        <p className="error-linea">{auth.error.message}</p>
        <button type="button" className="boton boton-primario" onClick={() => void auth.signinRedirect()}>
          Try again
        </button>
      </PantallaCentrada>
    )
  }

  if (!auth.isAuthenticated) {
    return (
      <PantallaCentrada>
        <h1>HealthyBeats</h1>
        <p>Live monitoring of heart rate, SpO₂ and falls.</p>
        <button type="button" className="boton boton-primario" onClick={() => void auth.signinRedirect()}>
          Sign in
        </button>
      </PantallaCentrada>
    )
  }

  return <Tablero usuario={auth.user?.profile.email} idToken={auth.user?.id_token} onSalir={() => void salir()} />
}

function PantallaCentrada({ children }: { children: React.ReactNode }) {
  return (
    <main className="pantalla-centrada">
      <div className="tarjeta tarjeta-login">
        <span className="marca-logo marca-logo-grande" aria-hidden="true">
          <IconoCorazon tam={28} />
        </span>
        {children}
      </div>
    </main>
  )
}
