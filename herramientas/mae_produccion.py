"""
mae_produccion.py · ¿El modelo desplegado le gana a la línea base CON DATOS REALES?
Lee de DynamoDB los ítems que ya tienen fc_real (cerrados por la Lambda 10 s después)
y resume el error por versión de modelo.

    python herramientas/mae_produccion.py --thing pulsera01 --horas 24
"""
import argparse
import sys
import time

import boto3
import pandas as pd
from boto3.dynamodb.conditions import Key

for _s in (sys.stdout, sys.stderr):          # Windows: cp1252 en pipes no tiene "→", "✅"…
    _s.reconfigure(encoding="utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--thing", required=True)
    ap.add_argument("--horas", type=float, default=24)
    ap.add_argument("--tabla", default="pulsoseguro_predicciones")
    a = ap.parse_args()

    t = boto3.resource("dynamodb").Table(a.tabla)
    desde = int((time.time() - a.horas * 3600) * 1000)
    kw = dict(KeyConditionExpression=Key("dispositivo").eq(a.thing) & Key("ts").gte(desde))
    items = []
    while True:                                     # paginación (1 MB por página)
        r = t.query(**kw)
        items += r["Items"]
        if "LastEvaluatedKey" not in r:
            break
        kw["ExclusiveStartKey"] = r["LastEvaluatedKey"]

    df = pd.DataFrame(items)
    if df.empty or "fc_real" not in df:
        print("Aún no hay predicciones cerradas en ese rango.")
        return
    df = df.dropna(subset=["fc_real"])
    for c in ("err_modelo", "err_base", "fc_real"):
        df[c] = df[c].astype(float)
    res = df.groupby("modelo_ver").agg(
        n=("fc_real", "size"),
        mae_modelo=("err_modelo", lambda e: e.abs().mean()),
        mae_media3=("err_base", lambda e: e.abs().mean()),
        fc_media=("fc_real", "mean"),
    ).round(2)
    res["gana_modelo"] = res["mae_modelo"] < res["mae_media3"]
    print(res.to_string())


if __name__ == "__main__":
    main()
