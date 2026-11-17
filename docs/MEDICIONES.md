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
| Aurora + app `petari` (`PETARI_BUILD_GX_PROBE=ON`) | ver `docs/BLOQUEOS.md` (B3) |

## Pruebas de MacioGalaxy en Linux (`ctest -LE "timing|slow"`)

59 de 59 pasan, entre ellas: núcleo (matemáticas, Nerve, RNG), memoria y heaps, archivos JKR, recursos J3D/KCL/JPA,
mensajes, NAND (con atributos en xattr o archivo lateral), DVD, OS (planificador con testigo de CPU, preempción,
alarmas), VI, audio (DSP/AI), sincronía GX, fallos (informe con registros), entrada, editor. La prueba lenta
`native_array_index_audit` (150 s) también pasa. Las de etiqueta `timing` no se ejecutaron (necesitan una máquina
quieta; esta es virtual y compartida).

Las pruebas que necesitan el disco (`--assets build/game-data/RMGE01/files`) no se registran sin datos: **no se
ejecutaron**.

## Motor de Choya con la Aurora única

`choya_motor_harness` (la prueba original de Choya) compilado contra `GXEnum.h` de Aurora `08122911`: **PRUEBA OK**
(colores, copias del EFB, invalidaciones, volcado y redibujo del cuadro).

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

Qué **no** mide esto: el coste de la ruta GX completa (FIFO + decodificación de Aurora) en el mismo escenario, porque
la app con Aurora todavía no corre aquí (B3). Sin esa referencia no se puede afirmar cuánto ahorra la ruta directa;
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
