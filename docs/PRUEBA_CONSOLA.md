# Prueba en la RG35XX H y criterio de cierre

Nada de esto se ha ejecutado todavía en la consola. Esta es la prueba que cierra la fase 1 (salida gráfica correcta y
estable por la ruta GX → Choya); el rendimiento se mide después, no antes.

## Preparación

1. Compilar y empaquetar (README, "Compilar para la consola"): `build.sh aarch64 app` debe terminar con
   `[elf] … aarch64, cargador /lib/ld-linux-aarch64.so.1 … GLIBC máx ≤ 2.36` y sin `FALLA`.
2. En la consola (SSH o terminal de KNULLI/muOS), antes de copiar nada:
   `ldd --version | head -1`, `ls /dev/dri /dev/fb0`, `ls /usr/lib/libmali* /usr/lib/libEGL* 2>/dev/null`.
   Si la glibc es menor que la que pide el binario, parar: hay que compilar con un sysroot más viejo (B10).
3. Copiar `out/portmaster/SD1/…` a la microSD y el disco propio extraído en
   `SD1/ports/galaxy-choya/gamedata/RMGE01/` (`files/` y `sys/`). Opcional: `assets.wlpk` convertido con Choya.

## Ejecución

Lanzar `Galaxy-Choya` desde Ports. Esperar al menos 2 minutos en el título y entrar al Observatorio si el mando
responde. Salir con Select+Start. Copiar `SD1/ports/galaxy-choya/log.txt` a la PC.

## Qué mirar en log.txt

| Línea | Esperado |
| --- | --- |
| `[galaxy-choya] … pantalla=fbdev` o `=sdl`, glibc, `/dev/dri` | coincide con lo visto en el paso 2 |
| `petari: graphics backend 8 (Aurora records; Choya presents)` | backend nulo de Aurora, Choya presenta |
| `[choya-pantalla] contexto GLES listo (<modo>, <ancho>x<alto>)` | 640x480 en la RG35XX H; nunca "no se pudo abrir" |
| `[choya-pantalla] en 10 s: … cuadros de lógica/s, … dibujados/s, … imágenes presentadas/s` | las tres cifras > 0 y estables; se anotan, no se juzgan aquí |
| `[choya-puente] cuadro N: … omitidos: …` | anotar los contadores distintos de cero (fase 5: qué falta para Galaxy) |
| `[galaxy-backend] escena …: gx (…)` | una línea por etapa; siempre `gx` (lista validada vacía) |
| `paquete: … texturas convertidas` | solo con `assets.wlpk` |
| `[choya-pantalla] hilo de pantalla detenido; contexto liberado` | al salir con Select+Start |
| `[galaxy-choya] salió con código 0` | salida limpia; la pantalla vuelve a EmulationStation |

## Criterio de cierre (todo a la vez)

1. El título y el Observatorio se ven sin pantallas negras ni parpadeo durante 2 minutos (foto o captura de la
   pantalla de la consola), con el mando moviendo a Mario.
2. `log.txt` sin `FALLA`, sin `abort` y sin "lectura del EFB … no devolvió las copias".
3. Las tres cifras de cuadros (lógica, dibujados, presentados) aparecen por separado y no son cero.
4. Select+Start devuelve al menú en menos de 3 s y el log tiene "contexto liberado"; un segundo arranque funciona.
5. Los contadores de omitidos del puente quedan anotados en docs/MEDICIONES.md para decidir la fase 5.

Si falla el punto 1 con `pantalla=sdl`, repetir con `echo fbdev > SD1/ports/galaxy-choya/conf/present.txt` (y al
revés). Ninguna cifra de FPS de esta prueba se publica como rendimiento: solo indica que el ciclo funciona.
