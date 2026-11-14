// Íconos SVG en línea (sin dependencias). Heredan `currentColor`.

interface Props {
  tam?: number
}

const base = (tam: number) => ({
  width: tam,
  height: tam,
  viewBox: "0 0 24 24",
  fill: "none",
  stroke: "currentColor",
  strokeWidth: 2,
  strokeLinecap: "round" as const,
  strokeLinejoin: "round" as const,
  "aria-hidden": true,
})

export const IconoCorazon = ({ tam = 20 }: Props) => (
  <svg {...base(tam)}>
    <path d="M19.5 12.6 12 20l-7.5-7.4A5 5 0 1 1 12 6a5 5 0 1 1 7.5 6.6Z" />
    <path d="M3.5 12h4l1.5-3 3 6 1.5-3h7" />
  </svg>
)

export const IconoGota = ({ tam = 20 }: Props) => (
  <svg {...base(tam)}>
    <path d="M12 3s6 6.5 6 11a6 6 0 0 1-12 0c0-4.5 6-11 6-11Z" />
  </svg>
)

export const IconoMano = ({ tam = 20 }: Props) => (
  <svg {...base(tam)}>
    <path d="M7 11V5.5a1.5 1.5 0 0 1 3 0V10m0-1.5v-4a1.5 1.5 0 0 1 3 0V10m0-3.5a1.5 1.5 0 0 1 3 0V11m0-2.5a1.5 1.5 0 0 1 3 0V15a6 6 0 0 1-6 6h-1a6 6 0 0 1-5-2.7L4.3 14a1.5 1.5 0 0 1 2.5-1.7L7 13" />
  </svg>
)

export const IconoAlerta = ({ tam = 20 }: Props) => (
  <svg {...base(tam)}>
    <path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z" />
    <path d="M12 9v4M12 17h.01" />
  </svg>
)

export const IconoOk = ({ tam = 20 }: Props) => (
  <svg {...base(tam)}>
    <circle cx="12" cy="12" r="9" />
    <path d="m8 12 3 3 5-6" />
  </svg>
)

export const IconoInfo = ({ tam = 20 }: Props) => (
  <svg {...base(tam)}>
    <circle cx="12" cy="12" r="9" />
    <path d="M12 16v-5M12 8h.01" />
  </svg>
)

export const IconoCaida = ({ tam = 20 }: Props) => (
  <svg {...base(tam)}>
    <circle cx="16" cy="4.5" r="2" />
    <path d="m9 21 2.5-5.5L15 17l3 4M6 11l4-3 4 1.5 2 3.5M3 21h18" />
  </svg>
)

export const IconoSalir = ({ tam = 18 }: Props) => (
  <svg {...base(tam)}>
    <path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4M16 17l5-5-5-5M21 12H9" />
  </svg>
)
