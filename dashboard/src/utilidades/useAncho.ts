import { useEffect, useState } from "react"

/**
 * Ancho en px de un elemento, actualizado con ResizeObserver.
 * Devuelve un callback ref, así funciona aunque el elemento se desmonte y vuelva a montar.
 */
export function useAncho<T extends HTMLElement>() {
  const [elemento, setElemento] = useState<T | null>(null)
  const [ancho, setAncho] = useState(0)
  useEffect(() => {
    if (!elemento) return
    const observador = new ResizeObserver(([entrada]) => setAncho(Math.floor(entrada.contentRect.width)))
    observador.observe(elemento)
    return () => observador.disconnect()
  }, [elemento])
  return [setElemento, ancho] as const
}
