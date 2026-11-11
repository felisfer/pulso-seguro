#!/usr/bin/env python3
"""
modelo_fc.py — Predicción de frecuencia cardiaca (FC) a corto plazo · PulsoSeguro

Uso:
  python modelo_fc.py dataset.csv                  # tus datos (salida de cargar_s3.py)
  python modelo_fc.py dataset.csv --horizonte 20   # predecir 20 s adelante (default 10 s)

Horizonte: con los datos hasta el instante t (ventana [t-8 s, t]) se predice la FC
que el dispositivo mostrará en t + H, es decir, la de la ventana [t+H-8 s, t+H].
  python modelo_fc.py --demo                       # prueba con datos sintéticos

Pipeline:
  1. Limpieza por sesión: orden por seq, huecos cortos interpolados, detección de dedo.
  2. FC de referencia en ventanas de 8 s cada 2 s: picos del IR filtrado (0.5-4 Hz),
     verificada contra el pico de la FFT. Las ventanas que no pasan el control de
     calidad (movimiento, sin dedo, latidos irregulares) quedan sin FC.
  3. Features de la ventana actual y de las anteriores: FC, tendencia, movimiento
     (acelerómetro y giroscopio), perfusión y razón rojo/IR.
  4. Modelo: HistGradientBoosting que predice el CAMBIO de FC dentro de H segundos.
     Se compara contra la persistencia ("la FC no cambia") y la media de las últimas
     3 ventanas. Validación cruzada agrupada por sesión (nunca filas al azar).
  5. Guarda en salida_fc/: modelo, predicciones, métricas y gráficas.

Ojo: la FC de referencia sale del mismo PPG, no de un equipo médico. Valídala al
menos una vez contra un oxímetro comercial o contando el pulso 30 s.
"""
import argparse
import json
from pathlib import Path

import joblib
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from scipy.signal import butter, filtfilt, find_peaks
from sklearn.ensemble import HistGradientBoostingRegressor
from sklearn.inspection import permutation_importance
from sklearn.model_selection import GroupKFold

FS = 100                 # Hz
VENTANA_S = 8            # duración de cada ventana
PASO_S = 2               # cada cuánto empieza una ventana
IR_DEDO = 50_000         # mismo umbral que el firmware
FC_MIN, FC_MAX = 40, 200
SATURACION = 262_000     # el ADC del MAX30102 es de 18 bits (máx. 262 143)
HUECO_MAX = 50           # muestras (0.5 s) que se interpolan; más largo = corte

FEATURES = [
    "fc", "fc_fft", "cv_ibi", "perfusion", "razon_r",
    "acc_media", "acc_std", "acc_frec", "gyro_std",
    "fc_lag1", "fc_lag2", "fc_lag3", "tendencia",
    "acc_std_lag1", "acc_std_lag2", "acc_std_lag3",
]
COLORES = {"reposo": "#16a34a", "caminar": "#d99a00", "correr": "#dc2626"}


# ============================================================================
# 1. Limpieza
# ============================================================================
def limpiar_sesion(g: pd.DataFrame) -> pd.DataFrame:
    """Ordena por seq, rellena huecos cortos y marca qué muestras son utilizables."""
    g = g.drop_duplicates("seq").sort_values("seq").set_index("seq")
    g = g.reindex(np.arange(g.index.min(), g.index.max() + 1))      # huecos -> NaN
    g["perdida"] = g["ir"].isna()                                    # antes de interpolar
    num = ["red", "ir", "ax", "ay", "az", "gx", "gy", "gz"]
    g[num] = g[num].interpolate(limit=HUECO_MAX, limit_area="inside")
    for c in ["sesion", "label", "sujeto"]:
        g[c] = g[c].ffill().bfill()
    g["valido"] = g[num].notna().all(axis=1) & (g["ir"] > IR_DEDO)
    g["t_s"] = (g.index - g.index.min()) / FS
    return g.reset_index()


def segmentos(mascara: np.ndarray):
    """Rangos [ini, fin) de muestras válidas consecutivas."""
    d = np.diff(np.r_[0, mascara.astype(int), 0])
    return list(zip(np.where(d == 1)[0], np.where(d == -1)[0]))


# ============================================================================
# 2. FC de referencia por ventana
# ============================================================================
_B, _A = butter(3, [0.5, 4.0], btype="band", fs=FS)


