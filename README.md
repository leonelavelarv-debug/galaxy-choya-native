# galaxy-choya-native

Port nativo de **Super Mario Galaxy** para la **Anbernic RG35XX H** (Linux aarch64, KNULLI/muOS, PortMaster).

- La lógica es la decompilación **Petari** ya adaptada a 64 bits por **MacioGalaxy**, compilada a instrucciones
  ARM64 (Cortex-A53). No hay emulador, intérprete PowerPC, recompilación dinámica ni DOL.
- Dibuja con el **motor GLES de Choya** (`choya_gx_core`), primero detrás de Aurora (fase 1, órdenes GX) y después
  con una **API directa** que recibe la geometría y los materiales de J3D sin FIFO GX (fase 2).
- No incluye datos del juego: necesitas tu propio disco (RMGE01) extraído.

> **Estado honesto (2026-11-16):** hay base Linux reproducible, juego compilado en Linux, motor GLES y ruta directa
> funcionando en pruebas con Mesa. **No es un port jugable todavía** y no se ha ejecutado en la RG35XX H. Ver
> [docs/BLOQUEOS.md](docs/BLOQUEOS.md) y [docs/MEDICIONES.md](docs/MEDICIONES.md).

## Hitos

| Hito | Estado |
| --- | --- |
| A. Base Linux reproducible | **hecho en PC x86-64**: dependencias fijadas, 5 parches verificados, núcleo + servicios + 1641 archivos del juego compilan, 59/59 pruebas de MacioGalaxy pasan. aarch64: toolchain y contenedor listos, sin ejecutar aquí |
| B. Modelo real con Choya (GX) | **app completa (juego + Aurora) enlaza en Linux x86-64 y arranca hasta pedir el disco**; falta el puente Aurora 08122911 → Choya (B1), el WebGPU nulo para la consola (B2) y un disco para probar |
| C. Arranque e interacción | pendiente de B |
| D. Escena jugable | pendiente |
| E. Ciclo de vida completo | pendiente |
| F. PortMaster y medición | lanzador, perfil de mando y empaquetador hechos; sin binario aarch64 ni medición en consola |
| G. Ruta directa | **API implementada y probada** (0 píxeles distintos frente a la captura GX en Mesa), conversión de listas GX y puente J3D compilados, selector `GALAXY_RENDERER` |
| H. Comparación | pendiente (necesita B y la consola) |

## Estructura

```
deps/lock.json            commits fijados (MacioGalaxy, Choya, Aurora, libc++, dependencias de Aurora)
patches/MacioGalaxy/      parches versionados (se aplican y verifican con scripts/fetch_deps.py)
compat/darwin/            capa Darwin → Linux (Mach, libdispatch, QoS, dladdr con .symtab) con implementación real
cmake/linux_port.cmake    se inyecta en el CMake de MacioGalaxy (CMAKE_PROJECT_PetariNative_INCLUDE)
cmake/toolchains/         aarch64-linux-gnu (clang + lld, -mcpu=cortex-a53)
direct/                   API directa (choya_direct), listas GX → mallas, puente J3D, selector de backend
tests/                    pruebas propias (Mesa, sin GPU, scripts)
portmaster/               lanzador Galaxy-Choya.sh, perfil de mando, port.json, README del port
scripts/                  fetch_deps.py, build.sh, build_libcxx.sh, package_portmaster.py, fix_include_case.py
docker/Dockerfile.aarch64 Debian 12 (glibc 2.36) para compilar el binario de la consola
docs/                     INSPECCION, ARQUITECTURA, MIGRACION, BLOQUEOS, MEDICIONES
```

`third_party/` (clones limpios) y `work/` (copias de trabajo con parches) se crean al compilar y no se versionan.

## Compilar y probar en una PC Linux

Requisitos: git, python3, cmake ≥ 3.25, ninja, clang 18 (o 16+), Mesa con EGL/GLES (para las pruebas gráficas).

```sh
git clone <este repositorio> galaxy-choya-native && cd galaxy-choya-native
python3 scripts/fetch_deps.py                 # clona en los commits fijados y aplica los parches (falla si no encajan)
sh scripts/build.sh host                      # libc++ estática + MacioGalaxy + motor de Choya + ruta directa
ctest --test-dir build/host/macio -LE "timing|slow"
ctest --test-dir build/host/choya             # motor de Choya, ruta directa, selector, lanzador, parches
```

Fase 1 con Aurora (descarga el paquete precompilado de Dawn para la PC):

```sh
GALAXY_GX=ON sh scripts/build.sh host
```

## Compilar para la consola (aarch64)

```sh
docker build -t galaxy-choya-aarch64 -f docker/Dockerfile.aarch64 docker
docker run --rm -v "$PWD":/src -w /src galaxy-choya-aarch64 sh scripts/build.sh aarch64
python3 scripts/package_portmaster.py --binary build/aarch64/macio/native/app/Petari --out out/portmaster
```

En la PC con Windows 11: lo mismo desde WSL2 (Ubuntu) con Docker Desktop encendido. El paquete comprueba que el
binario sea ELF aarch64 y lista las versiones de GLIBC que pide (`build-info.json`): compáralas con `ldd --version` en
la consola.

## Instalar (PortMaster)

```
SD1/ROMS/ports/Galaxy-Choya.sh
SD1/ports/galaxy-choya/                 ejecutable, conf/, cache/, saves/, LICENSES/
SD1/ports/galaxy-choya/gamedata/RMGE01/ tu disco extraído (files/ y sys/)
```

`log.txt` queda en `ports/galaxy-choya/` con todo, incluidos los errores de arranque.

## Variables nuevas de este proyecto

| Variable | Valores |
| --- | --- |
| `GALAXY_RENDERER` | `gx` (por defecto) o `direct` |
| `GALAXY_DIRECT_SCENES` | escenas extra para `direct`, separadas por comas (sin validar) |
| `GALAXY_DIRECT_FORCE` | `1`: `direct` en todas las escenas (solo para medir) |
| `GALAXY_GX` (compilación) | `ON` para compilar la fase 1 con Aurora |

También se respetan las de MacioGalaxy (`PETARI_*`) y las del motor de Choya (`CHOYA_*`, p. ej. `CHOYA_PAQUETE`).

## Decisiones técnicas

- **libc++ estática**: MacioGalaxy exige libc++; estática evita depender de la libstdc++ del firmware. El desenrollado
  usa libgcc_s (el de glibc): mezclarlo con libunwind de LLVM rompía `pthread_exit` (comprobado).
- **Capa Darwin → Linux en vez de reescribir**: el planificador, el audio y el VI de MacioGalaxy siguen idénticos; las
  llamadas de Mach se implementan en Linux (suspensión real por señal, `/proc`, semáforos POSIX, `SCHED_FIFO`).
- **Aurora única**: la de MacioGalaxy, porque sus parches de sincronía, EFB y memoria son los que Galaxy necesita.
- **Sin `-ffast-math`**, `-O2`, `-mcpu=cortex-a53`; LTO y PGO como opciones (`GALAXY_LTO`, `GALAXY_PGO_*`).

Atribuciones y licencias: [THIRD_PARTY.md](THIRD_PARTY.md). Código propio: MIT ([LICENSE](LICENSE)).
