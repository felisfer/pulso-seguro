#!/usr/bin/env python3
"""
Captura de datos ESP32-S3 (MAX30102 + MPU6050) -> CSV etiquetado para entrenamiento.

Cada ejecución = una SESIÓN con UNA etiqueta (la actividad/estado que estás grabando).
Después unes todas las sesiones en un solo dataset.

Ejemplos:
  python captura.py --listar
  python captura.py --label reposo  --sujeto s01 --duracion 60 --graficar
  python captura.py --label caminar --sujeto s01 --duracion 60
  python captura.py --unir                      # junta todo en datos/dataset.csv
"""
import argparse
import sys
import time
from datetime import datetime
from pathlib import Path

import pandas as pd
import serial
from serial.tools import list_ports

COLUMNAS = ["seq", "t_ms", "red", "ir", "ax", "ay", "az", "gx", "gy", "gz"]
FS_NOMINAL = 100          # Hz, configurado en el firmware
UMBRAL_DEDO = 50_000      # IR por debajo de esto ≈ no hay dedo sobre el sensor
BAUD = 921600


# ----------------------------------------------------------------- puerto serie
def detectar_puerto():
    """Busca el ESP32: USB nativo del S3 (VID 0x303A) o chips puente comunes."""
    for p in list_ports.comports():
        desc = (p.description or "").upper()
        if p.vid == 0x303A or "CP210" in desc or "CH34" in desc or "USB JTAG" in desc:
            return p.device
    return None


def abrir_serial(puerto):
    ser = serial.Serial(puerto, BAUD, timeout=1)
    time.sleep(0.5)
    ser.write(b"x")               # por si quedó transmitiendo de antes
    time.sleep(0.2)
    ser.reset_input_buffer()
    return ser


# ----------------------------------------------------------------- captura
def capturar(ser, duracion):
    filas, descartadas = [], 0
    ser.write(b"s")

    # Espera confirmación del firmware
    limite = time.time() + 5
    while time.time() < limite:
        linea = ser.readline().decode("ascii", errors="ignore").strip()
        if linea.startswith("#ERR"):
            raise RuntimeError(linea)
        if linea == "#START":
            break
    else:
        raise TimeoutError("El ESP32 no respondió a 's'. ¿Firmware cargado y puerto correcto?")

    print(f"Grabando {duracion} s... (Ctrl+C para terminar antes)")
    fin = time.time() + duracion
    try:
        while time.time() < fin:
            linea = ser.readline().decode("ascii", errors="ignore").strip()
            if not linea or linea.startswith("#"):
                if linea.startswith("#ERR"):
                    raise RuntimeError(linea)
                continue
            partes = linea.split(",")
            if len(partes) != len(COLUMNAS):
                descartadas += 1          # línea cortada o corrupta
                continue
            try:
                filas.append([float(v) for v in partes])
            except ValueError:
                descartadas += 1
                continue
            if len(filas) % FS_NOMINAL == 0:
                restante = max(0, fin - time.time())
                print(f"\r  {len(filas):6d} muestras | IR={partes[3]:>7} | faltan {restante:4.0f} s",
                      end="", flush=True)
    except KeyboardInterrupt:
        print("\nInterrumpido por el usuario.")
    finally:
        ser.write(b"x")
    print()
    return filas, descartadas


# ----------------------------------------------------------------- dataframe
def construir_df(filas, label, sujeto, sesion):
    df = pd.DataFrame(filas, columns=COLUMNAS)
    df[["seq", "t_ms", "red", "ir"]] = df[["seq", "t_ms", "red", "ir"]].astype("int64")
    # Tiempo "real" de muestreo: el índice seq a 100 Hz es más fiable que millis(),
    # porque el FIFO del MAX30102 puede entregar varias muestras de golpe.
    df["t_s"] = (df["seq"] - df["seq"].iloc[0]) / FS_NOMINAL
    df["dedo"] = df["ir"] > UMBRAL_DEDO
    df["label"] = label
    df["sujeto"] = sujeto
    df["sesion"] = sesion
    return df


