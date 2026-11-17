# Bloqueos concretos

Cada uno con archivo, función, causa y siguiente acción. Ninguno se "resolvió" con un stub que devuelva éxito.

## B1 — Puente Aurora 08122911 → núcleo de Choya (fase 1) sin escribir

- **Archivo:** `work/choya/choya/data/aurora/choya_gx.cpp` (`describe`, `capture_*`) y los parches de
  `choya/retarget.py` (`AURORA_EDITS`, inserta la captura en `lib/gfx/common.cpp`).
- **Causa:** están escritos para `aurora-main` de BrawlRecompiled. En la Aurora única (`08122911`) no existen
  `lib/gfx/common.cpp`, `gx::tev_stage_texture_dependency`, `tev_effective_texcoord`, `tev_z_texture_*`, ni los campos
  `dualTexEnabled/zFreeze/zTexture/numTexGens` de `ShaderConfig`; los uniformes siguen el orden de
  `gx::build_shader_info` (upstream) y no el que espera el núcleo (`vtx_start`, matriz por omisión, comienzos de
  arreglos en las palabras 0-33).
- **Siguiente acción:** escribir `choya_bridge/aurora_upstream.cpp` que, al cerrar cada `FramePacket` (en
  `gfx/render_worker.cpp`, antes de codificar para WebGPU), recorra `packet.ops`: `RenderPass` → `capture_pass` (con
  `resolveTarget/resolveRect/resolveFormat`, `paletteConvs`), `Command::SetViewport/SetScissor`, y los `Draw` cuyo
  `encoder == encode_draw<gx::render, gx::DrawData>` → `capture_draw`. Por dibujo: traducir `PipelineConfig` a
  `PipelineDesc` (los campos que upstream tiene; `ext.valid=1`), y **reempaquetar** su bloque de uniformes al orden
  del núcleo (calculando los desplazamientos de upstream con la misma secuencia de `build_shader_info`) más
  `vtx_start`/comienzos de arreglos desde `vertRange` y `BindGroupRanges::vaRanges`. Lo que no se traduzca se cuenta y
  se registra por material (nada se omite en silencio). Probar con volcados de cuadros reales (`CHOYA_GX_VOLCADO`)
  cuando haya disco.

## B2 — WebGPU nulo para la Aurora única

- **Archivo:** `work/choya/choya/data/aurora/sin_dawn/include/{webgpu,dawn}/*`, `webgpu_nulo.cpp`.
- **Causa:** las cabeceras son de la versión de Dawn de aurora-main; Aurora `08122911` pide Dawn `v20260807.225922`
  (API distinta). En la PC se usa el paquete precompilado de Dawn (solo referencia); en la consola no hay Dawn.
- **Siguiente acción:** regenerar el nulo con `scripts/generar-webgpu-nulo.py` de Choya a partir de las cabeceras
  del paquete `dawn-linux-aarch64.tar.gz` de esa versión y enlazarlo en lugar de `dawn::webgpu_dawn` con
  `AURORA_DAWN_PROVIDER=system` apuntando al nulo. Las lecturas del EFB y `GXDrawDone` no pueden ir al nulo: los
  atiende el núcleo de Choya (B1/B7).

## B3 — Shell de la app de macOS (resuelto en la PC; queda la presentación de la consola)

- **Hecho (parche 0005):** `app_main.cpp`, `display_link.cpp`, `latency_report.cpp`, `soak_telemetry.cpp` y
  `smoke_storage.cpp` compilan en Linux (AppKit solo en macOS; VI en su temporizador de 59.94 Hz, el respaldo que
  MacioGalaxy ya tenía; telemetría desde `/proc` y `mallinfo2`; SHA-256 portátil; el modo de humo en segundo plano,
  que aísla la caché de Metal, falla con un mensaje explícito). **El ejecutable `Petari` completo (juego + Aurora +
  SDL3 + Dawn de referencia) enlaza en Linux x86-64 y arranca hasta pedir el disco:**
  `petari: disc …/RMGE01 does not contain files/ or DATA/files/ from an extracted disc`.
