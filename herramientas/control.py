#!/usr/bin/env python3
"""Controla una sesion de captura del ESP32 via AWS IoT Core.

Ejemplo:
  python control.py --label reposo --sujeto s01 --duracion 60 --monitor
"""
import argparse, json, time
from datetime import datetime
import paho.mqtt.client as mqtt
import os

ENDPOINT = os.environ["AWS_IOT_ENDPOINT"]
DISPOSITIVO = os.environ["AWS_IOT_DEVICE"]
CERTS = os.environ["AWS_IOT_CERTS"]
CA_CERTS = os.environ["AWS_IOT_CA_CERTS"]
CERTFILE = os.environ["AWS_IOT_CERTFILE"]
KEYFILE = os.environ["AWS_IOT_KEYFILE"]

ap = argparse.ArgumentParser()
ap.add_argument("--label", required=True)
ap.add_argument("--sujeto", default="s01")
ap.add_argument("--duracion", type=float, default=60)
ap.add_argument("--monitor", action="store_true", help="cuenta los lotes recibidos")
args = ap.parse_args()

lotes = 0

def on_connect(cli, userdata, flags, reason_code, properties):
    print(f"Conectado: {reason_code}")
    cli.subscribe(f"bio/{DISPOSITIVO}/status", qos=1)
    if args.monitor:
        cli.subscribe(f"bio/{DISPOSITIVO}/data", qos=0)

def on_message(cli, userdata, msg):
    global lotes
    if msg.topic.endswith("/status"):
        print(f"Estado del dispositivo: {msg.payload.decode()}")
    else:
        lotes += 1
        print(f"\r  lotes: {lotes}  ({len(msg.payload)} bytes el ultimo)", end="", flush=True)

cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pc-captura")
cli.tls_set(ca_certs=f"{CA_CERTS}",
            certfile=f"{CERTFILE}",
            keyfile=f"{KEYFILE}")
cli.on_connect = on_connect
cli.on_message = on_message
cli.connect(ENDPOINT, 8883, keepalive=60)
cli.loop_start()
time.sleep(2)

sesion = datetime.now().strftime("%Y%m%d_%H%M%S")
cmd = {"cmd": "start", "sesion": sesion, "label": args.label, "sujeto": args.sujeto}
cli.publish(f"bio/{DISPOSITIVO}/cmd", json.dumps(cmd), qos=1).wait_for_publish()
print(f"Sesion {sesion} | {args.label} | {args.sujeto} | {args.duracion:.0f} s")

try:
    time.sleep(args.duracion)
except KeyboardInterrupt:
    print("\nInterrumpido")
finally:
    cli.publish(f"bio/{DISPOSITIVO}/cmd", json.dumps({"cmd": "stop"}), qos=1).wait_for_publish()
    print("\nstop enviado")
    cli.loop_stop(); cli.disconnect()
