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
- **Auditoría (fase 5, 2026-11-17).** Qué llega al núcleo y qué no, sin disco para medir cuánto lo usa Galaxy:
  | Operación | Estado | Motivo / diagnóstico |
  | --- | --- | --- |
  | CMPR (texturas comprimidas del disco) | **cubierta** | Aurora siempre la decodifica a RGBA8 (`BuildRGBA8FromCMPR`); llega al núcleo como RGBA8 |
  | BC1 directo (`GX_TF_BC1_PC`) | no se envía (contado) | solo aparece con paquetes de reemplazo de Aurora para PC, que el port no usa; con `assets.wlpk` las texturas van en ETC2 |
  | Índices de paleta R16Sint | no se envían (contado) | las paletas estáticas Aurora las convierte a RGBA en la CPU; las de copias del EFB van por `PaletteConv` del núcleo (cubierto) |
  | Copia textura → textura (`TextureCopy`) | omitida, contada y registrada | el núcleo no la tiene; sin disco no se sabe si Galaxy la usa |
  | Tareas de codificador (`EncoderTask`) | omitidas, contadas | registradas por Aurora/MacioGalaxy para su propio trabajo WebGPU |
  | Dibujos personalizados / ImGui | omitidos, contados | menús nativos de MacioGalaxy: ver B12 |
  | Líneas y puntos | contados (`lineDraws`); el núcleo no los dibuja | pendiente de saber si Galaxy los usa fuera de depuración |
  | Polygon offset | contado; el núcleo no lo aplica | puede dar z-fighting en calcomanías; medir con disco |
  | Niebla con ajuste por rango | contada; se dibuja sin el ajuste | la niebla normal sí se traduce |
  Todo lo omitido se cuenta en `galaxy_choya::stats()` y se escribe una vez en `log.txt`: nada se pierde en silencio.
- **Siguiente acción:** con disco propio, `GALAXY_BRIDGE_DEBUG=1` y los contadores del log en una escena completa
  (p. ej. Observatorio): implementar primero lo que aparezca con contador distinto de cero.

## B2 — WebGPU nulo para la Aurora única: hecho y probado en PC; falta el sha256 del paquete aarch64

- **Hecho:** `nulo/` genera el WebGPU nulo de Choya (`webgpu_nulo.cpp` del commit fijado + el resto generado con su
  `generar-webgpu-nulo.py`) contra las cabeceras de Dawn `v20260807.225922` (la versión que pide Aurora 08122911) y lo
  da como `dawn::webgpu_dawn` (`AURORA_DAWN_PROVIDER=system`, parche 0009 de MacioGalaxy, `GALAXY_WEBGPU_NULO=ON`).
  Lo único de `dawn::native` que usa Aurora (el constructor de `DawnInstanceDescriptor`) está en
  `nulo/dawn_native_nulo.cpp`. `build.sh aarch64 app` lo usa siempre: el binario de la consola no lleva Dawn.
- **Comprobado (PC):** las pruebas `galaxy_bridge_gx`, `galaxy_host_gx_pbuffer` y `galaxy_pack_gx` pasan con el nulo
  en lugar de Dawn (`build/host/macio-nulo`).
- **Lecturas del EFB:** no las da el nulo (devolvería ceros): las atiende el núcleo de Choya (B7, resuelto en la ruta GX).
- **Pendiente:** las cabeceras se extrajeron aquí del paquete x86-64 ya descargado (hash del árbol en `lock.json`).
  En el contenedor, `fetch_deps.py --dawn-headers` descarga el paquete aarch64; su `sha256` hay que fijarlo en
  `deps/lock.json` la primera vez (aquí no hay red para calcularlo).

## B3 — Shell de la app de macOS (resuelto en la PC; queda la presentación de la consola)

- **Hecho (parche 0005):** `app_main.cpp`, `display_link.cpp`, `latency_report.cpp`, `soak_telemetry.cpp` y
  `smoke_storage.cpp` compilan en Linux (AppKit solo en macOS; VI en su temporizador de 59.94 Hz, el respaldo que
  MacioGalaxy ya tenía; telemetría desde `/proc` y `mallinfo2`; SHA-256 portátil; el modo de humo en segundo plano,
  que aísla la caché de Metal, falla con un mensaje explícito). **El ejecutable `Petari` completo (juego + Aurora +
  SDL3 + Dawn de referencia) enlaza en Linux x86-64 y arranca hasta pedir el disco:**
  `petari: disc …/RMGE01 does not contain files/ or DATA/files/ from an extracted disc`.
- **Queda:** en la consola no hay Dawn ni Metal: la presentación tiene que ser la del núcleo de Choya (B1, B2, B4).
  Sin disco no se puede pasar del arranque (B9).

## B4 — Ventana/contexto en la consola: implementado (sdl / fbdev); sin probar en la RG35XX H

