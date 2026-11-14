"""
PulsoSeguro · Lambda de predicción de FC a +10 s.

Disparo:  IoT Rule  SELECT *, topic(2) AS thing, timestamp() AS ts FROM 'bio/+/features'
Entrada:  {"v":1, "boot":"9f3a1c2e", "win":123, "t_ms":456789, "f":[16 features], "thing":..., "ts":...}
Salida:   1) DynamoDB  → un ítem por ventana (predicción) + cierre del ciclo con la FC real
          2) MQTT      → bio/<thing>/pred  {"win", "fc", "fc_prevista", "semaforo", "modelo"}

Sin dependencias: solo la biblioteca estándar y boto3 (incluido en el runtime).
El modelo es un JSON en S3 (exportar_modelo.py); se recarga solo si cambia su ETag,
así que publicar un modelo nuevo = `aws s3 cp modelo_fc.json s3://...` (sin redesplegar).
"""
import json
import logging
import math
import os
import time
from decimal import Decimal

import boto3
from botocore.exceptions import ClientError

log = logging.getLogger()
log.setLevel(logging.INFO)

TABLA = os.environ["TABLA"]
BUCKET = os.environ["MODELO_BUCKET"]
LLAVE = os.environ.get("MODELO_LLAVE", "modelos/modelo_fc.json")
IOT_ENDPOINT = os.environ.get("IOT_ENDPOINT", "")      # https://xxxx-ats.iot.us-east-1.amazonaws.com
TTL_DIAS = int(os.environ.get("TTL_DIAS", "30"))
UMBRAL_AMARILLO = float(os.environ.get("UMBRAL_AMARILLO", "120"))
REVISAR_MODELO_S = int(os.environ.get("REVISAR_MODELO_S", "300"))
TOL_EMPATE_MS = 3000          # tolerancia al buscar la predicción hecha hace 10 s

s3 = boto3.client("s3")
tabla = boto3.resource("dynamodb").Table(TABLA)
iot = boto3.client("iot-data", endpoint_url=IOT_ENDPOINT) if IOT_ENDPOINT else None

_modelo = {"json": None, "etag": None, "revisado": 0.0}


# ------------------------------------------------------------------ modelo
def cargar_modelo() -> dict:
    """Cachea el modelo entre invocaciones (el contenedor de Lambda se reutiliza)."""
    ahora = time.time()
    if _modelo["json"] is not None and ahora - _modelo["revisado"] < REVISAR_MODELO_S:
        return _modelo["json"]
    try:
        etag = s3.head_object(Bucket=BUCKET, Key=LLAVE)["ETag"]
        if etag != _modelo["etag"]:
            cuerpo = s3.get_object(Bucket=BUCKET, Key=LLAVE)["Body"].read()
            m = json.loads(cuerpo)
            verificar_modelo(m)
            _modelo.update(json=m, etag=etag)
            log.info(json.dumps({"evento": "modelo_cargado", "version": m["version"], "usar": m["usar"]}))
    except Exception:
        if _modelo["json"] is None:
            raise                       # sin modelo no podemos predecir: que Lambda reintente
        log.exception("No se pudo revisar el modelo; sigo con la versión en memoria")
    _modelo["revisado"] = ahora
    return _modelo["json"]


def delta_ridge(m: dict, f: list) -> float:
    z = sum(w * (x - mu) / sd for w, x, mu, sd in zip(m["coef"], f, m["media"], m["escala"]))
    d = m["intercepto"] + z
    return max(-m["delta_max"], min(m["delta_max"], d))


def verificar_modelo(m: dict):
    """Autoprueba: el JSON trae ejemplos con su salida esperada (calculada en sklearn)."""
    n = len(m["features"])
    assert len(m["coef"]) == len(m["media"]) == len(m["escala"]) == n, "dimensiones inconsistentes"
    for e in m.get("ejemplos", []):
        assert abs(delta_ridge(m, e["f"]) - e["delta"]) < 1e-3, "el modelo no reproduce sus ejemplos"


# ------------------------------------------------------------------ util
def dec(x, nd=2):
    return Decimal(str(round(float(x), nd)))


def validar(ev: dict, m: dict):
    for k in ("thing", "ts", "boot", "win", "f"):
        if k not in ev:
            raise ValueError(f"falta '{k}'")
    if int(ev.get("v", -1)) != m["version_features"]:
        raise ValueError(f"version de features {ev.get('v')} != {m['version_features']} del modelo")
    f = [float(x) for x in ev["f"]]
    if len(f) != len(m["features"]) or not all(math.isfinite(x) for x in f):
        raise ValueError("vector de features inválido")
    if not 30 <= f[0] <= 220:
        raise ValueError(f"fc fuera de rango: {f[0]}")
    return f


