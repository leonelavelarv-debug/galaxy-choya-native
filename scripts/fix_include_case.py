#!/usr/bin/env python3
"""Corrige #include cuyo nombre solo coincide sin distinguir mayúsculas (macOS no las distingue; Linux sí).

Busca cada #include "..." / <...> en el árbol de MacioGalaxy, lo resuelve contra el directorio del archivo y las
rutas de inclusión del proyecto, y si solo existe con otra capitalización reescribe la línea con el nombre real.
Se usa para generar patches/MacioGalaxy/0001-linux-include-case.patch y en la prueba que lo vuelve a comprobar.

  python3 scripts/fix_include_case.py work/MacioGalaxy           # informa
  python3 scripts/fix_include_case.py work/MacioGalaxy --apply   # reescribe
"""
from __future__ import annotations

import os
import re
import sys

INCLUDE_DIRS = ["include", "libs/RVL_SDK/include", "libs/JSystem/include", "libs/nw4r/include",
                "libs/RVLFaceLib/include", "libs/MSL_C/include", "libs/MSL_C++/include", "native/include",
                "native/platform/include", "native/resource", "."]
EXT = (".c", ".cpp", ".h", ".hpp", ".inc")
INC = re.compile(rb'^(\s*#\s*include\s*)([<"])([^>"]+)([>"])', re.M)


def build_index(root: str) -> dict[str, list[str]]:
    idx: dict[str, list[str]] = {}
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d not in (".git", "build")]
        for fn in fns:
            full = os.path.join(dp, fn)
            idx.setdefault(full.lower(), []).append(full)
    return idx


def resolve(root: str, src_dir: str, name: str, idx) -> tuple[str | None, str | None]:
    """(ruta exacta si existe, ruta real si solo existe con otra capitalización)."""
    bases = [src_dir] + [os.path.join(root, d) for d in INCLUDE_DIRS]
    for b in bases:
        p = os.path.normpath(os.path.join(b, name))
        if os.path.isfile(p):
            # os.path.isfile no distingue en sistemas insensibles; se comprueba con la lista real del directorio
            if os.path.basename(p) in os.listdir(os.path.dirname(p)):
                return p, None
    for b in bases:
        p = os.path.normpath(os.path.join(b, name))
        cands = idx.get(p.lower())
        if cands and len(cands) == 1:
            return None, cands[0]
    return None, None


def main() -> int:
    root = os.path.abspath(sys.argv[1])
    apply = "--apply" in sys.argv
    idx = build_index(root)
    changes = 0
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d not in (".git", "build")]
        for fn in fns:
            if not fn.endswith(EXT):
                continue
            path = os.path.join(dp, fn)
            data = open(path, "rb").read()
            new = data

            def sub(m: re.Match) -> bytes:
                nonlocal changes
                name = m.group(3).decode("latin-1")
                exact, real = resolve(root, dp, name, idx)
                if exact or not real:
                    return m.group(0)
                # Conserva la parte de la ruta que escribió el autor y corrige solo la capitalización.
                fixed = real[len(real) - len(name):]
                if fixed.lower() != name.lower():
                    return m.group(0)
                changes += 1
                print(f"{os.path.relpath(path, root)}: {name} -> {fixed}")
                return m.group(1) + m.group(2) + fixed.encode("latin-1") + m.group(4)

            new = INC.sub(sub, data)
            if apply and new != data:
                open(path, "wb").write(new)
    print(f"{changes} include(s) con mayúsculas distintas")
    return 0


if __name__ == "__main__":
    sys.exit(main())
