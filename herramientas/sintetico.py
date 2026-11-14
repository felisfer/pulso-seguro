"""
sintetico.py · Genera un dataset.csv sintético con el MISMO formato que cargar_s3.py,
con transiciones reposo → caminar → esfuerzo → reposo, para probar el pipeline
(features, entrenamiento, paridad C/Python y Lambda) sin la pulsera.

    python herramientas/sintetico.py --sesiones 8 --duracion 240 -o dataset_sintetico.csv
"""
import argparse
import sys

import numpy as np
import pandas as pd

for _s in (sys.stdout, sys.stderr):          # Windows: cp1252 en pipes no tiene "→", "✅"…
    _s.reconfigure(encoding="utf-8", errors="replace")

FS = 100


def sesion(rng, nombre, sujeto, duracion):
    n = duracion * FS
    t = np.arange(n) / FS
    # Perfil de actividad por tramos (0 reposo, 1 caminar, 2 esfuerzo)
    cortes = np.sort(rng.uniform(0.15, 0.85, 3)) * duracion
    act = np.select([t < cortes[0], t < cortes[1], t < cortes[2]], [0, 1, 2], 0)
    base = rng.uniform(62, 80)
    objetivo = np.array([base, base + rng.uniform(12, 25), base + rng.uniform(35, 60)])[act]
    # La FC sigue al objetivo con una constante de tiempo de ~20 s (subidas y bajadas suaves)
    fc = np.empty(n); fc[0] = base
    a = 1 / (20 * FS)
    for i in range(1, n):
        fc[i] = fc[i - 1] + a * (objetivo[i] - fc[i - 1])
    fc += 1.5 * np.sin(2 * np.pi * 0.25 * t)          # arritmia sinusal respiratoria
    fase = 2 * np.pi * np.cumsum(fc / 60) / FS
    pulso = np.sin(fase) + 0.35 * np.sin(2 * fase - 0.8) + 0.12 * np.sin(3 * fase - 1.6)

    cad = np.array([0.0, 1.8, 2.6])[act]              # cadencia del brazo (Hz)
    amp = np.array([0.05, 1.5, 3.0])[act]
    fase_c = 2 * np.pi * np.cumsum(cad) / FS
    mov = amp * np.sin(fase_c)

    ir = 118_000 + 600 * pulso + 150 * mov + rng.normal(0, 25, n) + 300 * np.sin(2 * np.pi * 0.05 * t)
    red = 92_000 + 380 * pulso + 120 * mov + rng.normal(0, 25, n) + 250 * np.sin(2 * np.pi * 0.05 * t)
    ax = mov + rng.normal(0, 0.05, n)
    ay = 0.6 * mov + rng.normal(0, 0.05, n)
    az = 9.81 + 0.3 * mov + rng.normal(0, 0.05, n)
    g = 0.4 * amp * np.cos(fase_c)
    gx, gy, gz = g + rng.normal(0, 0.01, n), 0.5 * g + rng.normal(0, 0.01, n), rng.normal(0, 0.01, n)
    return pd.DataFrame(dict(
        sesion=nombre, label="transicion", sujeto=sujeto, seq=np.arange(n), t_ms=(t * 1000).astype(int),
        red=red.round().astype(int), ir=ir.round().astype(int),
        ax=ax.round(4), ay=ay.round(4), az=az.round(4), gx=gx.round(4), gy=gy.round(4), gz=gz.round(4)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sesiones", type=int, default=8)
    ap.add_argument("--duracion", type=int, default=240, help="segundos por sesión")
    ap.add_argument("--semilla", type=int, default=7)
    ap.add_argument("-o", "--salida", default="dataset_sintetico.csv")
    a = ap.parse_args()
    rng = np.random.default_rng(a.semilla)
    df = pd.concat([sesion(rng, f"sim{i:02d}", f"s{i % 3}", a.duracion) for i in range(a.sesiones)])
    df.to_csv(a.salida, index=False)
    print(f"{a.salida}: {len(df):,} filas, {a.sesiones} sesiones")


if __name__ == "__main__":
    main()
