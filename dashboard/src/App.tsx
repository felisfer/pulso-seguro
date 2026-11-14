import { useAuth } from "react-oidc-context"
import "./App.css"
import { IconoCorazon } from "./componentes/Iconos"
import { Tablero } from "./componentes/Tablero"
import { authHabilitado, config } from "./config"

export default function App() {
  if (authHabilitado) return <AppConLogin />
  if (config.demo) return <Tablero usuario="Modo demo" />
  return (
    <PantallaCentrada>
      <h1>Falta configuración</h1>
      <p>
        No se encontró <code>VITE_AUTHORITY</code>. En modo <code>{import.meta.env.MODE}</code> Vite lee{" "}
        <code>{import.meta.env.DEV ? ".env" : ".env.production"}</code>: agrega ahí las variables de{" "}
        <code>.env.example</code> y reinicia, o usa <code>VITE_DEMO=true</code> para ver la interfaz con datos simulados.
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
        <p>Cargando sesión…</p>
      </PantallaCentrada>
    )
  }

  if (auth.error) {
    return (
      <PantallaCentrada>
        <h1>No se pudo iniciar sesión</h1>
        <p className="error-linea">{auth.error.message}</p>
        <button type="button" className="boton boton-primario" onClick={() => void auth.signinRedirect()}>
          Volver a intentar
        </button>
      </PantallaCentrada>
    )
  }

  if (!auth.isAuthenticated) {
    return (
      <PantallaCentrada>
        <h1>PulsoSeguro</h1>
        <p>Monitoreo en vivo de frecuencia cardiaca, SpO₂ y caídas.</p>
        <button type="button" className="boton boton-primario" onClick={() => void auth.signinRedirect()}>
          Iniciar sesión
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