def fc_de_ventana(ir_f: np.ndarray) -> dict:
    """FC por picos (mediana de intervalos) y por FFT, con indicadores de calidad.
    ir_f: IR filtrado e invertido (cada latido es un máximo)."""
    n = 4096                                                     # zero padding: 0.0244 Hz
    esp = np.abs(np.fft.rfft(ir_f * np.hanning(len(ir_f)), n))
    f = np.fft.rfftfreq(n, 1 / FS)
    banda = (f >= FC_MIN / 60) & (f <= FC_MAX / 60)
    fc_fft = f[banda][np.argmax(esp[banda])] * 60

    # Picos robustos: la FFT da el periodo esperado y no se aceptan dos picos a menos
    # del 60 % de ese periodo (evita contar la muesca dicrota o el ruido como latido).
    distancia = max(int(0.6 * FS * 60 / fc_fft), int(FS * 60 / FC_MAX))
    picos, _ = find_peaks(ir_f, distance=distancia, prominence=0.3 * np.std(ir_f))
    ibi = np.diff(picos) / FS
    fc_picos = 60 / np.median(ibi) if len(ibi) >= 3 else np.nan
    cv = np.std(ibi) / np.mean(ibi) if len(ibi) >= 3 else np.nan

    if not np.isfinite(fc_picos):
        motivo = "pocos_latidos"           # < 4 picos en 8 s
    elif not FC_MIN <= fc_picos <= FC_MAX:
        motivo = "fuera_de_rango"
    elif cv >= 0.15:
        motivo = "irregular"               # intervalos muy distintos: latidos perdidos o picos extra
    elif abs(fc_picos - fc_fft) >= 10:
        motivo = "no_coincide_fft"         # picos y espectro no están de acuerdo
    else:
        motivo = "ok"
    return {"fc": fc_picos if motivo == "ok" else np.nan, "fc_picos": fc_picos, "fc_fft": fc_fft,
            "cv_ibi": cv, "n_picos": len(picos), "motivo": motivo, "picos": picos}


def frecuencia_dominante(x: np.ndarray) -> float:
    x = x - x.mean()
    esp = np.abs(np.fft.rfft(x, 2048))
    f = np.fft.rfftfreq(2048, 1 / FS)
    banda = (f >= 0.3) & (f <= 5)
    return f[banda][np.argmax(esp[banda])]


