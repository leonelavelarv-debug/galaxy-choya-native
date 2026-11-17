# Atribuciones y licencias de terceros

Este repositorio no incluye código de terceros copiado: `scripts/fetch_deps.py` clona cada dependencia en
`third_party/` en el commit de `deps/lock.json`, y los cambios necesarios se guardan como parches en `patches/`.

| Proyecto | Autores | Licencia | Uso |
| --- | --- | --- | --- |
| [Petari](https://github.com/SMGCommunity/Petari) | SMGCommunity y colaboradores | CC0 1.0 | decompilación de Super Mario Galaxy (dentro de MacioGalaxy) |
| [MacioGalaxy](https://github.com/classicrob/MacioGalaxy) | classicrob y colaboradores | CC0 1.0 (LICENSE del repositorio) | port nativo de Petari: servicios, lectores, JAudio, entrada, parches de Aurora |
| [Choya Wii Converter](https://github.com/leonelavelarv-debug/choya-wii-converter) | leonelavelarv-debug | la del repositorio | motor GLES, conversor `assets.wlpk`, mando |
| [Aurora](https://github.com/encounter/aurora) | Luke Street y colaboradores | MIT | implementación de GX/VI usada en la fase 1 |
| [Dawn](https://dawn.googlesource.com/dawn) (cabeceras de `sin_dawn`, paquete precompilado en la PC) | The Dawn Authors | BSD-3-Clause | WebGPU de referencia en la PC |
| [LLVM libc++/libc++abi](https://github.com/llvm/llvm-project) | LLVM Project | Apache-2.0 WITH LLVM-exception | biblioteca estándar estática |
| SDL3, abseil, xxHash, fmt, Dear ImGui, Tracy, zstd | sus autores | zlib / Apache-2.0 / BSD-2 / MIT / MIT / BSD-3 / BSD | dependencias de Aurora |

**Datos del juego:** ninguno. Super Mario Galaxy es de Nintendo; el usuario aporta su propio disco extraído (RMGE01).
Las tablas BCSV que MacioGalaxy incrusta (`src/Game/System/*.bcsv`) vienen de su repositorio, no de este.

Este proyecto no está afiliado a Nintendo, a SMGCommunity/Petari ni a MacioGalaxy. Los problemas de este port no deben
reportarse a esos proyectos.
