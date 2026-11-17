#!/usr/bin/env python3
"""Comprueba el ejecutable distribuible antes de empaquetarlo.

  check_elf.py --arch aarch64 --glibc-max 2.36 build/aarch64/app/native/app/Petari

- Arquitectura (e_machine) y cargador (PT_INTERP) de la arquitectura pedida.
- DT_NEEDED: solo bibliotecas del sistema base (libc, libm, libdl, libpthread, librt, libgcc_s, el cargador).
  EGL/GLES se cargan con dlopen (host de pantalla) y libc++ va estática: si aparecen libstdc++, libEGL, libGLESv2,
  libSDL3, libwebgpu_dawn o rutas de la PC, falla.
- Versiones GLIBC pedidas: la mayor no puede pasar de --glibc-max (la del firmware: `ldd --version` en la consola).
Usa readelf (binutils) o llvm-readelf. Sale con 1 y dice qué falla.
"""
import argparse
import re
import shutil
import subprocess
import sys

INTERP = {"aarch64": "/lib/ld-linux-aarch64.so.1", "x86_64": "/lib64/ld-linux-x86-64.so.2"}
MACHINE = {"aarch64": "AArch64", "x86_64": "Advanced Micro Devices X86-64"}
ALLOWED = re.compile(r"^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|libgcc_s\.so\.1|"
                     r"ld-linux-aarch64\.so\.1|ld-linux-x86-64\.so\.2)$")


def readelf() -> str:
    for name in ("readelf", "llvm-readelf", "llvm-readelf-18", "llvm-readelf-16"):
        if shutil.which(name):
            return name
    raise SystemExit("check_elf: falta readelf (binutils) o llvm-readelf")


def ver(v: str) -> tuple:
    return tuple(int(x) for x in v.split("."))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", required=True, choices=sorted(INTERP))
    ap.add_argument("--glibc-max", required=True)
    ap.add_argument("--allow", default="", help="bibliotecas extra que el port lleva en libs/ (separadas por comas)")
    ap.add_argument("binary")
    a = ap.parse_args()
    tool = readelf()
    out = subprocess.run([tool, "-h", "-l", "-d", "-V", "-W", a.binary], capture_output=True, text=True, check=True).stdout
    fails = []
    m = re.search(r"Machine:\s+(.+)", out)
    if not m or MACHINE[a.arch] not in m.group(1):
        fails.append(f"arquitectura {m.group(1).strip() if m else '?'} (se esperaba {MACHINE[a.arch]})")
    m = re.search(r"Requesting program interpreter: ([^\]]+)\]", out)
    if not m or m.group(1) != INTERP[a.arch]:
        fails.append(f"cargador {m.group(1) if m else 'ninguno'} (se esperaba {INTERP[a.arch]})")
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)
    extra = {x.strip() for x in a.allow.split(",") if x.strip()}
    bad = [n for n in needed if not ALLOWED.match(n) and n not in extra]
    if bad:
        fails.append("DT_NEEDED fuera del sistema base: " + ", ".join(bad))
    if re.search(r"\((RPATH|RUNPATH)\)", out):
        fails.append("tiene RPATH/RUNPATH (rutas de la máquina de compilación)")
    glibc = sorted({v for v in re.findall(r"GLIBC_(\d+\.\d+(?:\.\d+)?)", out)}, key=ver)
    top = glibc[-1] if glibc else "?"
    if glibc and ver(top) > ver(a.glibc_max):
        fails.append(f"pide GLIBC_{top} (> {a.glibc_max}); símbolos: " +
                     ", ".join(sorted(set(re.findall(r"Name: GLIBC_" + re.escape(top), out)))))
    print(f"[elf] {a.binary}: {a.arch}, cargador {m.group(1) if m else '?'}, DT_NEEDED {', '.join(needed)}, "
          f"GLIBC máx {top} (límite {a.glibc_max})")
    for f in fails:
        print(f"[elf] FALLA: {f}", file=sys.stderr)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
