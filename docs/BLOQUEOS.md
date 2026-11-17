# Bloqueos concretos

Cada uno con archivo, función, causa y siguiente acción. Ninguno se "resolvió" con un stub que devuelva éxito.

## B1 — Puente Aurora 08122911 → núcleo de Choya (fase 1): escrito y probado en PC; quedan aproximaciones

- **Hecho:** `choya_bridge/` reemplaza al adaptador de Choya (`choya_gx.cpp`, escrito para la Aurora de
  BrawlRecompiled). `patch_aurora_bridge.py` engancha la Aurora única en cinco puntos con anclas obligatorias (cada
  una debe aparecer exactamente una vez o la compilación se detiene): cierre del `FramePacket` (`frame.cpp`), alta de
  pipelines GX/borrado (`recording.cpp`), grupos de enlace (`command_processor.cpp`), subida y liberación de texturas
  (`texture.cpp`) y copias del EFB/pantalla (`GXFrameBuffer.cpp`). `galaxy_choya_bridge.cpp` traduce
  `PipelineConfig` → `PipelineDesc` y **reempaqueta** cada bloque de uniformes al orden del núcleo (cabecera de 80
  bytes con `vtx_start`, matriz actual y comienzos de arreglos; desplazamientos calculados con la secuencia de
  `fill_uniform`; niebla e indirectas convertidas al formato del núcleo). Los dibujos `GX_TRIANGLES` no indexados
  (`indexCount=0`) reciben índices secuenciales generados.
- **Comprobado (PC, Mesa llvmpipe, Dawn backend nulo; NO es la Mali):** `tests/bridge_gx_test.cpp` usa la API GX
  real → FIFO → Aurora con todos los parches de MacioGalaxy → puente → núcleo GLES. Píxel exacto en: color por
  vértice, dibujo no indexado, textura RGB565 subida por el gancho, canal iluminado (ambiente de registro × material
  del vértice), copia del EFB a textura reutilizada en el mismo cuadro, borrado del EFB y copia a pantalla. Ningún
  dibujo omitido.
- **Aproximaciones que quedan (contadas en `galaxy_choya::stats()` y registradas una vez en el log):** niebla con
  ajuste por rango (sin ajuste), texgens de relieve (bump), polygon offset, dibujos de líneas/puntos (el núcleo los
  omite), `TextureCopy` y tareas de codificador de Aurora, texturas BC1 y R16Sint (índices de paleta) que el núcleo
  no recibe aún. Sin probar con datos reales: luces de verdad (direccionales/puntuales), niebla, matrices indirectas
  y conversiones de paleta.
- **Siguiente acción:** con disco propio, volcar cuadros reales (`GALAXY_BRIDGE_DEBUG=1` registra la TEV de cada
  pipeline) y comparar contra la salida de Dawn en la PC; después, en la RG35XX H.

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

- **Causa:** el puente GX (B1) abre y cierra la captura del cuadro dentro de `capture_packet` (un solo
  `capture_begin`/`capture_end` por `FramePacket`). La ruta directa tendría que insertar sus dibujos en ese mismo
  intervalo y respetar el orden de los pases de Aurora; falta definir ese punto de inserción.
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
