import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  server: {
    allowedHosts: process.env?.ALLOWED_HOSTS?.split(',').map(host => host.trim()),
    port: parseInt(process.env.PORT || '5173')
  }
})
