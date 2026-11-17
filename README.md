# galaxy-choya-native

Port nativo de **Super Mario Galaxy** para la **Anbernic RG35XX H** (Linux aarch64, KNULLI/muOS, PortMaster).

- La lógica es la decompilación **Petari** ya adaptada a 64 bits por **MacioGalaxy**, compilada a instrucciones
  ARM64 (Cortex-A53). No hay emulador, intérprete PowerPC, recompilación dinámica ni DOL.
- Dibuja con el **motor GLES de Choya** (`choya_gx_core`), primero detrás de Aurora (fase 1, órdenes GX) y después
  con una **API directa** que recibe la geometría y los materiales de J3D sin FIFO GX (fase 2).
- No incluye datos del juego: necesitas tu propio disco (RMGE01) extraído.

> **Estado honesto (2026-11-17):** base Linux reproducible, juego compilado en Linux, puente Aurora → Choya,
> **host de pantalla** (hilo GLES que dibuja y presenta), lecturas del EFB a la hora del token, sustitución desde
> `assets.wlpk` y la ruta GX completa **sin Dawn** (WebGPU nulo de Choya) probados con datos sintéticos en Mesa (PC).
> **No es un port jugable todavía**: no se ha ejecutado con un disco ni en la RG35XX H, y no se ha compilado para
> aarch64 en esta máquina. Ver [docs/BLOQUEOS.md](docs/BLOQUEOS.md), [docs/MEDICIONES.md](docs/MEDICIONES.md) y
> [docs/PRUEBA_CONSOLA.md](docs/PRUEBA_CONSOLA.md).

## Hitos

| Hito | Estado |
| --- | --- |
| A. Base Linux reproducible | **hecho en PC x86-64**: dependencias fijadas, parches verificados por contenido (`fetch_deps.py --verify`), 67/67 pruebas de MacioGalaxy en serie. aarch64: scripts, toolchain y contenedor revisados; **sin compilar aquí** (no hay sysroot arm64 ni red) |
| B. Modelo real con Choya (GX) | app completa enlaza; con `CHOYA_GLES=1` Aurora graba (backend nulo, sin superficie) y el host de Choya crea el contexto, dibuja y presenta. Probado con la API GX real y datos sintéticos (pbuffer sin servidor gráfico y SDL/X11); sin disco |
| C. Arranque e interacción | pendiente de un disco (la app llega hasta pedirlo) |
| D. Escena jugable | pendiente |
| E. Ciclo de vida completo | salida limpia del hilo de pantalla implementada (leave/forceQuit, SIGTERM → SDL_EVENT_QUIT); sin probar en la consola |
| F. PortMaster y medición | lanzador con selección de pantalla (KMSDRM o fbdev), comprobación ELF/GLIBC, empaquetador con `libs.aarch64/`; sin binario aarch64 ni medición |
| G. Ruta directa | API y conversión probadas; **selector aplicado desde el juego** (registra el backend efectivo); las formas J3D aún no llegan a la ruta directa en el ejecutable (B11) |
| H. Comparación | pendiente (necesita disco y consola) |

## Ruta GX → Choya → pantalla

`CHOYA_GLES=1` activa el puente (y el paquete con `CHOYA_PAQUETE`). La app usa entonces el backend nulo de Aurora, sin
superficie WebGPU, y arranca el hilo `choya-pantalla`, dueño del contexto GLES (`GALAXY_PRESENT`):

| `GALAXY_PRESENT` | Contexto | Dónde |
| --- | --- | --- |
| `sdl` | `SDL_GL_CreateContext` sobre la ventana de Aurora (`SDL_WINDOW_OPENGL`) | PC (X11/Wayland); consola con DRM (SDL KMSDRM con EGL/GBM) |
| `fbdev` | EGL directo sobre `/dev/fb0` (ventana nativa del Mali de fbdev, como Choya) | consola sin `/dev/dri` |
| `pbuffer` | EGL fuera de pantalla | pruebas sin servidor gráfico |
| `auto` | `fbdev` si hay `/dev/fb0` y no hay DRM ni servidor gráfico; si no, `sdl` | por omisión |

Si el contexto no se puede crear, la app sale con error (no simula cuadros). Cada 10 s el log separa cuadros de
lógica, cuadros dibujados e imágenes presentadas. `GALAXY_BRIDGE_DEBUG=1` registra la TEV de cada pipeline y las
texturas que no están en el paquete.

## Estructura

