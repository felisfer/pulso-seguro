#!/usr/bin/env python3
"""Descarga los lotes de S3 y los convierte en un DataFrame de pandas."""
from concurrent.futures import ThreadPoolExecutor
import boto3
import pandas as pd
import os

BUCKET = os.environ["S3_BUCKET"]
PREFIJO = "raw/"                    # ej. "raw/esp32s3-01/2026/11/10/" para un solo dia
COLUMNAS = ["seq", "t_ms", "red", "ir", "ax", "ay", "az", "gx", "gy", "gz"]
FS = 100

s3 = boto3.client("s3")             # los clientes de boto3 se pueden usar entre hilos

def listar(bucket, prefijo):
    claves = []
    for pagina in s3.get_paginator("list_objects_v2").paginate(Bucket=bucket, Prefix=prefijo):
        claves += [o["Key"] for o in pagina.get("Contents", [])]
    return claves

def descargar(clave):
    return s3.get_object(Bucket=BUCKET, Key=clave)["Body"].read().decode()

def parsear(texto):
    sesion = label = sujeto = None
    for linea in texto.splitlines():
        linea = linea.strip()
        if not linea:
            continue
        if linea.startswith("#"):                 # encabezado de lote
            sesion, label, sujeto = (linea[1:].split(",") + ["", "", ""])[:3]
            continue
        partes = linea.split(",")
        if len(partes) == len(COLUMNAS) and sesion:
            yield [float(p) for p in partes] + [sesion, label, sujeto]

def construir(textos):
    filas = [f for texto in textos for f in parsear(texto)]
    df = pd.DataFrame(filas, columns=COLUMNAS + ["sesion", "label", "sujeto"])
    df[["seq", "t_ms", "red", "ir"]] = df[["seq", "t_ms", "red", "ir"]].astype("int64")
    # el orden de los archivos no importa y un reintento puede duplicar un lote
    df = (df.drop_duplicates(["sesion", "seq"])
            .sort_values(["sesion", "seq"])
            .reset_index(drop=True))
    df["t_s"] = df["seq"] / FS
    return df

def resumen(df):
    huecos = df.groupby("sesion")["seq"].agg(lambda s: int(s.max() - s.min() + 1 - len(s)))
    r = df.groupby(["sesion", "label", "sujeto"]).size().rename("muestras").reset_index()
    r["perdidas"] = r["sesion"].map(huecos)
    return r

if __name__ == "__main__":
    claves = listar(BUCKET, PREFIJO)
    print(f"{len(claves)} archivos en s3://{BUCKET}/{PREFIJO}")
    with ThreadPoolExecutor(max_workers=16) as ex:
        textos = list(ex.map(descargar, claves))
    df = construir(textos)
    print(resumen(df).to_string(index=False))
    df.to_csv("dataset.csv", index=False)
    print(f"{len(df)} filas -> dataset.csv")
