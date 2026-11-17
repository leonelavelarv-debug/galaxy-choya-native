# Pruebas y mediciones realmente ejecutadas

Entorno: PC Linux x86-64 (2 núcleos, Ubuntu 24.04), clang 18.1.3, libc++ 18.1.3 estática, Mesa llvmpipe (EGL sin
superficie, GLES 3.2). **Nada de esto es la RG35XX H ni su Mali-G31**: los tiempos de GPU de llvmpipe no dicen nada
de la consola y no se usan para prometer FPS.

## Compilación (MacioGalaxy en Linux, `build/host/macio`)

| Qué | Resultado |
| --- | --- |
| Núcleo, memoria, heaps, kernel, recursos, plataforma, entrada, editor, menú Home (`all`) | compila (435 objetivos) |
| `petari_game_objects`: los 1641 archivos de `src/Game` + BCSV incrustado | compila (tras los parches 0001-0003) |
| Puente J3D de la ruta directa (`galaxy_direct_j3d`) contra las cabeceras J3D de Petari | compila |
| Aurora `08122911` + parches de MacioGalaxy, SDL3, Dear ImGui, Tracy, zstd (`GALAXY_GX=ON`) | compila |
| App `Petari` completa (juego + Aurora + SDL3 + Dawn de referencia), tras el parche 0005 | **enlaza** (ELF x86-64, 38 MB) y arranca hasta pedir el disco (sin datos aquí) |

## Pruebas de MacioGalaxy en Linux (`ctest -j1`)

65 de 65 pasan en serie (2026-11-17, 202 s), entre ellas: núcleo (matemáticas, Nerve, RNG), memoria y heaps, archivos JKR, recursos J3D/KCL/JPA,
mensajes, NAND (con atributos en xattr o archivo lateral), DVD, OS (planificador con testigo de CPU, preempción,
alarmas), VI, audio (DSP/AI), sincronía GX, fallos (informe con registros), entrada, editor. Con `ctest -j2` en esta máquina de 2 núcleos fallan de forma
intermitente tres pruebas de tiempos (`native_platform_os`, `native_platform_gx_sync`, `native_audio_pacing`) y pasan
al repetirlas solas: miden plazos de planificación y no toleran la carga de otra prueba en paralelo.

Para que el diagnóstico del testigo de CPU nombre el sitio en el ejecutable cuando el hilo espera dentro de glibc
(futex, nanosleep), la capa Darwin desenrolla con la CFI de libgcc desde la señal de suspensión hasta el primer
marco del ejecutable con cadena de punteros de marco coherente (en macOS esto sale solo: libsystem conserva los
punteros de marco). En x86-64, sin registro de enlace, `lr` apunta a la propia función. El parche 0006 adapta dos
expectativas propias de macOS (nombre `__psynch_mutexwait`; "más rápido que dladdr", cuando en Linux dladdr ya es
la búsqueda en la `.symtab`).

Las pruebas que necesitan el disco (`--assets build/game-data/RMGE01/files`) no se registran sin datos: **no se
ejecutaron**.

## Motor de Choya con la Aurora única

`choya_motor_harness` (la prueba original de Choya) compilado contra `GXEnum.h` de Aurora `08122911`: **PRUEBA OK**
(colores, copias del EFB, invalidaciones, volcado y redibujo del cuadro).

## Puente Aurora 08122911 → Choya (`galaxy_bridge_gx`)

API GX real (`GXInit`, `GXBegin`…, `GXCopyTex`, `GXCopyDisp`) → FIFO → Aurora con los parches de MacioGalaxy y el
backend **nulo** de Dawn → puente → núcleo GLES de Choya en Mesa llvmpipe (EGL sin superficie, GLES 3.2), bajo
`xvfb-run` porque Aurora abre una ventana SDL. 4 cuadros de 640×480; se leen píxeles de los cuadros 2 y 3:

