# Super Mario Galaxy — galaxy-choya-native (PortMaster)

Port nativo para la Anbernic RG35XX H: el código del juego (decompilación Petari adaptada por MacioGalaxy) está
compilado para aarch64; no hay emulador ni intérprete PowerPC. Dibuja con el motor GLES de Choya.

**Estado: compilación de desarrollo, sin validar en la consola.** Lee `docs/MEDICIONES.md` y `docs/BLOQUEOS.md` del
repositorio antes de esperar algo.

## Instalación

1. Copia `ROMS/ports/Galaxy-Choya.sh` a `SD1/ROMS/ports/` y la carpeta `ports/galaxy-choya/` a `SD1/ports/`.
2. Pon tu disco extraído en `ports/galaxy-choya/gamedata/RMGE01/` (ver `gamedata/README.txt`).
3. Opcional: el `assets.wlpk` que genera `python -m choya convert` junto al ejecutable (texturas ETC2 reducidas).
4. Abre "Galaxy-Choya" en Ports. Todo lo que pasa queda en `ports/galaxy-choya/log.txt`.

## Backend gráfico

`conf/renderer.txt`: `gx` (por defecto, ruta completa) o `direct` (ruta directa de Choya, solo en escenas validadas;
las demás siguen por gx). Variables: `GALAXY_RENDERER`, `GALAXY_DIRECT_SCENES`, `GALAXY_DIRECT_FORCE`.

## Controles (conf/controls.txt)

| Botón | Acción |
| --- | --- |
| Stick izquierdo | Mover |
| Stick derecho | Puntero (R3 centra) |
| A (abajo) | Saltar / aceptar |
| B (derecha), R2 | Disparar trozos de estrella / atrás |
| X (izquierda), R1 | Giro (agitar el Wiimote) |
| L2 | Agacharse / bomba (Z) |
| L1 | Centrar cámara (C) |
| Y (arriba) | Start del título (A+B) |
| Start / Select | Pausa (+) / Menos (−) |
| Menú | Menú Home del port |
| Mantener L3 + stick izquierdo | Inclinación del mando (bola estelar, mantarraya) |
| Select + Start | Salir (cierre normal del juego) |

Las acciones que en la Wii exigen mantener (cargar el giro, apuntar mientras te mueves) funcionan manteniendo el
botón; ver `native/CONTROLS.md` de MacioGalaxy para la lista completa y el remapeo desde el menú Home.

## Guardados y caché

`saves/` (partidas, ajustes y reportes de fallo; no son compatibles con las de la Wii) y `cache/`.

## Licencias

`LICENSES/`: Petari/MacioGalaxy (CC0), Aurora (MIT), Dawn (BSD-3) y atribuciones.
