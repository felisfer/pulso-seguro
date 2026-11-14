"""
test_lambda.py · Prueba la Lambda completa contra AWS simulado (moto): S3 con el modelo,
tabla DynamoDB y publicación MQTT. No necesita credenciales ni cuesta nada.

    pip install "moto[dynamodb,s3,iotdata]" pandas numpy scikit-learn
    python lambda/test_lambda.py [dataset.csv] [modelo_fc.json]

Sin argumentos genera datos sintéticos y entrena un modelo al vuelo.
"""
import importlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

import boto3
import numpy as np
import pandas as pd
from moto import mock_aws

RAIZ = pathlib.Path(__file__).resolve().parents[1]
ENV_UTF8 = {**os.environ, "PYTHONUTF8": "1"}


def correr(*args):
    """Corre un script de Python y, si falla, muestra SU error en vez de solo el código de salida."""
    r = subprocess.run([sys.executable, *map(str, args)], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=ENV_UTF8)
    if r.returncode != 0:
        sys.exit(f"Falló: {' '.join(map(str, args))}\n--- stdout ---\n{r.stdout}\n--- stderr ---\n{r.stderr}")
    return r.stdout
sys.path.insert(0, str(RAIZ / "modelo"))
sys.path.insert(0, str(RAIZ / "lambda" / "src"))
from features import NOMBRES, VERSION_FEATURES, extraer_sesion  # noqa: E402

for _s in (sys.stdout, sys.stderr):          # Windows: cp1252 en pipes no tiene "→", "✅"…
    _s.reconfigure(encoding="utf-8", errors="replace")

os.environ.update(AWS_DEFAULT_REGION="us-east-1", AWS_ACCESS_KEY_ID="x", AWS_SECRET_ACCESS_KEY="x",
                  TABLA="pulsoseguro_predicciones", MODELO_BUCKET="bucket-pruebas",
                  IOT_ENDPOINT="https://ejemplo-ats.iot.us-east-1.amazonaws.com")


@mock_aws
def main():
    tmp = pathlib.Path(tempfile.mkdtemp())
    dataset = sys.argv[1] if len(sys.argv) > 1 else None
    modelo = sys.argv[2] if len(sys.argv) > 2 else None
    if not dataset:
        dataset = tmp / "ds.csv"
        correr(RAIZ / "herramientas/sintetico.py", "--sesiones", "6", "-o", dataset)
    if not modelo:
        modelo = tmp / "modelo_fc.json"
        correr(RAIZ / "modelo/exportar_modelo.py", dataset, "-o", modelo)

    # --- infraestructura simulada (mismo esquema que infra/template.yaml)
    boto3.client("s3").create_bucket(Bucket="bucket-pruebas")
    boto3.client("s3").upload_file(str(modelo), "bucket-pruebas", "modelos/modelo_fc.json")
    boto3.client("dynamodb").create_table(
        TableName="pulsoseguro_predicciones",
        KeySchema=[{"AttributeName": "dispositivo", "KeyType": "HASH"}, {"AttributeName": "ts", "KeyType": "RANGE"}],
        AttributeDefinitions=[{"AttributeName": "dispositivo", "AttributeType": "S"},
                              {"AttributeName": "ts", "AttributeType": "N"}],
        ProvisionedThroughput={"ReadCapacityUnits": 5, "WriteCapacityUnits": 5})

    lf = importlib.import_module("lambda_function")
    publicados = []                                   # moto no simula endpoints -ats propios:
    real = boto3.client("iot-data")                   # valido los parámetros con el cliente por defecto
    class IotEspia:
        def publish(self, **kw):
            real.publish(**kw)
            publicados.append(kw)
    lf.iot = IotEspia()

    # --- una sesión enviada tal como la mandaría el ESP32 (solo ventanas válidas)
    df = pd.read_csv(dataset)
    ses = df["sesion"].iloc[0]
    filas = extraer_sesion(df[df["sesion"] == ses].sort_values("seq"))
    rng = np.random.default_rng(0)
    t0 = 1_762_900_000_000
    enviados = 0
    for f in filas:
        if not f["valida"]:
            continue
        ev = {"v": VERSION_FEATURES, "boot": "9f3a1c2e", "win": f["win"], "t_ms": f["seq"] * 10,
              "f": [round(f[n], 4) for n in NOMBRES], "thing": "pulsera01",
              "ts": t0 + f["seq"] * 10 + int(rng.integers(50, 900))}        # latencia de red simulada
        r = lf.lambda_handler(ev, None)
        assert r["ok"], r
        enviados += 1
    # reintento duplicado: no debe crear otro ítem
    assert lf.lambda_handler(ev, None).get("duplicado")
    # mensaje malformado: se descarta sin excepción
    assert not lf.lambda_handler({**ev, "f": [1, 2]}, None)["ok"]

    items = boto3.resource("dynamodb").Table("pulsoseguro_predicciones").scan()["Items"]
    cerrados = [i for i in items if "fc_real" in i]
    mae_m = np.mean([abs(float(i["err_modelo"])) for i in cerrados])
    mae_b = np.mean([abs(float(i["err_base"])) for i in cerrados])
    print(f"Eventos: {enviados} | ítems: {len(items)} | con FC real a +10 s: {len(cerrados)}")
    print(f"MAE en 'producción' → modelo {mae_m:.2f} lpm, media3 {mae_b:.2f} lpm")
    print("Ejemplo de ítem:", json.dumps({k: str(v) for k, v in sorted(cerrados[len(cerrados) // 2].items())},
                                         ensure_ascii=False, indent=1))
    print("Último MQTT:", publicados[-1]["topic"], publicados[-1]["payload"])
    assert len(items) == enviados and len(publicados) == enviados
    # Contrato de /pred: lo que lee el ESP32 y lo que lee el dashboard
    pred = json.loads(publicados[-1]["payload"])
    for k in ("win", "fc_prevista", "semaforo", "ts", "fc_pred", "horizonte_s", "modelo"):
        assert k in pred, f"falta {k} en /pred"
    assert pred["fc_pred"] == pred["fc_prevista"] and pred["ts"] == ev["ts"]
    assert len(cerrados) >= 0.8 * (enviados - 5)
    print("\nLAMBDA OK ✅")


if __name__ == "__main__":
    main()
