import { useEffect, useState } from "react"

/**
 * Tamaño en px (ancho y alto) de un elemento, actualizado con ResizeObserver.
 * Devuelve un callback ref, así funciona aunque el elemento se desmonte y vuelva a montar.
 */
export function useTamano<T extends HTMLElement>() {
  const [elemento, setElemento] = useState<T | null>(null)
  const [tamano, setTamano] = useState({ ancho: 0, alto: 0 })
  useEffect(() => {
    if (!elemento) return
    const observador = new ResizeObserver(([entrada]) => {
      const ancho = Math.floor(entrada.contentRect.width)
      const alto = Math.floor(entrada.contentRect.height)
      setTamano(t => (t.ancho === ancho && t.alto === alto ? t : { ancho, alto }))
    })
    observador.observe(elemento)
    return () => observador.disconnect()
  }, [elemento])
  return [setElemento, tamano] as const
}
