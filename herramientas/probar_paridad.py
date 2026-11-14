"""
probar_paridad.py · Comprueba que el ESP32 (features_fc.h) y Python (features.py)
calculan las MISMAS features. Compila paridad.cpp en tu PC y compara ventana por ventana.

    python herramientas/probar_paridad.py dataset.csv

Compilador: usa el primero que encuentre, en este orden:
    g++  →  clang++  →  cl (MSVC, desde "Developer PowerShell")  →  ziglang (pip)
En Windows sin nada instalado, lo más fácil es:
    pip install ziglang
(un compilador C/C++ completo empaquetado como paquete de Python; no toca el PATH).
Fuerza uno con  --compilador g++|clang++|cl|zig.

Criterio: misma decisión de "válida" en >= 99 % de las ventanas y
|diferencia| de fc < 0.5 lpm en las ventanas válidas en ambos.
"""
import argparse
import importlib.util
import io
import os
import pathlib
import shutil
import subprocess
import sys

import pandas as pd

RAIZ = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAIZ / "modelo"))
from features import NOMBRES, extraer_sesion  # noqa: E402

for _s in (sys.stdout, sys.stderr):          # Windows: cp1252 en pipes no tiene "→", "✅"…
    _s.reconfigure(encoding="utf-8", errors="replace")

FUENTE = RAIZ / "herramientas" / "paridad.cpp"
INCLUDE = RAIZ / "firmware" / "pulso_seguro_aws"
BINARIO = RAIZ / "herramientas" / ("paridad.exe" if os.name == "nt" else "paridad")


def comando(nombre: str) -> list[str] | None:
    """Línea de compilación para cada compilador, o None si no está disponible."""
    gcc_like = ["-O2", "-std=c++17", "-I", str(INCLUDE), str(FUENTE), "-o", str(BINARIO)]
    if nombre in ("g++", "clang++") and shutil.which(nombre):
        return [nombre, *gcc_like]
    if nombre == "cl" and shutil.which("cl"):
        return ["cl", "/nologo", "/O2", "/std:c++17", "/EHsc", f"/I{INCLUDE}", str(FUENTE),
                f"/Fe{BINARIO}", f"/Fo{BINARIO.parent}{os.sep}"]
    if nombre == "zig" and importlib.util.find_spec("ziglang"):
        return [sys.executable, "-m", "ziglang", "c++", *gcc_like]
    return None


def compilar(forzado: str | None) -> None:
    candidatos = [forzado] if forzado else ["g++", "clang++", "cl", "zig"]
    for c in candidatos:
        cmd = comando(c)
        if cmd:
            print(f"Compilando con {c}…")
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode == 0:
                return
            sys.exit(f"Falló la compilación con {c}:\n{r.stdout}\n{r.stderr}")
    sys.exit("No encontré un compilador de C++.\n"
             "  Opción más simple:   pip install ziglang\n"
             "  Alternativas:        MSYS2 (g++), LLVM (clang++), Visual Studio Build Tools (cl) o WSL.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dataset")
    ap.add_argument("--compilador", choices=["g++", "clang++", "cl", "zig"])
    a = ap.parse_args()

    compilar(a.compilador)
    salida = subprocess.run([str(BINARIO), a.dataset], check=True, capture_output=True, text=True).stdout
    c = pd.read_csv(io.StringIO(salida))

    df = pd.read_csv(a.dataset)
    filas = []
    for ses, g in df.groupby("sesion", sort=False):
        for f in extraer_sesion(g.sort_values("seq")):
            filas.append({"sesion": ses, **f})
    p = pd.DataFrame(filas)
    p["valida"] = p["valida"].astype(int)
    p["sesion"] = p["sesion"].astype(str)
    c["sesion"] = c["sesion"].astype(str)

    m = p.merge(c, on=["sesion", "win"], suffixes=("_py", "_c"))
    print(f"Ventanas: Python {len(p)}, C {len(c)}, emparejadas {len(m)}")
    acuerdo = (m["valida_py"] == m["valida_c"]).mean()
    ambas = m[(m["valida_py"] == 1) & (m["valida_c"] == 1)]
    print(f"Acuerdo en 'válida': {acuerdo:.2%}  (válidas en ambos: {len(ambas)})")
    print(f"\n{'feature':<14}{'máx |dif|':>14}{'mediana |dif|':>16}")
    peor_fc = 0.0
    for n in NOMBRES:
        d = (ambas[f"{n}_py"] - ambas[f"{n}_c"]).abs()
        print(f"{n:<14}{d.max():>14.3g}{d.median():>16.3g}")
        if n == "fc":
            peor_fc = d.max() if len(d) else 0.0

    ok = len(p) == len(c) and acuerdo >= 0.99 and peor_fc < 0.5
    print("\nPARIDAD OK" if ok else "\nPARIDAD FALLA: revisa la última función que cambiaste")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