def reporte_calidad(df, descartadas):
    perdidas = int((df["seq"].diff().fillna(1) - 1).clip(lower=0).sum())
    dur = (df["t_ms"].iloc[-1] - df["t_ms"].iloc[0]) / 1000
    fs_real = (len(df) - 1) / dur if dur > 0 else float("nan")
    print("── Calidad de la sesión ──────────────────")
    print(f"  Muestras válidas : {len(df)}")
    print(f"  Duración         : {dur:.1f} s")
    print(f"  Frecuencia real  : {fs_real:.1f} Hz (esperado {FS_NOMINAL})")
    print(f"  Muestras perdidas: {perdidas}  | líneas corruptas: {descartadas}")
    print(f"  Con dedo puesto  : {df['dedo'].mean() * 100:.1f} %")
    if df["dedo"].mean() < 0.9:
        print("  ⚠ Mucho tiempo sin dedo: considera repetir la sesión.")


def graficar(df):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(3, 1, figsize=(11, 8), sharex=True)
    ax[0].plot(df["t_s"], df["ir"], label="IR")
    ax[0].plot(df["t_s"], df["red"], label="Rojo", alpha=0.7)
    ax[0].set_ylabel("PPG (cuentas)"); ax[0].legend()
    for c in ["ax", "ay", "az"]:
        ax[1].plot(df["t_s"], df[c], label=c)
    ax[1].set_ylabel("Acel (m/s²)"); ax[1].legend()
    for c in ["gx", "gy", "gz"]:
        ax[2].plot(df["t_s"], df[c], label=c)
    ax[2].set_ylabel("Giro (rad/s)"); ax[2].set_xlabel("Tiempo (s)"); ax[2].legend()
    fig.suptitle(f"Sesión {df['sesion'].iloc[0]}  —  label: {df['label'].iloc[0]}")
    plt.tight_layout(); plt.show()


def unir_sesiones(carpeta):
    archivos = sorted(p for p in carpeta.glob("*.csv") if p.name != "dataset.csv")
    if not archivos:
        sys.exit(f"No hay sesiones en {carpeta}/")
    df = pd.concat((pd.read_csv(a) for a in archivos), ignore_index=True)
    destino = carpeta / "dataset.csv"
    df.to_csv(destino, index=False)
    print(f"{len(archivos)} sesiones -> {destino} ({len(df)} filas)")
    print(df.groupby(["sujeto", "label"]).size().rename("muestras").to_string())


# ----------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="Captura ESP32-S3 MAX30102+MPU6050")
    ap.add_argument("--puerto", help="COM3, /dev/ttyACM0... (auto si se omite)")
    ap.add_argument("--label", help="Etiqueta de la sesión (ej. reposo, caminar, estres)")
    ap.add_argument("--sujeto", default="s01", help="ID de la persona")
    ap.add_argument("--duracion", type=float, default=60, help="Segundos a grabar")
    ap.add_argument("--salida", default="datos", help="Carpeta de salida")
    ap.add_argument("--graficar", action="store_true", help="Muestra las señales al terminar")
    ap.add_argument("--listar", action="store_true", help="Lista puertos serie y sale")
    ap.add_argument("--unir", action="store_true", help="Une todas las sesiones en dataset.csv")
    args = ap.parse_args()

    carpeta = Path(args.salida)
    carpeta.mkdir(exist_ok=True)

    if args.listar:
        for p in list_ports.comports():
            print(f"{p.device:15} {p.description}  VID={p.vid}")
        return
    if args.unir:
        unir_sesiones(carpeta)
        return
    if not args.label:
        ap.error("--label es obligatorio para grabar")

    puerto = args.puerto or detectar_puerto()
    if not puerto:
        sys.exit("No encontré el ESP32. Usa --listar y luego --puerto.")
    print(f"Usando {puerto}")

    with abrir_serial(puerto) as ser:
        filas, descartadas = capturar(ser, args.duracion)

    if not filas:
        sys.exit("No se recibieron datos.")

    sesion = datetime.now().strftime("%Y%m%d_%H%M%S")
    df = construir_df(filas, args.label, args.sujeto, sesion)
    archivo = carpeta / f"{args.sujeto}_{args.label}_{sesion}.csv"
    df.to_csv(archivo, index=False)
    print(f"Guardado: {archivo}")
    reporte_calidad(df, descartadas)

    if args.graficar:
        graficar(df)


if __name__ == "__main__":
    main()