- **Hecho:** `choya_bridge/galaxy_choya_host.cpp`. Con DRM: SDL3 KMSDRM con EGL/GBM sobre la ventana de Aurora
  (`SDL_WINDOW_OPENGL`). Sin DRM (Mali de fbdev): EGL directo sobre `/dev/fb0` como `choya_gles.cpp` de Choya, con SDL
  en `offscreen` solo para mando y audio. El lanzador elige según `/dev/dri` (o `conf/present.txt`) y lo escribe en
  `log.txt` junto con la glibc.
- **Sin comprobar:** qué pila tiene el firmware real (KNULLI con blob de fbdev o Panfrost con DRM) y si SDL3 KMSDRM
  convive con el EGL del firmware. Ver docs/PRUEBA_CONSOLA.md.

## B5 — Recursos de Galaxy en el conversor de Choya

- **Causa:** el conversor se probó con Wii Sports (BRRES/MDL0, BRSAR). Galaxy usa BMD/BDL, BTI, ARC con Yaz0 y
  JAudio (AAF/BAA/AW). No hay disco aquí para comprobar qué texturas encuentra y cuáles omite.
- **Siguiente acción:** `python -m choya scan <RMGE01>` y `convert --preview`; el reporte debe listar cada BMD/BDL/BTI
  y su cuenta de texturas. Si faltan, añadir el lector de la sección TEX1 de J3D al conversor (la clave del paquete es
  el contenido de la textura, así que el runtime ya la encontraría). El audio de JAudio no se convierte: lo sigue
  reproduciendo JAudio (requisito).

## B6 — Mezcla GX + directa dentro del mismo cuadro

- **Decisión:** no se mezclan. El selector decide por escena completa al cambiar de etapa (parche 0008) y lo registra;
  una escena va entera por un backend. El puente abre y cierra la captura por `FramePacket`; la ruta directa tendría
  que insertar sus dibujos en ese intervalo respetando los pases de Aurora (no implementado).

## B7 — Sincronía y lecturas del EFB

- **Ruta GX (hecho, probado en PC):** `GXDrawDone`, tokens y `GXSetDrawSync` siguen en `sync_bridge` de MacioGalaxy
  (orden del FIFO en la CPU, no dependen de la GPU). Las lecturas `GXPeekZ`/`GXPeekARGB` a la hora del token ya no las
  da Dawn nulo (habría dado ceros): `capture_segment` entrega al núcleo lo grabado hasta el token con copias Z24X8 y
  RGBA8, el hilo de pantalla las lee y llama a `ready(ticket)`; si el núcleo no devuelve las copias, el proceso aborta
  en vez de entregar un token con datos inventados. `galaxy_host_gx` comprueba que la instantánea ve el estado del
  token (no el final del cuadro).
- **Aproximación:** la copia de color se muestrea con el filtro de la copia del EFB del núcleo cuando el render de
  Aurora es más grande que el EFB lógico (Dawn tomaba el texel más cercano); igual con escala 1.
- **Ruta directa:** no emite tokens ni lecturas; por eso solo se habilitaría por escena (B6/B11).

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

## B11 — Ruta directa en el ejecutable

- **Comprobado:** antes, ni el selector ni `convert_shape` ni las bibliotecas directas estaban en `Petari`. Ahora el
  selector se aplica desde el juego (`SceneControlInfo::setStage`, parche 0008) y registra el backend efectivo.
- **Pendiente:** conversión y caché de `J3DShape` al cargar el modelo (`convert_shape`, con el contrato de orden de
  bytes corregido en la fase 3), materiales/texturas/matrices desde `J3DMaterial` y `J3DShapeMtx`, invalidación al
  descargar la escena, y la inserción de los dibujos en la captura (B6). Hasta entonces una escena pedida como
  `direct` sigue entera por GX y el log dice por qué. La lista de escenas validadas sigue vacía.

## B12 — Menús ImGui de MacioGalaxy con Choya

- **Causa:** con Choya presentando, Aurora ya no crea su `SDL_Renderer` (tomaría la superficie GL de la ventana o el
  KMS de la consola) e ImGui dibuja por WebGPU nulo: los menús nativos de MacioGalaxy (inicio, editor) no se ven. El
  juego no depende de ellos.
- **Siguiente acción:** pasar las listas de dibujo de ImGui al núcleo (un pase propio al final del cuadro) si hacen
  falta en la consola.

## B13 — Bibliotecas que pide el ejecutable

- **Comprobado en PC (`check_elf.py`):** además del sistema base, `libsqlite3`, `libpng16`, `libfreetype`, `libz` (y
  `libstdc++` solo por el Dawn de la PC; el nulo no la pide). En la consola van en `libs.aarch64/` del port, copiadas
  del sysroot Debian 12 arm64 (glibc 2.36); `check_elf.py` las acepta solo con `--allow`.
- **Sin comprobar:** el binario aarch64 real (no se compiló aquí) ni si el firmware ya trae esas bibliotecas.

