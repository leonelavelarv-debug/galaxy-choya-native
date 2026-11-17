#!/usr/bin/env python3
"""WebGPU nulo de Choya para la Aurora única (sin Dawn en la consola).

  generate.py <sin_dawn de Choya> <cabeceras de Dawn fijadas> <salida>

Copia la parte escrita a mano (webgpu_nulo.cpp, del commit fijado de Choya) y genera el resto
(webgpu_nulo_tipos.inc, webgpu_nulo_resto.inc) con scripts/generar-webgpu-nulo.py de Choya contra las cabeceras de
la versión de Dawn que fija Aurora 08122911 (v20260807.225922). No modifica nada fuera de <salida>.
"""
import importlib.util
import os
import shutil
import sys


def main() -> int:
    sin_dawn, headers, out = (os.path.abspath(p) for p in sys.argv[1:4])
    gen = os.path.join(sin_dawn, "..", "..", "..", "..", "scripts", "generar-webgpu-nulo.py")
    if not os.path.isfile(gen):
        raise SystemExit(f"generate.py: falta el generador de Choya ({gen})")
    header = os.path.join(headers, "dawn", "webgpu.h")
    if not os.path.isfile(header):
        raise SystemExit(f"generate.py: faltan las cabeceras de Dawn fijadas ({header}); python3 scripts/fetch_deps.py")
    os.makedirs(out, exist_ok=True)
    for name in ("webgpu_nulo.cpp", "LICENSE-dawn"):
        shutil.copyfile(os.path.join(sin_dawn, name), os.path.join(out, name))
    spec = importlib.util.spec_from_file_location("generar_webgpu_nulo", gen)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.RAIZ = out
    mod.CABECERA = header
    mod.A_MANO = os.path.join(out, "webgpu_nulo.cpp")
    return mod.main()


if __name__ == "__main__":
    sys.exit(main())
