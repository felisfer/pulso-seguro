"""
features.py · Extractor de features de PulsoSeguro (implementación de REFERENCIA).

Este archivo y `firmware/pulso_seguro_aws/features_fc.h` implementan EXACTAMENTE
el mismo algoritmo. Si cambias uno, cambia el otro y corre:

    python herramientas/probar_paridad.py dataset.csv

Por qué existe: en producción el ESP32 calcula las features y la Lambda solo
predice. Si el modelo se entrenara con features calculadas de otra forma
(p. ej. scipy.find_peaks + filtfilt con relleno), el modelo vería en producción
datos distintos a los del entrenamiento (training/serving skew). Por eso aquí
NO se usa scipy: todo está escrito a mano con operaciones fáciles de portar a C.

Algoritmo (ventana de 8 s = 800 muestras a 100 Hz, se emite cada 2 s = 200 muestras):
  1. IR y RED: se resta la media (DC) y se filtra con un Butterworth pasabanda
     0.5-4 Hz de orden 2 (2 biquads), hacia adelante y hacia atrás, estado inicial 0.
  2. fc_fft: DFT directa (ventana de Hann, N=1024 con ceros) en los bins de
     0.5-4 Hz; pico + interpolación parabólica.
  3. Picos: máximos locales > 0, lejos de los bordes (0.5 s), elegidos de mayor
     a menor altura con distancia mínima = 60 % del periodo de fc_fft.
  4. fc = 60 / mediana(IBI); cv_ibi = std/media de los IBI.
  5. Ventana válida si: dedo puesto, >= 3 IBI, cv_ibi < 0.15, |fc - fc_fft| <= 10.
  6. Lags: valores de las 3 ventanas anteriores; si una no existe o no fue
     válida, se usa el valor actual (así el lag "no aporta" en vez de inventar).
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

# ---------------------------------------------------------------- constantes
FS = 100                 # Hz
VENTANA = 800            # 8 s
PASO = 200               # 2 s
N_DFT = 1024
MARGEN = 50              # 0.5 s sin picos en cada borde (transitorio del filtro)
UMBRAL_DEDO = 50_000
CV_MAX = 0.15
TOL_FFT = 10.0           # lpm
F_MIN, F_MAX = 0.5, 4.0  # Hz
VERSION_FEATURES = 1

# Butterworth pasabanda 0.5-4 Hz, orden 2, fs=100: scipy.signal.butter(2, [0.5, 4], 'band', fs=100, output='sos')
SOS = (
    (0.01043241337109341, 0.02086482674218683, 0.01043241337109341, 1.0, -1.717844586580564, 0.7634885625932664),
    (1.0, -2.0, 1.0, 1.0, -1.9585309426687274, 0.9597079331261205),
)

NOMBRES = [
    "fc", "fc_fft", "cv_ibi", "perfusion", "razon_r",
    "acc_media", "acc_std", "acc_frec", "gyro_std",
    "fc_lag1", "fc_lag2", "fc_lag3",
    "acc_std_lag1", "acc_std_lag2", "acc_std_lag3",
    "tendencia",
]
K_MIN = int(math.ceil(F_MIN * N_DFT / FS))   # 6
K_MAX = int(math.floor(F_MAX * N_DFT / FS))  # 40
HANN = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(VENTANA) / (VENTANA - 1))


# ---------------------------------------------------------------- bloques
def _biquad(x: np.ndarray, s) -> np.ndarray:
    """Forma directa II transpuesta, estado inicial 0 (igual que en C)."""
    b0, b1, b2, _, a1, a2 = s
    y = np.empty_like(x)
    z1 = z2 = 0.0
    for i, xi in enumerate(x):
        yi = b0 * xi + z1
        z1 = b1 * xi - a1 * yi + z2
        z2 = b2 * xi - a2 * yi
        y[i] = yi
    return y


def pasabanda(x: np.ndarray) -> np.ndarray:
    y = np.asarray(x, dtype=np.float64)
    for s in SOS:
        y = _biquad(y, s)
    y = y[::-1]
    for s in SOS:
        y = _biquad(y, s)
    return y[::-1].copy()


def frec_dominante(x: np.ndarray) -> float:
    """Frecuencia (Hz) del pico del espectro en 0.5-4 Hz. Devuelve 0 si no hay energía."""
    xw = (x - x.mean()) * HANN
    n = np.arange(VENTANA)
    mags = []
    for k in range(K_MIN, K_MAX + 1):           # DFT directa: idéntica a la del ESP32
        ang = 2 * np.pi * k * n / N_DFT
        re = np.dot(xw, np.cos(ang))
        im = -np.dot(xw, np.sin(ang))
        mags.append(re * re + im * im)
    mags = np.array(mags)
    i = int(np.argmax(mags))
    if mags[i] <= 0:
        return 0.0
    delta = 0.0
    if 0 < i < len(mags) - 1:                   # interpolación parabólica
        a, b, c = mags[i - 1], mags[i], mags[i + 1]
        den = a - 2 * b + c
        if den != 0:
            delta = 0.5 * (a - c) / den
    return (K_MIN + i + delta) * FS / N_DFT


def detectar_picos(y: np.ndarray, dmin: int) -> list[int]:
    cand = [i for i in range(MARGEN, VENTANA - MARGEN)
            if y[i] > 0 and y[i] > y[i - 1] and y[i] >= y[i + 1]]
    cand.sort(key=lambda i: (-y[i], i))          # mayor altura primero; empate → índice menor
    elegidos: list[int] = []
    for i in cand:
        if all(abs(i - j) >= dmin for j in elegidos):
            elegidos.append(i)
    return sorted(elegidos)


def _std(x) -> float:
    x = np.asarray(x, dtype=np.float64)
    return float(np.sqrt(np.mean((x - x.mean()) ** 2)))


@dataclass
class Ventana:
    """Features 'propias' de una ventana (sin lags)."""
    valida: bool
    fc: float
    fc_fft: float
    cv_ibi: float
    perfusion: float
    razon_r: float
    acc_media: float
    acc_std: float
    acc_frec: float
    gyro_std: float
    n_ibi: int


def features_ventana(red, ir, amag, gmag) -> Ventana:
    red = np.asarray(red, np.float64); ir = np.asarray(ir, np.float64)
    amag = np.asarray(amag, np.float64); gmag = np.asarray(gmag, np.float64)
    dc_ir, dc_red = ir.mean(), red.mean()
    dedo = dc_ir > UMBRAL_DEDO

    f_ir = pasabanda(ir - dc_ir)
    f_red = pasabanda(red - dc_red)
    fc_fft = 60.0 * frec_dominante(f_ir)

    fc = cv = 0.0
    n_ibi = 0
    if fc_fft > 0:
        dmin = max(1, int(round(0.6 * FS * 60.0 / fc_fft)))
        picos = detectar_picos(f_ir, dmin)
        ibi = np.diff(picos) / FS
        n_ibi = len(ibi)
        if n_ibi >= 1:
            fc = 60.0 / float(np.median(ibi))
            cv = _std(ibi) / float(ibi.mean())

    s_ir, s_red = _std(f_ir), _std(f_red)
    perf = 100.0 * s_ir / dc_ir if dc_ir > 0 else 0.0
    r = (s_red / dc_red) / (s_ir / dc_ir) if (dc_red > 0 and dc_ir > 0 and s_ir > 0) else 0.0

    valida = bool(dedo and n_ibi >= 3 and cv < CV_MAX and abs(fc - fc_fft) <= TOL_FFT)
    return Ventana(valida, fc, fc_fft, cv, perf, r,
                   float(amag.mean()), _std(amag), frec_dominante(amag), _std(gmag), n_ibi)


# ---------------------------------------------------------------- streaming
@dataclass
class Extractor:
    """Recibe muestras una a una (como el ESP32) y emite una fila cada 2 s."""
    red: list = field(default_factory=list)
    ir: list = field(default_factory=list)
    amag: list = field(default_factory=list)
    gmag: list = field(default_factory=list)
    nuevas: int = 0
    win: int = 0
    hist: list = field(default_factory=list)   # (valida, fc, acc_std) de ventanas previas
    ultimo_seq: int | None = None

    def reiniciar(self):
        self.red.clear(); self.ir.clear(); self.amag.clear(); self.gmag.clear()
        self.nuevas = 0
        self.hist.clear()
        self.ultimo_seq = None

    def push(self, seq, red, ir, ax, ay, az, gx, gy, gz):
        if self.ultimo_seq is not None and seq != self.ultimo_seq + 1:
            self.reiniciar()                    # hueco (lote perdido o dedo fuera): empezar de nuevo
        self.ultimo_seq = seq
        for buf, v in ((self.red, red), (self.ir, ir),
                       (self.amag, math.sqrt(ax * ax + ay * ay + az * az)),
                       (self.gmag, math.sqrt(gx * gx + gy * gy + gz * gz))):
            buf.append(float(v))
            if len(buf) > VENTANA:
                buf.pop(0)
        self.nuevas += 1
        if len(self.ir) == VENTANA and self.nuevas >= PASO:
            self.nuevas = 0
            return self._emitir()
        return None

    def _emitir(self) -> dict:
        v = features_ventana(self.red, self.ir, self.amag, self.gmag)
        fila = {"win": self.win, "seq": self.ultimo_seq, "valida": v.valida, **{k: getattr(v, k) for k in (
            "fc", "fc_fft", "cv_ibi", "perfusion", "razon_r",
            "acc_media", "acc_std", "acc_frec", "gyro_std")}}
        for k in (1, 2, 3):
            if len(self.hist) >= k and self.hist[-k][0]:
                fila[f"fc_lag{k}"] = self.hist[-k][1]
            else:
                fila[f"fc_lag{k}"] = v.fc
            fila[f"acc_std_lag{k}"] = self.hist[-k][2] if len(self.hist) >= k else v.acc_std
        fila["tendencia"] = v.fc - fila["fc_lag3"]
        self.hist.append((v.valida, v.fc, v.acc_std))
        if len(self.hist) > 3:
            self.hist.pop(0)
        self.win += 1
        return fila


def vector(fila: dict) -> list[float]:
    """Orden exacto de las features que espera el modelo (y que manda el ESP32)."""
    return [float(fila[n]) for n in NOMBRES]


def extraer_sesion(df) -> list[dict]:
    """Aplica el extractor a un DataFrame de UNA sesión ordenado por seq."""
    ex = Extractor()
    filas = []
    cols = ["seq", "red", "ir", "ax", "ay", "az", "gx", "gy", "gz"]
    for r in df[cols].itertuples(index=False):
        f = ex.push(*r)
        if f is not None:
            filas.append(f)
    return filas