def ventanas_de_sesion(g: pd.DataFrame) -> pd.DataFrame:
    """Una fila por ventana de 8 s (paso 2 s). k = índice de la ventana en la sesión."""
    W, P = VENTANA_S * FS, PASO_S * FS
    filas = []
    ir_f = np.full(len(g), np.nan)
    rojo_f = np.full(len(g), np.nan)
    for a, b in segmentos(g["valido"].to_numpy()):
        if b - a > 30:                                            # filtfilt necesita margen
            ir_f[a:b] = -filtfilt(_B, _A, g["ir"].to_numpy()[a:b])
            rojo_f[a:b] = -filtfilt(_B, _A, g["red"].to_numpy()[a:b])

    acc = np.sqrt(g["ax"] ** 2 + g["ay"] ** 2 + g["az"] ** 2).to_numpy()
    gyr = np.sqrt(g["gx"] ** 2 + g["gy"] ** 2 + g["gz"] ** 2).to_numpy()
    for k, ini in enumerate(range(0, len(g) - W + 1, P)):
        sl = slice(ini, ini + W)
        # t_s = FINAL de la ventana: el instante en que un dispositivo ya tendría esa FC
        fila = {"k": k, "t_s": g["t_s"].iat[ini] + VENTANA_S,
                "label": g["label"].iat[ini + W // 2],
                "dedo": g["valido"].iloc[sl].mean(), "motivo": "sin_dedo_o_hueco",
                "ir_dc": g["ir"].iloc[sl].mean(),
                "saturado_%": 100 * (g["ir"].iloc[sl] >= SATURACION).mean()}
        a_w = acc[sl]
        if np.isfinite(a_w).all():
            fila.update(acc_media=a_w.mean(), acc_std=a_w.std(),
                        acc_frec=frecuencia_dominante(a_w), gyro_std=np.nanstd(gyr[sl]))
        if fila["dedo"] == 1.0 and np.isfinite(ir_f[sl]).all():
            r = fc_de_ventana(ir_f[sl])
            r.pop("picos")
            dc_ir, dc_rojo = g["ir"].iloc[sl].mean(), g["red"].iloc[sl].mean()
            ac_ir, ac_rojo = np.std(ir_f[sl]), np.std(rojo_f[sl])
            fila.update(r, perfusion=100 * ac_ir / dc_ir,
                        razon_r=(ac_rojo / dc_rojo) / (ac_ir / dc_ir))
        filas.append(fila)
    v = pd.DataFrame(filas)
    for c in ["fc", "fc_fft", "fc_picos", "cv_ibi", "perfusion", "razon_r",
              "acc_media", "acc_std", "acc_frec", "gyro_std"]:
        if c not in v:
            v[c] = np.nan
    return v


# ============================================================================
# 3. Features con historia + objetivo a H segundos
# ============================================================================
def agregar_historia(v: pd.DataFrame, pasos: int) -> pd.DataFrame:
    v = v.sort_values("k").set_index("k")
    for i in (1, 2, 3):
        v[f"fc_lag{i}"] = v["fc"].shift(i)
        v[f"acc_std_lag{i}"] = v["acc_std"].shift(i)
    v["tendencia"] = (v["fc"] - v["fc_lag2"]) / (2 * PASO_S)      # lpm por segundo
    v["fc_futura"] = v["fc"].shift(-pasos)
    v["media3"] = v[["fc", "fc_lag1", "fc_lag2"]].mean(axis=1)
    return v.reset_index()


def construir_dataset(df: pd.DataFrame, horizonte_s: int):
    pasos = horizonte_s // PASO_S
    todas, calidad = [], []
    for sesion, g in df.groupby("sesion", sort=False):
        g = limpiar_sesion(g)
        v = ventanas_de_sesion(g)
        v["sesion"], v["sujeto"] = sesion, g["sujeto"].iat[0]
        todas.append(agregar_historia(v, pasos))
        calidad.append({"sesion": sesion, "label": "/".join(g["label"].unique()),
                        "duracion_s": round(len(g) / FS), "perdidas_%": round(100 * g["perdida"].mean(), 2),
                        "con_dedo_%": round(100 * g["valido"].mean(), 1),
                        "ventanas": len(v), "con_fc_%": round(100 * v["fc"].notna().mean(), 1)})
    return pd.concat(todas, ignore_index=True), pd.DataFrame(calidad)


# ============================================================================
# 4. Entrenamiento y validación
# ============================================================================
def nuevo_modelo():
    return HistGradientBoostingRegressor(max_iter=300, learning_rate=0.05, max_leaf_nodes=15,
                                         min_samples_leaf=20, l2_regularization=1.0, random_state=0)


def metricas(y, p):
    e = p - y
    return {"MAE": float(np.mean(np.abs(e))), "RMSE": float(np.sqrt(np.mean(e ** 2)))}


def validar(d: pd.DataFrame):
    """Predicciones fuera de muestra: cada sesión se predice con un modelo que no la vio."""
    sesiones = d["sesion"].unique()
    d = d.copy()
    d["pred"] = np.nan
    imp = None
    if len(sesiones) >= 2:
        cv = GroupKFold(n_splits=min(5, len(sesiones)))
        folds = list(cv.split(d, groups=d["sesion"]))
        modo = f"GroupKFold por sesión ({len(folds)} folds, {len(sesiones)} sesiones)"
    else:                                                          # una sola sesión: corte temporal
        corte = int(len(d) * 0.7)
        hueco = VENTANA_S // PASO_S + 1                            # evita ventanas solapadas
        folds = [(np.arange(corte - hueco), np.arange(corte, len(d)))]
        modo = "una sola sesión: 70 % inicial para entrenar, 30 % final para probar"
    for tr, te in folds:
        m = nuevo_modelo().fit(d.iloc[tr][FEATURES], d.iloc[tr]["fc_futura"] - d.iloc[tr]["fc"])
        d.iloc[te, d.columns.get_loc("pred")] = d.iloc[te]["fc"] + m.predict(d.iloc[te][FEATURES])
        imp = (m, d.iloc[te])
    return d, modo, imp


# ============================================================================
# 5. Gráficas
# ============================================================================
def grafica_senal(df, salida):
    """Una ventana de 8 s: IR crudo, IR filtrado con los latidos detectados."""
    for _, g in df.groupby("sesion", sort=False):
        g = limpiar_sesion(g)
        for a, b in segmentos(g["valido"].to_numpy()):
            if b - a < 30 * FS:
                continue
            ini = a + 10 * FS
            sl = slice(ini, ini + VENTANA_S * FS)
            ir_f = -filtfilt(_B, _A, g["ir"].to_numpy()[a:b])[ini - a: ini - a + VENTANA_S * FS]
            fc_picos, _ = find_peaks(ir_f, distance=int(FS * 60 / FC_MAX), prominence=0.4 * np.std(ir_f))
            r = fc_de_ventana(ir_f)
            t = np.arange(len(ir_f)) / FS
            fig, ax = plt.subplots(2, 1, figsize=(10, 5), sharex=True)
            ax[0].plot(t, g["ir"].to_numpy()[sl], color="#64748b", lw=1)
            ax[0].set_ylabel("IR crudo")
            ax[0].set_title(f"Ventana de {VENTANA_S} s · sesión {g['sesion'].iat[0]} · {g['label'].iat[ini]}",
                            loc="left", fontsize=11)
            ax[1].plot(t, ir_f, color="#2563eb", lw=1.2, label="IR filtrado 0.5-4 Hz (invertido)")
            ax[1].plot(t[fc_picos], ir_f[fc_picos], "o", color="#dc2626", label="latido detectado")
            ax[1].set_ylabel("IR filtrado")
            ax[1].set_xlabel("tiempo (s)")
            ax[1].set_title(f"FC por picos = {r['fc_picos']:.0f} lpm · FC por FFT = {r['fc_fft']:.0f} lpm · "
                            f"variación de intervalos = {r['cv_ibi']:.2f}", loc="left", fontsize=9)
            ax[1].set_ylim(top=ax[1].get_ylim()[1] * 1.15)
            fig.legend(*ax[1].get_legend_handles_labels(), loc="lower center", ncol=2, fontsize=8, frameon=False)
            for a_ in ax:
                a_.spines[["top", "right"]].set_visible(False)
            fig.tight_layout(rect=(0, 0.05, 1, 1))
            fig.savefig(salida / "1_senal_y_latidos.png", dpi=130)
            plt.close(fig)
            return


def grafica_tiempo(d, todas, horizonte, salida):
    """FC de referencia vs predicción vs persistencia en la sesión más larga."""
    ses = todas.groupby("sesion")["k"].count().idxmax()
    v = todas[todas["sesion"] == ses].sort_values("k")
    p = d[d["sesion"] == ses].sort_values("k")
    fig, ax = plt.subplots(2, 1, figsize=(11, 6), sharex=True, gridspec_kw={"height_ratios": [3, 1]})
    for lbl, tramo in v.groupby((v["label"] != v["label"].shift()).cumsum()):
        ax[0].axvspan(tramo["t_s"].min(), tramo["t_s"].max() + PASO_S,
                      color=COLORES.get(tramo["label"].iat[0], "#94a3b8"), alpha=0.08, lw=0)
        ax[0].text(tramo["t_s"].min() + 1, 1.01, tramo["label"].iat[0], transform=ax[0].get_xaxis_transform(),
                   fontsize=8, color=COLORES.get(tramo["label"].iat[0], "#475569"))
    ax[0].plot(v["t_s"], v["fc"], color="#0f172a", lw=1.6, label="FC de referencia (PPG)")
    ax[0].plot(p["t_s"] + horizonte, p["pred"], color="#2563eb", lw=1.4,
               label=f"predicción del modelo ({horizonte} s antes)")
    ax[0].plot(p["t_s"] + horizonte, p["fc"], color="#f97316", lw=1, ls="--",
               label="persistencia (la FC no cambia)")
    ax[0].set_ylabel("FC (lpm)")
    ax[0].legend(loc="upper left", fontsize=8, frameon=False, bbox_to_anchor=(0, -0.02, 1, 1))
    ax[1].plot(v["t_s"], v["acc_std"], color="#64748b")
    ax[1].set_ylabel("movimiento\nstd |a| (m/s²)")
    ax[1].set_xlabel("tiempo (s)")
    for a_ in ax:
        a_.spines[["top", "right"]].set_visible(False)
    fig.suptitle(f"Sesión {ses}: el modelo anticipa los cambios de FC que la persistencia solo sigue",
                 x=0.01, ha="left", fontsize=11)
    fig.tight_layout()
    fig.savefig(salida / "2_fc_en_el_tiempo.png", dpi=130)
    plt.close(fig)


def grafica_errores(d, salida):
    filas = []
    for lbl, g in d.groupby("label"):
        filas.append((lbl, metricas(g["fc_futura"], g["pred"])["MAE"],
                      metricas(g["fc_futura"], g["fc"])["MAE"], len(g)))
    filas.append(("todas", metricas(d["fc_futura"], d["pred"])["MAE"],
                  metricas(d["fc_futura"], d["fc"])["MAE"], len(d)))
    x = np.arange(len(filas))
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.bar(x - 0.2, [f[2] for f in filas], 0.4, color="#f97316", label="persistencia")
    ax.bar(x + 0.2, [f[1] for f in filas], 0.4, color="#2563eb", label="modelo")
    for i, f in enumerate(filas):
        ax.text(i - 0.2, f[2], f"{f[2]:.1f}", ha="center", va="bottom", fontsize=8)
        ax.text(i + 0.2, f[1], f"{f[1]:.1f}", ha="center", va="bottom", fontsize=8)
    ax.set_xticks(x, [f"{f[0]}\n(n={f[3]})" for f in filas])
    ax.set_ylabel("error absoluto medio (lpm)")
    ax.set_title("Error fuera de muestra por actividad (menor es mejor)", loc="left", fontsize=11)
    ax.legend(frameon=False)
    ax.spines[["top", "right"]].set_visible(False)
    fig.tight_layout()
    fig.savefig(salida / "3_error_por_actividad.png", dpi=130)
    plt.close(fig)


def grafica_importancia(imp, salida):
    m, prueba = imp
    r = permutation_importance(m, prueba[FEATURES], prueba["fc_futura"] - prueba["fc"],
                               n_repeats=10, random_state=0, scoring="neg_mean_absolute_error")
    orden = np.argsort(r.importances_mean)
    fig, ax = plt.subplots(figsize=(7, 5))
    ax.barh(np.array(FEATURES)[orden], r.importances_mean[orden], xerr=r.importances_std[orden],
            color="#2563eb")
    ax.set_xlabel("aumento del MAE al desordenar la feature (lpm)")
    ax.set_title("¿Qué usa el modelo? (importancia por permutación)", loc="left", fontsize=11)
    ax.spines[["top", "right"]].set_visible(False)
    fig.tight_layout()
    fig.savefig(salida / "4_importancia.png", dpi=130)
    plt.close(fig)


def diagnostico(df, todas, salida):
    """Por qué se rechazan ventanas: tabla por sesión + figura con ejemplos."""
    print("\n── Diagnóstico de la FC de referencia (% de ventanas por motivo) ──")
    tabla = pd.crosstab(todas["sesion"], todas["motivo"], normalize="index").mul(100).round(0)
    orden = [c for c in ["ok", "pocos_latidos", "irregular", "no_coincide_fft", "fuera_de_rango",
                         "sin_dedo_o_hueco"] if c in tabla]
    print(tabla[orden].astype(int).to_string())
    extra = todas.groupby("sesion").agg(
        IR_medio=("ir_dc", "median"), saturado_pct=("saturado_%", "mean"),
        perfusion_pct=("perfusion", "median"), latidos_8s=("n_picos", "median"),
        fc_picos=("fc_picos", "median"), fc_fft=("fc_fft", "median"), cv=("cv_ibi", "median"))
    print("\nValores típicos por sesión (medianas):")
    print(extra.round(2).to_string())
    print("Guía: perfusion < 0.1 % = pulso muy débil (poca presión o dedo mal colocado) · "
          "saturado > 0 % = baja el brillo del LED · latidos_8s ≈ FC/7.5 si todo va bien.")

    # Figura: 6 ventanas rechazadas (prioridad: sesiones de reposo) + 1 aceptada
    W = VENTANA_S * FS
    rech = todas[todas["motivo"].isin(["pocos_latidos", "irregular", "no_coincide_fft", "fuera_de_rango"])]
    rech = pd.concat([rech[rech["label"] == "reposo"], rech[rech["label"] != "reposo"]])
    ejemplos = rech.groupby("motivo", sort=False).head(2).head(6)
    acept = todas[todas["motivo"] == "ok"].head(1)
    ejemplos = pd.concat([acept, ejemplos])
    if ejemplos.empty:
        return
    n = len(ejemplos)
    fig, axs = plt.subplots(n, 1, figsize=(10, 1.9 * n), squeeze=False)
    sesiones = {s: limpiar_sesion(g) for s, g in df.groupby("sesion", sort=False)}
    for ax, (_, e) in zip(axs[:, 0], ejemplos.iterrows()):
        g = sesiones[e["sesion"]]
        ini = int(e["k"]) * PASO_S * FS
        crudo = g["ir"].to_numpy()[ini:ini + W]
        filt = -filtfilt(_B, _A, crudo)
        picos = fc_de_ventana(filt)["picos"]
        t = np.arange(len(filt)) / FS
        color = "#16a34a" if e["motivo"] == "ok" else "#dc2626"
        ax.plot(t, filt, color="#2563eb", lw=1)
        ax.plot(t[picos], filt[picos], "o", ms=4, color=color)
        ax.set_title(f"{e['motivo'].upper()} · {e['sesion']} ({e['label']}) · t={e['t_s']:.0f} s · "
                     f"picos {e['fc_picos']:.0f} lpm · FFT {e['fc_fft']:.0f} lpm · CV {e['cv_ibi']:.2f} · "
                     f"perfusión {e['perfusion']:.2f} %", loc="left", fontsize=8, color=color)
        ax.set_yticks([])
        ax.spines[["top", "right", "left"]].set_visible(False)
    axs[-1, 0].set_xlabel("tiempo dentro de la ventana (s) · IR filtrado 0.5-4 Hz, puntos = latidos detectados")
    fig.tight_layout()
    fig.savefig(salida / "5_diagnostico_ventanas.png", dpi=130)
    plt.close(fig)


# ============================================================================
# Datos sintéticos (--demo): mismo formato que cargar_s3.py
# ============================================================================
def generar_demo(n_sesiones=8, seed=0) -> pd.DataFrame:
    rng = np.random.default_rng(seed)
    objetivo = {"reposo": 0, "caminar": 30, "correr": 70}        # lpm sobre la basal
    movimiento = {"reposo": (0.0, 0.0), "caminar": (1.0, 0.9), "correr": (2.2, 1.4)}
    partes = []
    for s in range(n_sesiones):
        sujeto = f"s{s % 4 + 1:02d}"
        basal = rng.uniform(62, 78)
        bloques = ["reposo"] + list(rng.choice(["caminar", "correr", "reposo"], 4)) + ["reposo"]
        dur = rng.integers(45, 90, len(bloques)) * FS
        lbl = np.repeat(bloques, dur)
        n = len(lbl)
        t = np.arange(n) / FS
        meta = basal + np.array([objetivo[x] for x in lbl])
        fc = np.empty(n)
        fc[0] = basal
        for i in range(1, n):                                     # sube rápido, baja lento
            tau = 15 if meta[i] > fc[i - 1] else 35
            fc[i] = fc[i - 1] + (meta[i] - fc[i - 1]) / (tau * FS) + rng.normal(0, 0.02)
        fase = np.cumsum(fc / 60 / FS) % 1
        pulso = np.exp(-((fase - 0.15) / 0.06) ** 2) + 0.35 * np.exp(-((fase - 0.45) / 0.08) ** 2)
        m = np.array([movimiento[x][0] for x in lbl])
        w = 2 * np.pi * np.array([movimiento[x][1] for x in lbl])
        resp = np.sin(2 * np.pi * 0.25 * t)
        art = np.minimum(m, 1) * 150 * np.sin(w * t + 0.7)
        ir = 115_000 - 1200 * pulso + 300 * resp + art + rng.normal(0, 40, n)
        red = 90_000 - 470 * pulso + 230 * resp + 0.8 * art + rng.normal(0, 40, n)
        for _ in range(2):                                        # dedo retirado un momento
            i0 = rng.integers(20 * FS, n - 10 * FS)
            ir[i0:i0 + 3 * FS] = red[i0:i0 + 3 * FS] = 700
        d = pd.DataFrame({
            "seq": np.arange(n), "t_ms": (t * 1000).astype(int), "red": red.astype(int), "ir": ir.astype(int),
            "ax": m * 1.5 * np.sin(w * t) + rng.normal(0, .03, n),
            "ay": m * 0.8 * np.sin(2 * w * t + 1) + rng.normal(0, .03, n),
            "az": 9.81 + m * 2.0 * np.sin(2 * w * t) + rng.normal(0, .03, n),
            "gx": m * 0.6 * np.cos(w * t) + rng.normal(0, .005, n),
            "gy": m * 0.3 * np.sin(2 * w * t) + rng.normal(0, .005, n),
            "gz": m * 0.2 * np.cos(w * t + .5) + rng.normal(0, .005, n),
            "sesion": f"demo_{s:02d}", "label": lbl, "sujeto": sujeto})
        d = d.drop(index=rng.choice(n, n // 500, replace=False))  # 0.2 % de muestras perdidas
        partes.append(d)
    return pd.concat(partes, ignore_index=True)


# ============================================================================
def main():
    ap = argparse.ArgumentParser(description="Modelo de predicción de FC · PulsoSeguro")
    ap.add_argument("csv", nargs="?", help="dataset.csv generado por cargar_s3.py")
    ap.add_argument("--horizonte", type=int, default=10, help="segundos a futuro (múltiplo de 2)")
    ap.add_argument("--salida", default="salida_fc")
    ap.add_argument("--demo", action="store_true", help="usar datos sintéticos")
    args = ap.parse_args()
    if not args.csv and not args.demo:
        ap.error("indica dataset.csv o usa --demo")
    if args.horizonte % PASO_S:
        ap.error(f"--horizonte debe ser múltiplo de {PASO_S}")

    salida = Path(args.salida)
    salida.mkdir(exist_ok=True)
    df = generar_demo() if args.demo else pd.read_csv(args.csv)
    print(f"Datos: {len(df):,} muestras · {df['sesion'].nunique()} sesiones · "
          f"{df['sujeto'].nunique()} sujetos · {'SINTÉTICOS' if args.demo else args.csv}")

    todas, calidad = construir_dataset(df, args.horizonte)
    print("\n── Calidad por sesión ─────────────────────────────")
    print(calidad.to_string(index=False))
    diagnostico(df, todas, salida)

    d = todas.dropna(subset=["fc", "fc_futura"]).reset_index(drop=True)
    print(f"\nVentanas con FC actual y FC a +{args.horizonte} s: {len(d)} de {len(todas)}")
    if len(d) < 50:
        raise SystemExit("\nMuy pocas ventanas válidas para entrenar (mínimo 50). Revisa el diagnóstico "
                         f"de arriba y la figura {salida}/5_diagnostico_ventanas.png.")

    d, modo, imp = validar(d)
    d = d.dropna(subset=["pred"])
    res = {"modelo": metricas(d["fc_futura"], d["pred"]),
           "persistencia": metricas(d["fc_futura"], d["fc"]),
           "media_3_ventanas": metricas(d["fc_futura"], d["media3"])}
    print(f"\n── Validación: {modo} ──")
    print(f"Predicción a +{args.horizonte} s · {len(d)} ventanas de prueba")
    for k, v in res.items():
        print(f"  {k:18s} MAE {v['MAE']:5.2f} lpm   RMSE {v['RMSE']:5.2f} lpm")
    mejora = 100 * (1 - res["modelo"]["MAE"] / res["persistencia"]["MAE"])
    if mejora > 0:
        print(f"  -> el modelo reduce el error de la persistencia en {mejora:.0f} %")
    else:
        print(f"  -> el modelo es {-mejora:.0f} % PEOR que la persistencia: todavía no sirve.")
    if d["sesion"].nunique() < 4 or len(d) < 500:
        print("  ⚠ Pocos datos: con menos de ~4 sesiones / 500 ventanas el resultado es inestable.\n"
              "    Graba más sesiones (varias personas, cambios de actividad) antes de sacar conclusiones.")

    grafica_senal(df, salida)
    grafica_tiempo(d, todas, args.horizonte, salida)
    grafica_errores(d, salida)
    grafica_importancia(imp, salida)

    final = nuevo_modelo().fit(d[FEATURES], d["fc_futura"] - d["fc"])   # con todos los datos
    joblib.dump({"modelo": final, "features": FEATURES, "horizonte_s": args.horizonte,
                 "ventana_s": VENTANA_S, "paso_s": PASO_S, "predice": "fc_futura - fc"},
                salida / "modelo_fc.joblib")
    d.to_csv(salida / "predicciones_validacion.csv", index=False)
    calidad.to_csv(salida / "calidad_sesiones.csv", index=False)
    (salida / "metricas.json").write_text(json.dumps(
        {"horizonte_s": args.horizonte, "validacion": modo, "n_ventanas": len(d), **res}, indent=2))
    print(f"\nGuardado en {salida}/: modelo_fc.joblib, metricas.json, predicciones y 4 gráficas")


if __name__ == "__main__":
    main()
