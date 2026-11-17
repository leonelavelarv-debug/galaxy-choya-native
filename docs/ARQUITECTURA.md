# Arquitectura

## Fases

```
Fase 1 (integración):  Petari nativo → J3D/GX → Aurora (única, 08122911) → puente Aurora→Choya → núcleo GLES de Choya → pantalla
Fase 2 (directa):      Petari nativo → J3D (lectores/animación de MacioGalaxy) → puente J3D → galaxy::direct::Device → núcleo GLES de Choya → pantalla
```

Ambas fases terminan en **el mismo núcleo** (`choya_gx_core.cpp`): un solo generador de shaders TEV, una sola caché de
programas, un solo EFB, un solo dueño de la presentación (`choya::gx::draw_frame`). La ruta directa no es un segundo
renderizador: llama a `capture_begin/pass/draw/copy/end` igual que el puente de Aurora, sin FIFO ni lista GX.

## Módulos (carpetas)

| Módulo | Dónde | Lenguaje |
| --- | --- | --- |
| Lógica del juego | `work/MacioGalaxy/src` (Petari) + parches `patches/MacioGalaxy` | C++17 nativo aarch64 |
| Servicios Linux | `work/MacioGalaxy/native/platform` + `compat/darwin` (capa Darwin→Linux) | C++17 |
| Procesamiento GX | Aurora única `work/aurora` + parches Python de MacioGalaxy (`native/gx/patch_aurora_*.py`) | C++20 (solo Aurora) |
| Adaptador J3D directo | `direct/src/j3d_shape_bridge.cpp`, `direct/src/gx_display_list.cpp` | C++17 |
| API directa | `direct/src/choya_direct.cpp` | C++17 |
| Renderizador GLES | `work/choya/choya/data/aurora/choya_gx_core.cpp` (sin cambios) | C++20 (lo pide el núcleo) |
| Recursos y cachés | conversor de Choya (`python -m choya convert` → `assets.wlpk`), `pack_lookup` en el núcleo | Python (PC) / C++ |
| Audio | JAudio + DSP/AI de MacioGalaxy, salida SDL3 (`native/audio/sdl_sink.cpp`) | C++17 |
| Entrada | `native/input` de MacioGalaxy (SDL3), perfil `portmaster/controls-rg35xxh.txt` | C++17 |
| Empaquetado | `portmaster/`, `scripts/package_portmaster.py` | sh / Python |

## Una sola implementación de cada servicio

- **Aurora:** una versión (la de MacioGalaxy). No se compila `aurora-main` de BrawlRecompiled.
- **SDL:** SDL3 (el que pide Aurora y usa la entrada de MacioGalaxy). Choya usaba SDL2 en su visor; el port no lo
  enlaza. Comprobar SDL3 en el firmware no hace falta para enlazar (va estático desde las fuentes fijadas), pero sí
  para que el backend KMSDRM/Mali funcione (B4).
- **Reloj de vídeo:** el VI de MacioGalaxy (`vi_host.cpp`, temporizador de 59.94 Hz en Linux: el display link de
  macOS no existe). El núcleo de Choya no lleva reloj propio: dibuja cuando le entregan un cuadro.
- **Presentación:** solo `choya::gx::draw_frame` + intercambio EGL. Con el núcleo de Choya activo, Aurora no presenta
  (el puente pone a Aurora en modo "sin GPU", como `sin_gpu()` en Choya).
- **Hilos:** el planificador de MacioGalaxy (testigo de CPU). El hilo GX de Choya (`choya_hilo.cpp`) **no** se
  activa todavía: primero ejecución gráfica ordenada (requisito), después se valida la propiedad de los datos.

## Convivencia de los backends

Regla actual: **un backend por escena**, elegido al cambiar de escena (`galaxy::renderer_for_scene`). Nunca se
mezclan en el mismo cuadro, así que no comparten estado GL a mitad de cuadro. Lo que sí comparten, por diseño:

| Recurso | Cómo |
| --- | --- |
| Framebuffer/EFB y profundidad | el del núcleo (uno solo) |
| Estado GL | lo maneja el núcleo (`GlShadow`), los dos entran por `capture_*` |
| Texturas | ids del núcleo (`texture_data`, `texture_from_pack`); el grupo de texturas de la ruta directa usa ids con el bit 63 |
| Programas | caché del núcleo (clave = `PipelineDesc`) |
| Orden | el de los `capture_*` dentro del cuadro; la ruta directa no reordena ni agrupa dibujos |
| Sincronización | `GXDrawDone`/`GXSetDrawSync` siguen en el puente GX (sync_bridge de MacioGalaxy); la ruta directa no los emite y por eso solo se habilita en escenas donde ningún dibujo directo depende de ellos (B7) |

Mezclar dentro del mismo cuadro (p. ej. escenario por directa y partículas por GX) requiere que el puente GX y la
ruta directa escriban en la **misma** captura del cuadro; está diseñado (los dos usan `capture_*`), pero no
implementado (B6).

## Ruta directa: qué es persistente

- Geometría: se convierte una vez (`convert_display_list` al cargar el modelo; `Device::create_mesh`) y vive en un
  arena estable; el arena solo se rehace al crear o soltar mallas (prueba: `reconstrucciones_de_geometria=1` tras
  75 004 dibujos).
- Materiales: un `PipelineDesc` por material; un pipeline por (material, formato de malla) registrado una vez.
- Por cuadro: solo los uniformes (matrices, colores animados, konstantes).
- Pendiente: que el núcleo mantenga en la GPU la parte estática del búfer de vértices (hoy lo vuelve a subir cada
  cuadro, igual que en la ruta GX) — B8.

## Compilación

- Código propio: C++17. Núcleo de Choya y Aurora: C++20 por objetivo.
- `-mcpu=cortex-a53`, `-O2` (toolchain), sin `-ffast-math` (MacioGalaxy además usa `-ffp-contract=off`).
- libc++ 18 estática (con `-fexperimental-library` para `std::jthread` de Aurora); desenrollado con libgcc_s, como
  glibc.
- `-fno-omit-frame-pointer`: el planificador recorre la pila por punteros de marco.
- LTO (`-DGALAXY_LTO=ON`) y PGO (`-DGALAXY_PGO_GENERATE=ON`, luego `-DGALAXY_PGO_PROFILE=...`) son opciones
  reproducibles; no se activan hasta tener la referencia en la consola.