# ------------------------------------------------------------------ handler
def lambda_handler(event, context):
    m = cargar_modelo()
    try:
        f = validar(event, m)
    except (ValueError, TypeError) as e:
        log.warning(json.dumps({"evento": "descartado", "motivo": str(e), "thing": event.get("thing")}))
        return {"ok": False, "motivo": str(e)}       # no reintentar un mensaje malformado

    thing, ts, boot, win = str(event["thing"]), int(event["ts"]), str(event["boot"]), int(event["win"])
    idx = {n: i for i, n in enumerate(m["features"])}
    fc = f[idx["fc"]]

    pred_modelo = fc + delta_ridge(m, f)
    pred_base = (fc + f[idx["fc_lag1"]] + f[idx["fc_lag2"]]) / 3          # media de 3 ventanas
    prevista = pred_modelo if m["usar"] == "ridge" else pred_base
    semaforo = "amarillo" if prevista > UMBRAL_AMARILLO else "verde"

    item = {
        "dispositivo": thing, "ts": ts, "boot": boot, "win": win,
        "t_ms": int(event.get("t_ms", 0)),
        "fc": dec(fc), "acc_std": dec(f[idx["acc_std"]], 3),
        "fc_pred_modelo": dec(pred_modelo), "fc_pred_base": dec(pred_base),
        "fc_prevista": dec(prevista), "usado": m["usar"], "semaforo_pred": semaforo,
        "modelo_ver": m["version"], "objetivo_ts": ts + 1000 * m["horizonte_s"],
        "expira": ts // 1000 + TTL_DIAS * 86400,
    }
    try:
        # Idempotente: si IoT/Lambda reintenta el mismo evento (mismo ts), no se duplica
        tabla.put_item(Item=item, ConditionExpression="attribute_not_exists(ts)")
    except ClientError as e:
        if e.response["Error"]["Code"] == "ConditionalCheckFailedException":
            return {"ok": True, "duplicado": True}
        raise

    if iot:
        iot.publish(topic=f"bio/{thing}/pred", qos=0, payload=json.dumps({
            "win": win, "fc": round(fc, 1), "fc_prevista": round(prevista, 1),
            "semaforo": semaforo, "modelo": m["version"], "usado": m["usar"]}))

    cerrado = cerrar_ciclo(thing, ts, boot, win, fc, m["horizonte_s"])
    log.info(json.dumps({"evento": "prediccion", "thing": thing, "win": win, "fc": round(fc, 1),
                         "prevista": round(prevista, 1), "cerro": cerrado}))
    return {"ok": True, "fc_prevista": round(prevista, 1), "cerro_ciclo": cerrado}


def cerrar_ciclo(thing, ts, boot, win, fc_real, horizonte_s) -> bool:
    """La FC de AHORA es el 'valor real' de la predicción hecha hace 10 s (win - 5).
    Se escribe en ese ítem junto con el error del modelo y de la línea base:
    así DynamoDB guarda el MAE en producción sin ningún proceso extra."""
    pasos = horizonte_s // 2
    centro = ts - 1000 * horizonte_s
    r = tabla.query(
        KeyConditionExpression="dispositivo = :d AND ts BETWEEN :a AND :b",
        FilterExpression="boot = :boot AND win = :w",
        ExpressionAttributeValues={":d": thing, ":a": centro - TOL_EMPATE_MS, ":b": centro + TOL_EMPATE_MS,
                                   ":boot": boot, ":w": win - pasos},
        ProjectionExpression="ts, fc_pred_modelo, fc_pred_base",
    )
    if not r["Items"]:
        return False                    # la ventana de hace 10 s no fue válida o no llegó
    viejo = r["Items"][0]
    try:
        tabla.update_item(
            Key={"dispositivo": thing, "ts": viejo["ts"]},
            UpdateExpression="SET fc_real = :r, err_modelo = :em, err_base = :eb",
            ConditionExpression="attribute_not_exists(fc_real)",
            ExpressionAttributeValues={":r": dec(fc_real),
                                       ":em": dec(float(viejo["fc_pred_modelo"]) - fc_real),
                                       ":eb": dec(float(viejo["fc_pred_base"]) - fc_real)},
        )
    except ClientError as e:
        if e.response["Error"]["Code"] != "ConditionalCheckFailedException":
            raise
    return True
