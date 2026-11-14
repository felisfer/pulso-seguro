"""
exportar_modelo.py · Entrena el predictor de FC a +10 s con las features del ESP32
y lo exporta a un JSON que la Lambda evalúa sin numpy ni scikit-learn.

    python modelo/exportar_modelo.py dataset.csv -o modelo_fc.json
    python modelo/exportar_modelo.py --demo                 # con datos sintéticos

Por qué Ridge: una predicción Ridge es  delta = b + Σ wᵢ·(xᵢ - μᵢ)/σᵢ.
Son 16 multiplicaciones: cabe en un JSON de 2 KB, la Lambda arranca en
milisegundos y el mismo JSON se puede llevar después al ESP32 (opción "en el borde").

El JSON incluye la decisión honesta: si en validación cruzada Ridge NO supera a
la media de 3 ventanas, "usar" = "media3" y la Lambda publica la línea base
(y sigue guardando la predicción de Ridge en DynamoDB para comparar en vivo).
"""
import argparse
import datetime as dt
import json
import pathlib
import sys

import numpy as np
import pandas as pd
from sklearn.linear_model import RidgeCV
from sklearn.model_selection import GroupKFold
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from features import NOMBRES, PASO, VERSION_FEATURES, extraer_sesion  # noqa: E402

for _s in (sys.stdout, sys.stderr):          # Windows: cp1252 en pipes no tiene "→", "✅"…
    _s.reconfigure(encoding="utf-8", errors="replace")

HORIZONTE_S = 10
PASOS = HORIZONTE_S * 100 // PASO           # 5 ventanas
DELTA_MAX = 40.0


def construir_tabla(df: pd.DataFrame) -> pd.DataFrame:
    filas = []
    for ses, g in df.groupby("sesion", sort=False):
        for f in extraer_sesion(g.sort_values("seq")):
            filas.append({"sesion": ses, **f})
    t = pd.DataFrame(filas)
    # Objetivo: la FC de la ventana que termina exactamente 10 s (1000 muestras) después
    fut = t[["sesion", "seq", "valida", "fc"]].copy()
    fut["seq"] -= PASOS * PASO
    t = t.merge(fut, on=["sesion", "seq"], how="left", suffixes=("", "_fut"))
    t["valida_fut"] = t["valida_fut"].fillna(False).astype(bool)
    t["y"] = t["fc_fut"] - t["fc"]
    return t


def metricas(err):
    err = np.asarray(err)
    return {"mae": round(float(np.mean(np.abs(err))), 3), "rmse": round(float(np.sqrt(np.mean(err ** 2))), 3)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dataset", nargs="?")
    ap.add_argument("-o", "--salida", default="modelo_fc.json")
    ap.add_argument("--demo", action="store_true", help="genera datos sintéticos con transiciones")
    a = ap.parse_args()

    if a.demo:
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "herramientas"))
        from sintetico import sesion
        rng = np.random.default_rng(7)
        df = pd.concat([sesion(rng, f"sim{i:02d}", f"s{i % 3}", 240) for i in range(8)])
    elif a.dataset:
        df = pd.read_csv(a.dataset)
    else:
        ap.error("indica dataset.csv o --demo")

    t = construir_tabla(df)
    d = t[t["valida"] & t["valida_fut"]].reset_index(drop=True)
    print(f"Ventanas: {len(t)} | válidas con objetivo a +{HORIZONTE_S} s: {len(d)} | sesiones: {d['sesion'].nunique()}")
    if d["sesion"].nunique() < 2:
        sys.exit("Se necesitan al menos 2 sesiones para validar por sesión.")

    X, y, grupos = d[NOMBRES].to_numpy(), d["y"].to_numpy(), d["sesion"].to_numpy()
    base_media3 = d[["fc", "fc_lag1", "fc_lag2"]].mean(axis=1).to_numpy() - d["fc"].to_numpy()

    modelo = lambda: make_pipeline(StandardScaler(), RidgeCV(alphas=np.logspace(-2, 3, 20)))
    pred = np.zeros_like(y)
    for tr, te in GroupKFold(n_splits=min(5, d["sesion"].nunique())).split(X, y, grupos):
        pred[te] = np.clip(modelo().fit(X[tr], y[tr]).predict(X[te]), -DELTA_MAX, DELTA_MAX)

    cv = {"ridge": metricas(pred - y), "persistencia": metricas(-y), "media3": metricas(base_media3 - y)}
    print(f"\n{'+10 s':<14}{'MAE':>8}{'RMSE':>8}")
    for k, m in cv.items():
        print(f"{k:<14}{m['mae']:>8.2f}{m['rmse']:>8.2f}")
    usar = "ridge" if cv["ridge"]["mae"] < cv["media3"]["mae"] else "media3"
    print(f"\n→ La Lambda publicará: {usar}"
          + ("" if usar == "ridge" else "  (Ridge aún no supera la línea base; se sigue guardando para comparar)"))

    final = modelo().fit(X, y)
    esc, rid = final.named_steps["standardscaler"], final.named_steps["ridgecv"]
    version = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%S")
    ejemplos = [{"f": [round(float(v), 6) for v in X[i]],
                 "delta": float(np.clip(final.predict(X[i:i + 1])[0], -DELTA_MAX, DELTA_MAX))}
                for i in np.linspace(0, len(X) - 1, 3).astype(int)]
    js = {
        "version": version,
        "tipo": "ridge",
        "usar": usar,
        "version_features": VERSION_FEATURES,
        "horizonte_s": HORIZONTE_S,
        "features": NOMBRES,
        "media": esc.mean_.tolist(),
        "escala": esc.scale_.tolist(),
        "coef": rid.coef_.tolist(),
        "intercepto": float(rid.intercept_),
        "alpha": float(rid.alpha_),
        "delta_max": DELTA_MAX,
        "validacion": {"metodo": "GroupKFold por sesión", "n": int(len(d)),
                       "sesiones": int(d["sesion"].nunique()), **cv},
        "ejemplos": ejemplos,     # la Lambda y el ESP32 deben reproducir estos deltas
    }
    # ASCII puro (los acentos quedan como \u00f3): se lee igual en Windows, Linux y la Lambda.
    pathlib.Path(a.salida).write_text(json.dumps(js, indent=1, ensure_ascii=True), encoding="utf-8")
    print(f"\n{a.salida} escrito (versión {version}, {pathlib.Path(a.salida).stat().st_size} bytes)")
    print("Pesos más grandes (sobre features estandarizadas):")
    for n, w in sorted(zip(NOMBRES, rid.coef_), key=lambda p: -abs(p[1]))[:5]:
        print(f"  {n:<14}{w:+.3f}")


if __name__ == "__main__":
    main()
