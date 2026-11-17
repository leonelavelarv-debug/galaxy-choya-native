# Inspección inicial (registro)

Fecha: 2026-11-16. Todo lo que dice "comprobado" se verificó en el código o compilando/ejecutando en este entorno
(PC Linux x86-64, Ubuntu 24.04, clang 18, Mesa llvmpipe). "Documentado" es lo que afirman los README sin que aquí se
haya podido comprobar. "Pendiente" es lo que falta.

## 1. Commits utilizados (deps/lock.json)

| Dependencia | Commit | Notas |
| --- | --- | --- |
| MacioGalaxy | `60662da6f5d0e95467637eee4ebab3282eeaf863` | Petari `e5bc761c0` adaptado a macOS/arm64 |
| Choya Wii Converter | `95c75ccc6a56c6ce3b2be4c84903e43ffd1f84e8` (0.42.1) | motor GLES, conversor, empaquetado |
| Aurora (única) | `08122911e8621acb7ded6563813b264bec1494b5` | la que fija MacioGalaxy en `native/gx/CMakeLists.txt` |
| llvm-project (libc++) | `c13b7485b879…` (`llvmorg-18.1.3`) | solo `libcxx`, `libcxxabi`, `runtimes` |
| Dependencias de Aurora | ver `deps/lock.json` → `aurora_extern` | clonadas por git (las URL de archivo de GitHub dan 403 en este entorno) |

Referencia comparada (no se usa para compilar): BrawlRecompiled `a77be8847ec9…`, carpeta `wiicompiled/aurora-main`
(la Aurora contra la que se escribió el adaptador de Choya).

## 2. Cómo compila y ejecuta MacioGalaxy (comprobado)

- CMake de nivel superior `PetariNative`: `petari_core` (Nerve/Spine/JMath/mtx), `petari_memory`, `petari_heaps`,
  `petari_kernel`, `petari_resources` (lectores J3D/KCL/JPA/U8/arc), `native/platform/*` (OS, DVD, NAND, SC, ARAM,
  VI, audio AI/DSP, GX sync), `native/input`, `petari_game_objects` (1640 archivos de `src/Game`, objeto),
  `petari_game_libraries` (JSystem/nw4r). Con `PETARI_BUILD_GX_PROBE=ON` añade Aurora (FetchContent), `native/gx`
  (parches Python sobre Aurora), `native/app` (ejecutable `petari`) y `native/movie`.
- Las fuentes con literales Shift-JIS se recodifican al configurar (`native/tools/encode_legacy_sources.py`).
- Exige **libc++** (`native/include/petari/game_compat.hpp` se niega con libstdc++) y C++17 para el juego; Aurora usa
  C++20 (`std::jthread`, `std::stop_token`).
- El juego corre sobre hilos del sistema con un "testigo" de CPU que reproduce el procesador único de la Wii
  (`native/platform/os/os_thread.cpp`), con preempción forzada que suspende hilos y recorre su pila por punteros de
  marco.

## 3. Servicios (comprobado: archivos)

| Servicio | Dónde | Dependencias de Apple encontradas |
| --- | --- | --- |
| Memoria / heaps | `native/memory`, `native/src/host_allocation.cpp` | `mach-o` (tabla de símbolos nlist para clasificar código), `pthread_get_stack*_np` |
| Hilos / planificador | `native/platform/os/os_thread.cpp` | Mach: `thread_suspend/resume/get_state/info`, semáforos Mach, QoS, `getsegmentdata` |
| Alarmas / tiempo | `os_alarm.cpp`, `os_time.cpp` | ninguna |
| Archivos / disco | `native/platform/dvd` | ninguna |
| Guardado (NAND) | `native/platform/nand/isfs_host.cpp` | `setxattr/getxattr` de Darwin (6 argumentos) |
| Audio | `native/platform/audio`, `native/audio/sdl_sink.cpp` | libdispatch, `os_unfair_lock`, Mach time-constraint, CoreAudio (latencia), `sysctl` |
| VI / sincronía | `native/platform/vi/vi_host.cpp`, `native/app/display_link.cpp` | Mach time, display link de macOS |
| Entrada | `native/input` | ninguna (SDL3) |
| Fallos | `native/platform/os/os_crash.cpp` | `mcontext` de Darwin |
| Caché de instrucciones | `os_cache.cpp` | `sys_icache_invalidate` |
| Presentación | `native/gx/present`, `native/app` | Metal/Dawn, display link |

## 4. Aurora: MacioGalaxy frente a Choya (comprobado en el código)

- **MacioGalaxy** usa Aurora upstream `08122911` y la modifica con ~30 scripts Python que reescriben archivos al
  configurar (`native/gx/patch_aurora_*.py`): traducción de direcciones, texturas/paletas, matrices posteriores,
  sincronía (`GXDrawDone`, `GXSetDrawSync`), lecturas del EFB (`efb_peek.cpp`), presentación, asignaciones. Los
  parches fallan si no encuentran su texto (comprobado en `patch_aurora_present.py`: `COMMAND_ERROR_IS_FATAL`).
