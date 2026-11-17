#!/usr/bin/env python3
"""Clona las dependencias fijadas en deps/lock.json y prepara copias de trabajo con los parches aplicados.

  third_party/<nombre>  clon limpio en el commit fijado (no se modifica nunca)
  work/<nombre>         copia de trabajo (git worktree) con patches/<nombre>/*.patch aplicados en orden

Cada parche se comprueba con `git apply --check` antes de aplicarse; si alguno no encaja, el script se detiene y
dice cuál y por qué. Los parches ya aplicados se detectan con `git apply --reverse --check` (idempotente).
El resultado queda en work/patches-status.json.

Uso:
  python3 scripts/fetch_deps.py            # clona lo que falte y aplica parches
  python3 scripts/fetch_deps.py --verify   # solo comprueba commits y estado de parches (sin red si ya está clonado)
  python3 scripts/fetch_deps.py --only MacioGalaxy aurora
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, "deps", "lock.json")
THIRD = os.path.join(ROOT, "third_party")
WORK = os.path.join(ROOT, "work")
PATCHES = os.path.join(ROOT, "patches")


class Fallo(Exception):
    pass


def git(*args: str, cwd: str | None = None, check: bool = True, capture: bool = True) -> subprocess.CompletedProcess:
    r = subprocess.run(["git", *args], cwd=cwd, text=True,
                       stdout=subprocess.PIPE if capture else None, stderr=subprocess.PIPE if capture else None)
    if check and r.returncode != 0:
        raise Fallo(f"git {' '.join(args)} (en {cwd or '.'}) falló:\n{r.stderr.strip()}")
    return r


def clonar(nombre: str, d: dict, verify: bool) -> str:
    dst = os.path.join(THIRD, nombre)
    commit = d["commit"]
    if not os.path.isdir(os.path.join(dst, ".git")):
        if verify:
            raise Fallo(f"{nombre}: falta third_party/{nombre} (ejecuta sin --verify)")
        os.makedirs(dst, exist_ok=True)
        git("init", "-q", cwd=dst)
        git("remote", "add", "origin", d["repo"], cwd=dst)
        if d.get("sparse"):
            git("config", "core.sparseCheckout", "true", cwd=dst)
            with open(os.path.join(dst, ".git", "info", "sparse-checkout"), "w") as f:
                f.write("\n".join(d["sparse"]) + "\n")
            git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", d.get("tag", commit), cwd=dst)
        else:
            git("fetch", "-q", "--depth", "1", "origin", commit, cwd=dst)
        git("checkout", "-q", "FETCH_HEAD", cwd=dst)
    head = git("rev-parse", "HEAD", cwd=dst).stdout.strip()
    if head != commit:
        raise Fallo(f"{nombre}: third_party está en {head}, el lock pide {commit}")
    sucio = git("status", "--porcelain", cwd=dst).stdout.strip()
    if sucio:
        raise Fallo(f"{nombre}: third_party/{nombre} tiene cambios locales; debe quedar limpio:\n{sucio[:500]}")
    return dst


def parches_de(nombre: str) -> list[str]:
    d = os.path.join(PATCHES, nombre)
    if not os.path.isdir(d):
        return []
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".patch"))


def _sha(path: str) -> str:
    import hashlib
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def preparar_trabajo(nombre: str, src: str, commit: str, verify: bool) -> dict:
    """Aplica los parches en orden. work/<nombre>/.galaxy-parches guarda el sha256 de cada parche ya aplicado: si un
    parche aplicado cambió o se quitó, la copia de trabajo ya no corresponde y hay que rehacerla (se dice cómo)."""
    dst = os.path.join(WORK, nombre)
    if not os.path.isdir(dst):
        if verify:
            raise Fallo(f"{nombre}: falta work/{nombre}")
        os.makedirs(WORK, exist_ok=True)
        git("worktree", "prune", cwd=src)
        git("worktree", "add", "-q", "--detach", dst, commit, cwd=src)
        with open(os.path.join(src, ".git", "info", "exclude"), "a") as f:
            f.write(".galaxy-parches\n")
    sello = os.path.join(dst, ".galaxy-parches")
    hechos: list[str] = []
    if os.path.isfile(sello):
        with open(sello) as f:
            hechos = [l.split()[0] for l in f if l.strip()]
    lista = parches_de(nombre)
    shas = [_sha(p) for p in lista]
    if hechos != shas[:len(hechos)]:
        raise Fallo(f"{nombre}: work/{nombre} tiene parches que ya no coinciden con patches/{nombre}. Rehazla:\n"
                    f"  rm -rf work/{nombre} && git -C third_party/{nombre} worktree prune && python3 scripts/fetch_deps.py")
    estado = {}
    for i, p in enumerate(lista):
        rel = os.path.relpath(p, ROOT)
        if i < len(hechos):
            estado[rel] = "ya_aplicado"
            continue
        chk = git("apply", "--check", "--whitespace=nowarn", p, cwd=dst, check=False)
        if chk.returncode != 0:
            raise Fallo(f"PARCHE OBLIGATORIO NO APLICABLE: {rel}\n{chk.stderr.strip()}\n"
                        f"El destino cambió o el commit fijado no coincide. No se continúa con un árbol a medias.")
        if verify:
            estado[rel] = "pendiente"
            continue
        git("apply", "--whitespace=nowarn", p, cwd=dst)
        with open(sello, "a") as f:
            f.write(f"{shas[i]} {os.path.basename(p)}\n")
        estado[rel] = "aplicado"
    return estado


def arbol_real(nombre: str, src: str, commit: str) -> None:
    """--verify: el contenido de work/<nombre> (incluidos cambios sin commit y archivos nuevos) debe ser exactamente el
    commit fijado más los parches de patches/<nombre> en orden. Se compara el árbol git de ambos, con índices
    temporales: no se toca el índice ni los archivos de la copia de trabajo."""
    import tempfile
    dst = os.path.join(WORK, nombre)
    with tempfile.TemporaryDirectory() as tmp:
        env_real = dict(os.environ, GIT_INDEX_FILE=os.path.join(tmp, "real"))
        env_esp = dict(os.environ, GIT_INDEX_FILE=os.path.join(tmp, "esperado"))
        def run(args, cwd, env):
            r = subprocess.run(["git", *args], cwd=cwd, env=env, capture_output=True, text=True)
            if r.returncode != 0:
                raise Fallo(f"{nombre}: git {' '.join(args)}: {r.stderr.strip()}")
            return r.stdout.strip()
        run(["read-tree", "HEAD"], dst, env_real)
        run(["add", "-A", "."], dst, env_real)
        real = run(["write-tree"], dst, env_real)
        run(["read-tree", commit], dst, env_esp)
        for parche in parches_de(nombre):
            run(["apply", "--cached", "--whitespace=nowarn", parche], dst, env_esp)
        esperado = run(["write-tree"], dst, env_esp)
        if real != esperado:
            difs = run(["diff-tree", "-r", "--name-only", esperado, real], dst, env_real).splitlines()
            raise Fallo(f"{nombre}: work/{nombre} no es el commit fijado + sus parches ({len(difs)} archivo(s) "
                        f"distintos, p. ej. {', '.join(difs[:5])}). Rehazla: rm -rf work/{nombre} && "
                        f"git -C third_party/{nombre} worktree prune && python3 scripts/fetch_deps.py")


def arbol_sha(raiz: str) -> str:
    """sha256 de un árbol de archivos (rutas relativas ordenadas + sha256 de cada archivo)."""
    import hashlib
    h = hashlib.sha256()
    for base, dirs, files in os.walk(raiz):
        dirs.sort()
        for n in sorted(files):
            ruta = os.path.join(base, n)
            h.update(os.path.relpath(ruta, raiz).encode() + b"\0" + _sha(ruta).encode() + b"\n")
    return h.hexdigest()


def cabeceras_dawn(d: dict, verify: bool, paquete: str | None) -> str:
    """Cabeceras de la versión de Dawn que fija Aurora (para el WebGPU nulo de Choya; no se usa Dawn). Se extraen del
    paquete precompilado fijado (cualquier arquitectura: las cabeceras son iguales) y se comprueba su hash."""
    import tarfile
    destino = os.path.join(THIRD, "dawn-headers")
    inc = os.path.join(destino, "include")
    if not os.path.isdir(inc):
        if verify:
            raise Fallo("dawn-headers: falta third_party/dawn-headers (python3 scripts/fetch_deps.py --dawn-headers)")
        os.makedirs(destino, exist_ok=True)
        if paquete and os.path.isdir(os.path.join(paquete, "include")):
            import shutil
            shutil.copytree(os.path.join(paquete, "include"), inc)
        else:
            import urllib.request
            arch = d["urls"]["aarch64"]
            tgz = os.path.join(destino, "dawn.tar.gz")
            print(f"[deps] dawn-headers: descargando {arch}")
            urllib.request.urlretrieve(arch, tgz)
            if _sha(tgz) != d["sha256"]["aarch64"]:
                raise Fallo(f"dawn-headers: {tgz} no coincide con el sha256 del lock")
            with tarfile.open(tgz) as t:
                miembros = [m for m in t.getmembers() if "/include/" in "/" + m.name and m.isfile()]
                for m in miembros:
                    rel = m.name.split("include/", 1)[1]
                    os.makedirs(os.path.dirname(os.path.join(inc, rel)), exist_ok=True)
                    with t.extractfile(m) as fsrc, open(os.path.join(inc, rel), "wb") as fdst:
                        fdst.write(fsrc.read())
            os.remove(tgz)
    sha = arbol_sha(inc)
    if sha != d["tree_sha256"]:
        raise Fallo(f"dawn-headers: el árbol de cabeceras ({sha}) no es el del lock ({d['tree_sha256']})")
    return sha


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--aurora-extern", action="store_true", help="clonar también las dependencias de Aurora")
    ap.add_argument("--dawn-headers", action="store_true",
                    help="extraer las cabeceras de Dawn fijadas (WebGPU nulo para la consola)")
    ap.add_argument("--dawn-package-dir", help="paquete de Dawn ya extraído (sin red); si no, se descarga el fijado")
    a = ap.parse_args()
    with open(LOCK) as f:
        lock = json.load(f)["dependencias"]
    resumen = {}
    try:
        for nombre, d in lock.items():
            if a.only and nombre not in a.only:
                continue
            src = clonar(nombre, d, a.verify)
            if d.get("sparse") and not parches_de(nombre):
                # Sin parches: se usa el clon disperso tal cual (un worktree no hereda el sparse-checkout).
                resumen[nombre] = {"commit": d["commit"], "parches": {}, "ruta": os.path.relpath(src, ROOT)}
                print(f"[deps] {nombre}: {d['commit'][:12]} OK (sin copia de trabajo)")
                continue
            resumen[nombre] = {"commit": d["commit"], "parches": preparar_trabajo(nombre, src, d["commit"], a.verify),
                               "ruta": os.path.relpath(os.path.join(WORK, nombre), ROOT)}
            if a.verify:
                arbol_real(nombre, src, d["commit"])  # contenido real, no solo el registro de aplicación
                resumen[nombre]["arbol"] = "verificado"
            n = len(resumen[nombre]["parches"])
            print(f"[deps] {nombre}: {d['commit'][:12]} OK, {n} parche(s)")
    except Fallo as e:
        print(f"[deps] ERROR: {e}", file=sys.stderr)
        return 1
    # Dependencias de Aurora (solo con --aurora-extern o si ya están clonadas: no hacen falta sin GALAXY_GX).
    ext = json.load(open(LOCK)).get("aurora_extern", {}).get("dependencias", {})
    extdir = os.path.join(THIRD, "aurora-extern")
    try:
        for nombre, d in ext.items():
            dst = os.path.join(extdir, nombre)
            if not os.path.isdir(os.path.join(dst, ".git")):
                if not a.aurora_extern:
                    continue
                if a.verify:
                    raise Fallo(f"aurora-extern/{nombre}: falta")
                os.makedirs(dst, exist_ok=True)
                git("init", "-q", cwd=dst)
                git("remote", "add", "origin", d["repo"], cwd=dst)
                git("fetch", "-q", "--depth", "1", "origin", d["ref"], cwd=dst)
                git("checkout", "-q", "FETCH_HEAD", cwd=dst)
            head = git("rev-parse", "HEAD", cwd=dst).stdout.strip()
            if head != d["commit"]:
                raise Fallo(f"aurora-extern/{nombre} está en {head}, el lock pide {d['commit']}")
            resumen.setdefault("aurora_extern", {})[nombre] = d["commit"]
        if resumen.get("aurora_extern"):
            print(f"[deps] aurora-extern: {len(resumen['aurora_extern'])} dependencia(s) OK")
    except Fallo as e:
        print(f"[deps] ERROR: {e}", file=sys.stderr)
        return 1
    dh = json.load(open(LOCK)).get("dawn_headers")
    if dh and (a.dawn_headers or os.path.isdir(os.path.join(THIRD, "dawn-headers"))):
        try:
            resumen["dawn_headers"] = cabeceras_dawn(dh, a.verify, a.dawn_package_dir)
            print(f"[deps] dawn-headers: {dh['version']} OK")
        except Fallo as e:
            print(f"[deps] ERROR: {e}", file=sys.stderr)
            return 1
    os.makedirs(WORK, exist_ok=True)
    with open(os.path.join(WORK, "patches-status.json"), "w") as f:
        json.dump(resumen, f, indent=1, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
