"""
simular_features.py · Publica en bio/<thing>/features lo que mandaría la pulsera,
calculado con features.py a partir de un dataset.csv. Sirve para probar la pila
de AWS (Rule → Lambda → DynamoDB → bio/<thing>/pred) sin el ESP32.

    export AWS_IOT_ENDPOINT=xxxxxxxx-ats.iot.us-east-1.amazonaws.com
    python herramientas/simular_features.py dataset.csv --thing pulsera01 \
        --cert certs/pc-captura.cert.pem --key certs/pc-captura.private.key --ca certs/AmazonRootCA1.pem

Se conecta con el client ID 'pc-captura' (debe ser igual al nombre de su Thing, como en control.py)
y publica en el tópico de la pulsera: la política bio-policy lo permite (topic/bio/*).
También se suscribe a bio/<thing>/pred para que veas la respuesta de la Lambda en vivo.
"""
import argparse
import json
import os
import pathlib
import secrets
import ssl
import sys
import time

import pandas as pd
import paho.mqtt.client as mqtt

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "modelo"))
from features import NOMBRES, VERSION_FEATURES, extraer_sesion  # noqa: E402

for _s in (sys.stdout, sys.stderr):          # Windows: cp1252 en pipes no tiene "→", "✅"…
    _s.reconfigure(encoding="utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dataset")
    ap.add_argument("--thing", required=True, help="nombre del Thing de la pulsera (va en el tópico)")
    ap.add_argument("--cliente", default="pc-captura")
    ap.add_argument("--cert", required=True); ap.add_argument("--key", required=True); ap.add_argument("--ca", required=True)
    ap.add_argument("--sesion", help="sesión del dataset (por defecto, la primera)")
    ap.add_argument("--intervalo", type=float, default=2.0, help="segundos entre mensajes (2 = tiempo real)")
    a = ap.parse_args()
    endpoint = os.environ.get("AWS_IOT_ENDPOINT") or sys.exit("define AWS_IOT_ENDPOINT")

    df = pd.read_csv(a.dataset)
    ses = a.sesion or df["sesion"].iloc[0]
    filas = [f for f in extraer_sesion(df[df["sesion"] == ses].sort_values("seq")) if f["valida"]]
    print(f"Sesión {ses}: {len(filas)} ventanas válidas")

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=a.cliente)
    c.tls_set(ca_certs=a.ca, certfile=a.cert, keyfile=a.key, tls_version=ssl.PROTOCOL_TLSv1_2)
    c.on_message = lambda _c, _u, msg: print(f"   ← {msg.topic}: {msg.payload.decode()}")
    c.connect(endpoint, 8883)
    c.subscribe(f"bio/{a.thing}/pred", qos=0)
    c.loop_start()
    time.sleep(1)

    boot = secrets.token_hex(4)          # como el ESP32: un id nuevo por "arranque"
    for f in filas:
        msg = {"v": VERSION_FEATURES, "boot": boot, "win": f["win"], "t_ms": f["seq"] * 10,
               "f": [round(f[n], 4) for n in NOMBRES]}
        c.publish(f"bio/{a.thing}/features", json.dumps(msg), qos=0)
        print(f"→ win {f['win']:4d}  fc {f['fc']:6.1f}")
        time.sleep(a.intervalo)
    time.sleep(3)
    c.loop_stop(); c.disconnect()


if __name__ == "__main__":
    main()
