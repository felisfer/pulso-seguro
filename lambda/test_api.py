"""
test_api.py · Prueba la Lambda del historial (lambda/api/api_historial.py) contra DynamoDB
simulado (moto). No necesita credenciales ni cuesta nada.

    pip install "moto[dynamodb]"
    python lambda/test_api.py
"""
import importlib
import json
import os
import pathlib
import sys
from decimal import Decimal

import boto3
from moto import mock_aws

RAIZ = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAIZ / "lambda" / "api"))
for _s in (sys.stdout, sys.stderr):          # Windows: cp1252 en pipes no tiene "✅"
    _s.reconfigure(encoding="utf-8", errors="replace")

os.environ.update(AWS_DEFAULT_REGION="us-east-2", AWS_ACCESS_KEY_ID="x", AWS_SECRET_ACCESS_KEY="x",
                  TABLA="pulsoseguro_predicciones")

T0 = 1_762_900_000_000


def pedir(api, **q):
    """Evento de HTTP API (payload 2.0) ya autorizado por el JWT."""
    ev = {"queryStringParameters": {k: str(v) for k, v in q.items()},
          "requestContext": {"authorizer": {"jwt": {"claims": {"sub": "abc"}}}}}
    r = api.lambda_handler(ev, None)
    return r["statusCode"], json.loads(r["body"])


@mock_aws
def main():
    boto3.client("dynamodb").create_table(
        TableName="pulsoseguro_predicciones",
        KeySchema=[{"AttributeName": "dispositivo", "KeyType": "HASH"}, {"AttributeName": "ts", "KeyType": "RANGE"}],
        AttributeDefinitions=[{"AttributeName": "dispositivo", "AttributeType": "S"},
                              {"AttributeName": "ts", "AttributeType": "N"}],
        ProvisionedThroughput={"ReadCapacityUnits": 5, "WriteCapacityUnits": 5})
    tabla = boto3.resource("dynamodb").Table("pulsoseguro_predicciones")

    # 1 h de predicciones cada 2 s (1800 ítems), igual que las escribe lambda_function.py.
    # Las últimas 5 aún no tienen fc_real (su ciclo se cierra 10 s después).
    n = 1800
    with tabla.batch_writer() as b:
        for k in range(n):
            ts = T0 + k * 2000
            item = {"dispositivo": "pulsera01", "ts": ts, "boot": "9f3a1c2e", "win": k,
                    "fc": Decimal(str(70 + k % 10)), "fc_prevista": Decimal(str(71 + k % 10)),
                    "fc_pred_modelo": Decimal("71"), "fc_pred_base": Decimal("70"),
                    "usado": "media3", "modelo_ver": "20261114-073743", "objetivo_ts": ts + 10_000}
            if k < n - 5:
                item.update(fc_real=Decimal(str(72 + k % 10)), err_modelo=Decimal("1"), err_base=Decimal("2"))
            b.put_item(Item=item)
        b.put_item(Item={"dispositivo": "otra", "ts": T0, "fc": Decimal("60"), "fc_prevista": Decimal("61")})

    api = importlib.import_module("api_historial")

    # --- rango pequeño: sin reducir, en orden cronológico y con el contrato del dashboard
    c, r = pedir(api, thing="pulsera01", desde=T0, hasta=T0 + 60_000)
    assert c == 200 and r["total"] == 31 and not r["truncado"], r
    p = r["items"]
    assert [x["ts"] for x in p] == sorted(x["ts"] for x in p)
    assert p[0] == {"ts": T0, "fc_pred": 71.0, "horizonte_s": 10, "modelo": "20261114-073743",
                    "fc": 70.0, "usado": "media3", "fc_real": 72.0}, p[0]

    # --- 1 h completa: 1800 ítems → MAX_PUNTOS (720) promediados; los sin fc_real no lo inventan
    c, r = pedir(api, thing="pulsera01", desde=T0, hasta=T0 + 3_600_000)
    assert c == 200 and r["total"] == n and not r["truncado"]
    assert len(r["items"]) <= api.MAX_PUNTOS, len(r["items"])
    assert "fc_real" not in r["items"][-1] and "fc_real" in r["items"][0]
    assert all(70 <= x["fc"] <= 79 for x in r["items"])

    # --- tope de lectura: se conservan los MÁS RECIENTES y se avisa con truncado
    api.MAX_ITEMS = 500
    c, r = pedir(api, thing="pulsera01", desde=T0, hasta=T0 + 3_600_000)
    assert c == 200 and r["truncado"] and r["total"] == 500
    assert r["items"][-1]["ts"] >= T0 + (n - 3) * 2000, "debe incluir lo más reciente"
    api.MAX_ITEMS = 5000

    # --- otro thing no se mezcla
    c, r = pedir(api, thing="otra", desde=T0, hasta=T0 + 3_600_000)
    assert c == 200 and r["total"] == 1

    # --- errores de entrada
    assert pedir(api, thing="a b", desde=T0, hasta=T0 + 1)[0] == 400
    assert pedir(api, thing="pulsera01", desde="x", hasta=T0)[0] == 400
    assert pedir(api, thing="pulsera01", desde=T0, hasta=T0)[0] == 400
    assert pedir(api, thing="pulsera01", desde=T0, hasta=T0 + 2 * 86_400_000)[0] == 400
    assert api.lambda_handler({}, None)["statusCode"] == 400

    print("API HISTORIAL OK ✅")


if __name__ == "__main__":
    main()
