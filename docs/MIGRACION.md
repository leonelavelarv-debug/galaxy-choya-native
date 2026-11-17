# Componentes migrados y componentes que siguen por GX

Estado al 2026-11-17. La columna "Ruta GX" pasa por el puente `choya_bridge/` (Aurora 08122911 → núcleo de Choya),
probado en PC con Mesa; sus aproximaciones están en BLOQUEOS.md (B1). "Implementado" = código en el repositorio, compilado y con prueba. "Validado" = comparado con
la ruta GX en la RG35XX H con el juego (ninguno todavía).

| Componente gráfico | Ruta GX (fase 1) | Ruta directa (fase 2) | Validado en consola |
| --- | --- | --- | --- |
| Geometría estática J3D (tiras, abanicos, cuadrados, triángulos; atributos directos e indexados; posiciones/normales/colores/UV) | sí (Aurora) | implementado: `gx_display_list.cpp` + `j3d_shape_bridge.cpp` (compila contra J3D de Petari), prueba sintética | no |
| Mallas persistentes, materiales registrados, grupos de texturas | — | implementado: `choya_direct.cpp`, prueba en Mesa (0 píxeles distintos frente a la captura GX) | no |
| Materiales TEV sin iluminación (hasta 16 etapas, konstantes, registros, swap, alfa, mezcla, profundidad, cull) | sí | implementado (traducción a `PipelineDesc`) | no |
| Matrices de textura (texgen MTX2x4/MTX3x4) | sí | implementado | no |
| Skinning por índice de matriz (PNMTXIDX, hasta 10 matrices por dibujo, como GX) | sí | implementado en la malla y el uniforme; sin probar con modelos reales | no |
| Iluminación por vértice (canales con luz) | sí | **no**: se rechaza con motivo y sigue por GX | — |
| Niebla | sí | no (GX) | — |
| Texturas indirectas | sí | no (GX) | — |
| Texgen de relieve (bump) / SRTG | sí | no (GX) | — |
| Normales NBT | sí | no (GX) | — |
| Matrices de textura por vértice (TEXnMTXIDX) | sí | no (GX) | — |
| Líneas y puntos | sí | no (GX) | — |
| Animación de material (colores, SRT, patrones) | sí | los valores animados entran por `DrawParams` (colores, konstantes, matrices); falta conectarlos a J3D | no |
| Partículas (JPA) | sí | no (GX) | — |
| Interfaz (layouts nw4r) | sí | no (GX) | — |
| Sombras / efectos de profundidad | sí | no (GX) | — |
| Copias del EFB a textura | sí | API implementada (`copy_to_texture`), sin uso desde J3D | no |
| Lecturas del EFB (`GXPeek*`), `GXDrawDone`, `GXSetDrawSync` | sí (sync_bridge/efb_peek de MacioGalaxy) | no los emite: por eso direct solo se activa por escena | — |
| Vídeos (THP) | sí | no (GX) | — |

Selección: `GALAXY_RENDERER=gx|direct`, por escena (`direct/src/renderer_select.cpp`). La lista de escenas validadas
está **vacía**: la ruta directa no se activa por defecto en ninguna escena hasta medirla en la consola.