- **Choya** se escribió contra `aurora-main` de BrawlRecompiled (sin revisión propia). Diferencias medidas:
  - `GXEnum.h`: solo 2 enumeradores de más en upstream (`GX_TF_BC1_PC`, `GX_TF_RG8_PC`). El núcleo de Choya
    (`choya_gx_core.cpp`), que solo depende de `GXEnum.h`, **compila contra la Aurora única y su prueba pasa**.
  - `ShaderConfig`: aurora-main tiene `dualTexEnabled`, `fogRangeAdjust`, `zFreeze`, `zTexture`, `numTexGens`;
    upstream no (tiene `fogRangeEnabled`, `TcgConfig::embossSrc`, `AttrConfig::nbt3`). Las funciones auxiliares que usa
    el adaptador (`tev_stage_texture_dependency`, `tev_effective_texcoord`, `tev_z_texture_*`) **no existen** en upstream.
  - Cuadro: aurora-main arma pases en `lib/gfx/common.cpp` (donde Choya inserta su captura); upstream usa
    `gfx::detail::FramePacket` (`frame_packet.hpp`) con `RenderPass::commands` y dibujos GX reconocibles por
    `encoder == encode_draw<gx::render, gx::DrawData>` (`recording.cpp`).
  - Uniformes: upstream los ordena según `build_shader_info` (tamaños de viewport, proyección, 30 matrices, registros
    TEV, luces, colores, konstantes, matrices posteriores, niebla, escalas, matrices indirectas); el núcleo de Choya
    espera su propia disposición (palabra 0 `vtx_start`, 1 matriz por omisión, 8.. comienzos de arreglos y lo que
    declare el `PipelineDesc`).
  - **Conclusión:** se elige la Aurora de MacioGalaxy (`08122911`), porque sus parches de sincronía/EFB/memoria son
    los que necesita Galaxy, y se conserva el núcleo de Choya tal cual. El adaptador `choya_gx.cpp` no se puede
    aplicar sobre esa Aurora: hay que escribir uno nuevo (ver `docs/BLOQUEOS.md`, B1).
- **Proveedor sin Dawn** (`choya/data/aurora/sin_dawn`): `webgpu.h`/`webgpu_cpp.h` de la versión de Dawn de
  aurora-main y una implementación nula. Upstream `08122911` pide Dawn `v20260807.225922`; las cabeceras no coinciden
  y el proveedor nulo habría que regenerarlo con `scripts/generar-webgpu-nulo.py` de Choya contra esas cabeceras
  (pendiente, B2).

## 5. Interfaces para renderizado directo (comprobado)

El núcleo de Choya no tiene API de modelos: recibe cuadros (`capture_begin/pass/draw/copy/end`) con vértices que el
shader lee de un búfer de bytes big endian, `PipelineDesc` que define el programa (TEV completo, texgen, canales,
alfa, mezcla, profundidad) y grupos de texturas. Eso basta para una ruta directa **sin FIFO GX**: se implementó
`galaxy::direct::Device` sobre esas mismas funciones (mismo generador de shaders, misma caché, mismo EFB).

## 6. Recursos (Choya)

- El conversor reconoce texturas GX dentro de U8/ARC/Yaz0 y modelos **MDL0/BRRES** (NW4R). Galaxy usa **BMD/BDL**
  (J3D) y animaciones BCK/BTK/BRK/BTP/BVA/BPK: el visor de modelos de Choya no los sustituye (documentado en el
  propio README de Choya; comprobado en `choya/mdl0.py`). Para texturas, el paquete se busca por la clave del
  contenido (`pack_lookup`), que no depende del formato del contenedor: sirve para las texturas de BMD/BDL/BTI si el
  conversor las encuentra. Que el conversor recorra los BMD/BDL de Galaxy está **pendiente de comprobar con el disco**
  (B5).

## 7. Clasificación

| Afirmación | Estado |
| --- | --- |
| MacioGalaxy juega en macOS (README) | documentado |
| El núcleo y los servicios de MacioGalaxy compilan en Linux y sus 59 pruebas pasan | **comprobado (x86-64)** |
| Los 1641 archivos del juego compilan en Linux | **comprobado (x86-64)** |
| Aurora + app `petari` compilan en Linux | en curso / ver BLOQUEOS |
| El motor GLES de Choya dibuja con la Aurora única | **comprobado (Mesa)** |
| La ruta directa produce la misma imagen que la ruta de captura GX | **comprobado (Mesa, escena sintética)** |
| Cualquier cosa en la RG35XX H | pendiente (no hay consola en este entorno) |
| Cualquier cosa con datos del juego | pendiente (no hay disco en este entorno) |
