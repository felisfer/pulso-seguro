import { useAuth } from "react-oidc-context"
import './App.css'

function App() {
  const auth = useAuth()

  const signOutRedirect = () => {
    const clientId = import.meta.env.VITE_CLIENT_ID
    const logoutUri = import.meta.env.VITE_LOGOUT_URI
    const cognitoDomain = import.meta.env.VITE_COGNITO_DOMAIN
    window.location.href = `${cognitoDomain}/logout?client_id=${clientId}&logout_uri=${encodeURIComponent(logoutUri)}`
  }

  const signOut = async () => {
    await auth.removeUser()
    signOutRedirect()
  }

  if (auth.isLoading) {
    return <div>Loading...</div>
  }

  if (auth.error) {
    return <div>Encountering error... {auth.error.message}</div>
  }

  if (auth.isAuthenticated) {
    return (
      <div>
        <pre> Hello: {auth.user?.profile.email} </pre>
        <pre> ID Token: {auth.user?.id_token} </pre>
        <pre> Access Token: {auth.user?.access_token} </pre>
        <pre> Refresh Token: {auth.user?.refresh_token} </pre>

        <button onClick={() => auth.removeUser()}>Sign out</button>
      </div>
    )
  }

  return (
    <div>
      <button onClick={() => auth.signinRedirect()}>Sign in</button>
      <button onClick={() => signOut()}>Sign out</button>
    </div>
  )
}

export default App