```
deps/lock.json            commits fijados (MacioGalaxy, Choya, Aurora, libc++, dependencias de Aurora)
patches/                  parches versionados de MacioGalaxy (9) y Choya (1); fetch_deps.py los aplica y verifica
choya_bridge/             puente Aurora → núcleo de Choya y host de pantalla (hilo GLES)
nulo/                     WebGPU nulo de Choya para la Aurora única (sin Dawn)
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

Requisitos: git, python3, cmake ≥ 3.25, ninja, clang 18 (o 16+), Mesa con EGL/GLES y `xvfb-run` (pruebas gráficas).

```sh
git clone https://github.com/leonelavelarv-debug/galaxy-choya-native.git && cd galaxy-choya-native
python3 scripts/fetch_deps.py                 # commits fijados + parches (falla si no encajan)
sh scripts/build.sh host                      # bibliotecas y pruebas: libc++ estática, MacioGalaxy, Choya, ruta directa
ctest --test-dir build/host/macio -j1         # 67 pruebas (las de tiempos fallan si se corren en paralelo en 2 núcleos)
ctest --test-dir build/host/choya

GALAXY_GX=ON sh scripts/build.sh host         # + Aurora, puente y host de pantalla (Dawn de referencia para la PC)
ctest --test-dir build/host/macio -R "galaxy_(bridge|host|pack)" --output-on-failure
GALAXY_GX=ON GALAXY_WEBGPU_NULO=ON sh scripts/build.sh host   # lo mismo sin Dawn (WebGPU nulo, como la consola)

sh scripts/build.sh host app                  # solo el ejecutable: build/host/app/native/app/Petari + check_elf
```

Las cabeceras de Dawn que usa el WebGPU nulo se extraen del paquete fijado (`fetch_deps.py --dawn-headers`; sin red:
`--dawn-package-dir <paquete ya extraído>`) y se comprueban por hash.

## Compilar para la consola (aarch64)

```sh
docker build -t galaxy-choya-aarch64 -f docker/Dockerfile.aarch64 docker
docker run --rm -v "$PWD":/src -w /src galaxy-choya-aarch64 sh scripts/build.sh aarch64 app
mkdir -p out/libs.aarch64 && for l in libsqlite3.so.0 libpng16.so.16 libfreetype.so.6 libz.so.1; do \
  docker run --rm -v "$PWD":/src galaxy-choya-aarch64 cp -L /usr/lib/aarch64-linux-gnu/$l /src/out/libs.aarch64/; done
python3 scripts/package_portmaster.py --binary build/aarch64/app/native/app/Petari --libs out/libs.aarch64 \
  --out out/portmaster
```

`build.sh aarch64 app` compila solo el ejecutable (sin pruebas), con el WebGPU nulo (sin Dawn), y `check_elf.py`
comprueba arquitectura, cargador, `DT_NEEDED` (solo sistema base + las cuatro de `libs.aarch64/`) y que no pida una
GLIBC mayor que 2.36 (`GALAXY_GLIBC_MAX`). En Windows 11: lo mismo desde WSL2 con Docker Desktop.

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
| `GALAXY_PRESENT` | `auto`, `sdl`, `fbdev` o `pbuffer` (ver arriba); `GALAXY_PRESENT_PROBE=x,y;…` para pruebas |
| `GALAXY_GX` (compilación) | `ON` para compilar la fase 1 con Aurora |
| `GALAXY_WEBGPU_NULO` (compilación) | `ON`: WebGPU nulo de Choya en vez de Dawn (obligatorio en `aarch64 app`) |
| `PETARI_GFX_BACKEND` | sin Choya, backend de Dawn de referencia en la PC: `auto`, `vulkan`, `opengl` |

También se respetan las de MacioGalaxy (`PETARI_*`) y las del motor de Choya (`CHOYA_*`, p. ej. `CHOYA_PAQUETE`).

## Decisiones técnicas

- **libc++ estática**: MacioGalaxy exige libc++; estática evita depender de la libstdc++ del firmware. El desenrollado
  usa libgcc_s (el de glibc): mezclarlo con libunwind de LLVM rompía `pthread_exit` (comprobado).
- **Capa Darwin → Linux en vez de reescribir**: el planificador, el audio y el VI de MacioGalaxy siguen idénticos; las
  llamadas de Mach se implementan en Linux (suspensión real por señal, `/proc`, semáforos POSIX, `SCHED_FIFO`).
- **Aurora única**: la de MacioGalaxy, porque sus parches de sincronía, EFB y memoria son los que Galaxy necesita.
- **Sin `-ffast-math`**, `-O2`, `-mcpu=cortex-a53`; LTO y PGO como opciones (`GALAXY_LTO`, `GALAXY_PGO_*`).

Atribuciones y licencias: [THIRD_PARTY.md](THIRD_PARTY.md). Código propio: MIT ([LICENSE](LICENSE)).