- **Queda:** en la consola no hay Dawn ni Metal: la presentación tiene que ser la del núcleo de Choya (B1, B2, B4).
  Sin disco no se puede pasar del arranque (B9).

## B4 — Ventana/contexto en la consola

- **Causa:** la consola no tiene X11/Wayland; SDL3 debe usar KMSDRM y la libGLES de Mali del firmware. No hay
  consola en este entorno para comprobar que SDL3 abre KMSDRM con el blob de Mali de KNULLI/muOS.
- **Siguiente acción:** en la consola, `SDL_VIDEO_DRIVER=kmsdrm` y anotar en `log.txt` `SDL_GetCurrentVideoDriver()`
  y `GL_RENDERER`; si SDL3 no sirve, usar la pantalla EGL/GBM propia de Choya (`choya_gles.cpp`), que ya funciona en
  KNULLI con Wii Sports (según el historial de Choya), y dejar SDL3 solo para entrada y audio.

## B5 — Recursos de Galaxy en el conversor de Choya

- **Causa:** el conversor se probó con Wii Sports (BRRES/MDL0, BRSAR). Galaxy usa BMD/BDL, BTI, ARC con Yaz0 y
  JAudio (AAF/BAA/AW). No hay disco aquí para comprobar qué texturas encuentra y cuáles omite.
- **Siguiente acción:** `python -m choya scan <RMGE01>` y `convert --preview`; el reporte debe listar cada BMD/BDL/BTI
  y su cuenta de texturas. Si faltan, añadir el lector de la sección TEX1 de J3D al conversor (la clave del paquete es
  el contenido de la textura, así que el runtime ya la encontraría). El audio de JAudio no se convierte: lo sigue
  reproduciendo JAudio (requisito).

## B6 — Mezcla GX + directa dentro del mismo cuadro

- **Causa:** el puente GX (B1) y la ruta directa necesitan escribir en la misma captura del cuadro (un solo
  `capture_begin`). Hasta que B1 exista no se puede ordenar la mezcla.
- **Acción actual:** selección por escena (`renderer_for_scene`), nunca mezcla.

## B7 — Sincronía y lecturas del EFB en la ruta directa

- **Causa:** `GXDrawDone`, `GXSetDrawSync` y `GXPeek*` los resuelve hoy el puente GX de MacioGalaxy (`sync_bridge`,
  `efb_peek`). La ruta directa no los emite.
- **Siguiente acción:** exponer en el núcleo una espera de "cuadro dibujado" (`draw_frame` terminado) y lecturas de
  color/profundidad desde el EFB de GLES, y conectarlas a `sync_backend` de MacioGalaxy antes de habilitar direct en
  escenas que las usen.

## B8 — Geometría estática residente en la GPU

- **Archivo:** `choya_gx_core.cpp` (`capture_begin` copia `verts`; el cuadro sube el búfer como textura de palabras).
- **Causa:** el núcleo vuelve a subir todos los vértices cada cuadro, también los estáticos de la ruta directa.
- **Siguiente acción:** una segunda textura de palabras persistente para el arena de la ruta directa, enlazada como
  `vbuf` en sus dibujos (el shader no cambia), invalidada solo cuando cambia el arena. Medir en la Mali antes y después.

## B9 — Hardware y datos

- No hay RG35XX H ni disco RMGE01 en este entorno: no se ejecutó nada en la consola, no se midió la Mali, no se
  probaron escenas reales, guardado/carga ni audio con datos. Las pruebas con `--assets` de MacioGalaxy no corrieron.
- No hay demonio de Docker aquí: el binario aarch64 no se compiló. El toolchain y el contenedor están escritos pero
  sin ejecutar.

## B10 — glibc del firmware

- **Causa:** el contenedor usa Debian 12 (glibc 2.36). Si el firmware trae una glibc más vieja, el binario no arranca
  (`GLIBC_2.3x not found` en `log.txt`).
- **Siguiente acción:** comparar `build-info.json` (que lista las versiones GLIBC pedidas) con `ldd --version` en la
  consola; si hace falta, cambiar la imagen base a una con glibc más vieja (libc++ ya va estática).