| Caso | Esperado | Leído |
| --- | --- | --- |
| Cuadrado con color por vértice (`GX_QUADS`, índices de Aurora) | `ff0000` | `ff0000` |
| Triángulo no indexado (`GX_TRIANGLES`, `indexCount=0`, índices generados por el puente) | `00ff00` | `00ff00` |
| Textura RGB565 8×8 subida por el gancho de `texture.cpp` (TEV `GX_REPLACE`) | `ffff00` | `ffff00` |
| Canal iluminado: ambiente de registro verde × material blanco del vértice, sin luces | `00ff00` | `00ff00` |
| Copia del EFB a textura (`GXCopyTex`) muestreada en el mismo cuadro | `ff0000` | `ff0000` |
| Fondo (borrado del EFB + copia a pantalla) | `0000ff` | `0000ff` (cuadro 0: negro, calentamiento de pipelines) |

Contadores: `cuadros=4 pases=12 dibujos_gx=20 indices_generados=4 copias_a_pantalla=4 texturas=1`,
`omitidos(pipeline=0 uniformes=0 otros=0)`, 51 520 bytes de uniformes reempaquetados. **Es corrección en PC, no
rendimiento**: no hay tiempos porque llvmpipe y el backend nulo de Dawn no dicen nada de la Mali-G31.

## Ruta directa (`galaxy_direct_mesa_test`)

| Comprobación | Resultado |
| --- | --- |
| Escena (cuadrado + triángulo con matriz) por la ruta directa vs. la misma escena en el formato de captura GX | **0 píxeles distintos de 307 200** |
| Segundo cuadro con otra matriz | el triángulo se mueve; geometría **no** reconstruida |
| Material con iluminación | rechazado con motivo ("iluminación por vértice: pendiente…") |
| Malla vacía / índices fuera de rango | rechazadas |

Coste de CPU de preparar y entregar el cuadro por la ruta directa (sin GPU), 30 cuadros cada uno:

| Dibujos por cuadro | Preparar + `capture_*` | `draw_frame` del núcleo (llvmpipe) |
| --- | --- | --- |
| 500 | 0,32 ms | 19,9 ms |
| 2000 | 0,80 ms | 66,0 ms |

Qué **no** mide esto: el coste de la ruta GX completa (FIFO + decodificación de Aurora) en el mismo escenario: la app
con Aurora ya enlaza, pero sin disco no llega a dibujar nada (B9). Sin esa referencia no se puede afirmar cuánto ahorra la ruta directa;
solo que su propio coste de preparación es bajo y lineal en el número de dibujos. La comparación GX vs. directa con la
misma escena, cámara, resolución, recursos y recorrido queda para la consola (procedimiento abajo).

## Listas GX → mallas (`galaxy_gx_display_list_test`)

Tira, abanico y cuadrado con posiciones S16 cuantizadas (frac 4), colores indexados RGBA8 y UV F32 directas: mismo
orden de triángulos que Aurora (`prepare_idx_buffer`), deduplicación de vértices, rechazo explícito de índices fuera
del arreglo, líneas y comandos de estado dentro de la lista.

## Otras

`renderer_select` (selector), `portmaster_launcher_syntax` (`bash -n`), `include_case_clean` (ningún `#include` que
dependa de no distinguir mayúsculas), `patches_verify` (commits y parches): pasan.

## Procedimiento en la consola (pendiente)

1. `GALAXY_RENDERER=gx`, `PETARI_FRAME_STATS=1`: anotar actualizaciones de lógica/s, cuadros GX procesados, imágenes
   presentadas, percentiles por cuadro (p50/p95/p99), tiempo de CPU de lógica y de preparación gráfica, memoria
   residente (`/proc/self/status` VmRSS) y de la Mali (`/sys/kernel/debug/mali0/gpu_memory` si existe).
2. Misma escena y recorrido grabado (`native/ROUTE_RECORDING.md`) con `GALAXY_RENDERER=direct GALAXY_DIRECT_SCENES=<escena>`.
3. Comparar capturas (`CHOYA_GX_VOLCADO`) píxel a píxel; separar la ganancia por quitar GX de la ganancia por bajar
   resolución o efectos (mismas opciones en ambas corridas).
4. Solo si la imagen coincide y el tiempo mejora, añadir la escena a `validated_direct_scenes()`.
