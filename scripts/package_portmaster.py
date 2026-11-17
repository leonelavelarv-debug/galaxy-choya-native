#!/usr/bin/env python3
"""Arma el paquete PortMaster de galaxy-choya-native.

  python3 scripts/package_portmaster.py --binary build/aarch64/macio/native/app/Petari --out out/portmaster

Estructura (como en la microSD):
  SD1/ROMS/ports/Galaxy-Choya.sh
  SD1/ports/galaxy-choya/galaxy-choya          ejecutable ELF aarch64 (con .symtab: no se quita, ver host_allocation)
  SD1/ports/galaxy-choya/galaxy-choya.gptk     salida limpia con gptokeyb
  SD1/ports/galaxy-choya/port.json, README.md, LICENSES/
  SD1/ports/galaxy-choya/conf/ cache/ saves/   configuración, caché y guardados
  SD1/ports/galaxy-choya/gamedata/RMGE01/      AQUÍ el usuario pone su disco extraído (no se incluye)
  SD1/ports/galaxy-choya/assets.wlpk           opcional: texturas convertidas con Choya (--pack)

El script comprueba que el ejecutable sea ELF aarch64 y lista las versiones de GLIBC que pide, para compararlas con
la consola. Con --allow-host acepta un binario de PC solo para revisar la estructura (el zip queda marcado NO-CONSOLA).
No copia nunca datos del juego.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_EXT = (".iso", ".rvz", ".wbfs", ".arc", ".szs", ".bdl", ".bmd", ".brres", ".thp", ".aw", ".bms")


def elf_machine(path: str) -> str:
    with open(path, "rb") as f:
        head = f.read(20)
    if head[:4] != b"\x7fELF":
        return "no-ELF"
    machine = struct.unpack_from("<H", head, 18)[0]
    return {0xB7: "aarch64", 0x3E: "x86-64"}.get(machine, hex(machine))


def glibc_versions(path: str) -> list[str]:
    try:
        out = subprocess.run(["objdump", "-T", path], capture_output=True, text=True).stdout
    except FileNotFoundError:
        data = open(path, "rb").read()
        return sorted({m.decode() for m in re.findall(rb"GLIBC_[0-9.]+", data)})
    return sorted(set(re.findall(r"GLIBC_[0-9.]+", out)), key=lambda v: [int(x) for x in v[6:].split(".")])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "portmaster"))
    ap.add_argument("--pack", help="assets.wlpk convertido por Choya (opcional)")
    ap.add_argument("--allow-host", action="store_true", help="aceptar un binario de PC (solo para revisar)")
    a = ap.parse_args()

    arch = elf_machine(a.binary)
    if arch != "aarch64" and not a.allow_host:
        print(f"ERROR: {a.binary} es {arch}, no aarch64. Compila con scripts/build.sh aarch64.", file=sys.stderr)
        return 1
    if a.pack and os.path.basename(a.pack) != "assets.wlpk":
        print("ERROR: --pack debe ser el assets.wlpk que genera `python -m choya convert`", file=sys.stderr)
        return 1

    sd = os.path.join(a.out, "SD1")
    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    port = os.path.join(sd, "ports", "galaxy-choya")
    roms = os.path.join(sd, "ROMS", "ports")
    for d in (port, roms, os.path.join(port, "conf"), os.path.join(port, "cache"), os.path.join(port, "saves"),
              os.path.join(port, "gamedata", "RMGE01"), os.path.join(port, "LICENSES")):
        os.makedirs(d, exist_ok=True)
    pm = os.path.join(ROOT, "portmaster")
    shutil.copy2(os.path.join(pm, "Galaxy-Choya.sh"), roms)
    os.chmod(os.path.join(roms, "Galaxy-Choya.sh"), 0o755)
    for name in ("galaxy-choya.gptk", "port.json", "README.md", "controls-rg35xxh.txt"):
        shutil.copy2(os.path.join(pm, name), port)
    shutil.copy2(os.path.join(pm, "gamedata-README.txt"), os.path.join(port, "gamedata", "README.txt"))
    with open(os.path.join(port, "conf", "renderer.txt"), "w") as f:
        f.write("gx\n")
    exe = os.path.join(port, "galaxy-choya")
    shutil.copy2(a.binary, exe)
    os.chmod(exe, 0o755)
    if a.pack:
        shutil.copy2(a.pack, os.path.join(port, "assets.wlpk"))
    # Licencias y atribuciones.
    lic = os.path.join(port, "LICENSES")
    shutil.copy2(os.path.join(ROOT, "THIRD_PARTY.md"), lic)
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(lic, "LICENSE-galaxy-choya-native"))
    for dep, files in (("MacioGalaxy", ["LICENSE"]), ("aurora", ["LICENSE"]),
                       ("choya", ["choya/data/aurora/sin_dawn/LICENSE-dawn", "choya/data/LICENSE-ogws-symbols.txt"])):
        for rel in files:
            src = os.path.join(ROOT, "work", dep, rel)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(lic, f"{dep}-{os.path.basename(rel)}"))
    # Ningún dato del juego dentro del paquete.
    for dp, _dn, fns in os.walk(a.out):
        for fn in fns:
            if fn.lower().endswith(GAME_EXT):
                print(f"ERROR: archivo de juego en el paquete: {os.path.join(dp, fn)}", file=sys.stderr)
                return 1
    manifest = {"binario": os.path.relpath(exe, a.out), "arquitectura": arch, "glibc": glibc_versions(exe),
                "assets_wlpk": bool(a.pack), "consola": arch == "aarch64"}
    with open(os.path.join(port, "build-info.json"), "w") as f:
        json.dump(manifest, f, indent=1, ensure_ascii=False)
    tag = "" if arch == "aarch64" else "-NO-CONSOLA"
    zpath = os.path.join(a.out, f"galaxy-choya{tag}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for dp, _dn, fns in os.walk(sd):
            for fn in fns:
                full = os.path.join(dp, fn)
                info = zipfile.ZipInfo.from_file(full, os.path.relpath(full, sd))
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(full, "rb") as fh:
                    z.writestr(info, fh.read())
        # Carpetas vacías que el port necesita.
        for empty in ("ports/galaxy-choya/cache/", "ports/galaxy-choya/saves/"):
            z.writestr(empty, "")
    print(json.dumps(manifest, ensure_ascii=False))
    print(f"paquete: {zpath}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
