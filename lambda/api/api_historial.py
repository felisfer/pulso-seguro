"""
PulsoSeguro · API del historial de predicciones (HTTP API → Lambda → DynamoDB).

    GET /predicciones?thing=<thing>&desde=<ms>&hasta=<ms>
    Authorization: Bearer <id_token de Cognito>   (lo valida el autorizador JWT de API Gateway)

    200 → {"items": [{"ts", "fc_pred", "horizonte_s", "modelo", "fc", "fc_real"?, "usado"}],
           "total": <ítems leídos>, "truncado": <bool>}

- `ts` es el instante de la predicción (ms); `fc_pred` es la FC prevista para `ts + horizonte_s`.
- `fc_real` (la FC medida a +10 s) solo existe cuando la Lambda de predicción cerró el ciclo.
- Para no gastar la capacidad de lectura (5 RCU aprovisionadas), se leen como máximo MAX_ITEMS
  ítems, empezando por los más recientes (`truncado` = true si quedaron más), y la respuesta se
  reduce a MAX_PUNTOS promediando grupos consecutivos (la gráfica no necesita más).

Sin dependencias: solo la biblioteca estándar y boto3 (incluido en el runtime).
"""
import json
import logging
import os
import re

import boto3
from boto3.dynamodb.conditions import Key

log = logging.getLogger()
log.setLevel(logging.INFO)

tabla = boto3.resource("dynamodb").Table(os.environ["TABLA"])
MAX_ITEMS = int(os.environ.get("MAX_ITEMS", "5000"))       # ~2 MB ≈ 250 RCU por consulta
MAX_PUNTOS = int(os.environ.get("MAX_PUNTOS", "720"))
MAX_RANGO_MS = 24 * 3600 * 1000 + 5 * 60 * 1000             # 24 h (+ holgura de relojes)
THING_VALIDO = re.compile(r"^[A-Za-z0-9_:-]{1,128}$")       # nombres válidos de un Thing de IoT


def responder(codigo: int, cuerpo: dict) -> dict:
    return {
        "statusCode": codigo,
        "headers": {"Content-Type": "application/json", "Cache-Control": "no-store"},
        "body": json.dumps(cuerpo, separators=(",", ":"), ensure_ascii=False),
    }


def leer(thing: str, desde: int, hasta: int) -> tuple[list, bool]:
    """Ítems del rango en orden cronológico y si se cortó en MAX_ITEMS (se conservan los recientes)."""
    kw = {
        "KeyConditionExpression": Key("dispositivo").eq(thing) & Key("ts").between(desde, hasta),
        "ProjectionExpression": "#ts, fc, fc_prevista, fc_real, modelo_ver, usado, objetivo_ts",
        "ExpressionAttributeNames": {"#ts": "ts"},
        "ScanIndexForward": False,                           # del más reciente al más viejo
    }
    items: list = []
    while True:
        r = tabla.query(**kw, Limit=min(1000, MAX_ITEMS - len(items)))
        items += r["Items"]
        if "LastEvaluatedKey" not in r:
            return items[::-1], False
        if len(items) >= MAX_ITEMS:
            return items[::-1], True
        kw["ExclusiveStartKey"] = r["LastEvaluatedKey"]


def convertir(i: dict) -> dict:
    ts = int(i["ts"])
    p = {
        "ts": ts,
        "fc_pred": round(float(i["fc_prevista"]), 1),
        "horizonte_s": round((int(i["objetivo_ts"]) - ts) / 1000) if "objetivo_ts" in i else 10,
        "modelo": str(i.get("modelo_ver", "?")),
        "fc": round(float(i["fc"]), 1),
        "usado": str(i.get("usado", "?")),
    }
    if "fc_real" in i:
        p["fc_real"] = round(float(i["fc_real"]), 1)
    return p


def reducir(puntos: list, maximo: int) -> list:
    """Promedia grupos consecutivos hasta dejar `maximo` puntos (fc_real solo con los que lo tienen)."""
    if len(puntos) <= maximo:
        return puntos
    tam = -(-len(puntos) // maximo)                          # división hacia arriba
    salida = []
    for k in range(0, len(puntos), tam):
        g = puntos[k:k + tam]
        media = lambda campo: round(sum(p[campo] for p in g) / len(g), 1)
        q = {**g[-1], "ts": round(sum(p["ts"] for p in g) / len(g)), "fc_pred": media("fc_pred"), "fc": media("fc")}
        reales = [p["fc_real"] for p in g if "fc_real" in p]
        q.pop("fc_real", None)
        if reales:
            q["fc_real"] = round(sum(reales) / len(reales), 1)
        salida.append(q)
    return salida


def lambda_handler(event, context):
    q = event.get("queryStringParameters") or {}
    thing = q.get("thing", "")
    if not THING_VALIDO.match(thing):
        return responder(400, {"error": "thing inválido"})
    try:
        desde, hasta = int(q["desde"]), int(q["hasta"])
    except (KeyError, ValueError):
        return responder(400, {"error": "desde y hasta deben ser epoch en ms"})
    if hasta <= desde or hasta - desde > MAX_RANGO_MS:
        return responder(400, {"error": "rango inválido (máximo 24 h)"})

    items, truncado = leer(thing, desde, hasta)
    puntos = reducir([convertir(i) for i in items if "fc_prevista" in i and "fc" in i], MAX_PUNTOS)
    log.info(json.dumps({"evento": "historial", "thing": thing, "rango_min": round((hasta - desde) / 60000),
                         "leidos": len(items), "devueltos": len(puntos), "truncado": truncado}))
    return responder(200, {"items": puntos, "total": len(items), "truncado": truncado})
