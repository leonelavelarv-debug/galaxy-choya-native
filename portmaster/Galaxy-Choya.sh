#!/bin/bash
# galaxy-choya-native: lanzador PortMaster de Super Mario Galaxy (port nativo, Petari/MacioGalaxy + motor GLES de Choya).
# Va en SD1/ROMS/ports/Galaxy-Choya.sh; el port en SD1/ports/galaxy-choya/.
# No incluye datos del juego: el usuario pone su disco extraído (RMGE01) en galaxy-choya/gamedata/RMGE01/.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

# shellcheck source=/dev/null
source "$controlfolder/control.txt"
# shellcheck source=/dev/null
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

# Carpeta del port: SD1/ports/galaxy-choya (KNULLI la expone como /roms/ports/galaxy-choya o /userdata/roms/ports).
GAMEDIR="/$directory/ports/galaxy-choya"
[ -d "$GAMEDIR" ] || GAMEDIR="$(cd "$(dirname "$0")/../../ports/galaxy-choya" 2>/dev/null && pwd)"
cd "$GAMEDIR" || exit 1

# log.txt desde el principio: también los errores de arranque.
LOG="$GAMEDIR/log.txt"
: > "$LOG"
exec > >(tee -a "$LOG") 2>&1
echo "[galaxy-choya] $(date) CFW=${CFW_NAME:-?} DEVICE=${DEVICE_NAME:-?} pantalla=${DISPLAY_WIDTH}x${DISPLAY_HEIGHT}"

# Datos aportados por el usuario.
DISC="$GAMEDIR/gamedata/RMGE01"
if [ ! -d "$DISC/files" ] && [ ! -d "$DISC/DATA/files" ]; then
  echo "[galaxy-choya] ERROR: falta el disco extraído en gamedata/RMGE01 (files/ y sys/). Ver README.md del port."
  pm_message "Falta gamedata/RMGE01: copia tu disco extraído (ver README)." 2>/dev/null
  sleep 5
  pm_finish 2>/dev/null
  exit 1
fi

# Rutas propias: configuración, caché y guardados dentro de la carpeta del port.
mkdir -p "$GAMEDIR/conf" "$GAMEDIR/cache" "$GAMEDIR/saves"
# Perfil de mando de la RG35XX H la primera vez (MacioGalaxy lee controls.txt de su carpeta de usuario).
[ -f "$GAMEDIR/saves/controls.txt" ] || cp "$GAMEDIR/controls-rg35xxh.txt" "$GAMEDIR/saves/controls.txt"
export HOME="$GAMEDIR/conf"
export XDG_CONFIG_HOME="$GAMEDIR/conf"
export XDG_CACHE_HOME="$GAMEDIR/cache"
export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH:-aarch64}:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Backend gráfico: gx (por defecto) o direct (solo escenas validadas; ver README). Se puede fijar en conf/renderer.txt.
if [ -z "$GALAXY_RENDERER" ] && [ -f "$GAMEDIR/conf/renderer.txt" ]; then
  GALAXY_RENDERER="$(head -n1 "$GAMEDIR/conf/renderer.txt" | tr -d '\r ')"
fi
export GALAXY_RENDERER="${GALAXY_RENDERER:-gx}"
export CHOYA_GLES=1
# Paquete de texturas convertidas por Choya (opcional): si falta, se usan las texturas originales del disco.
if [ -f "$GAMEDIR/assets.wlpk" ]; then
  export CHOYA_PAQUETE="$GAMEDIR/assets.wlpk"
fi
echo "[galaxy-choya] backend=$GALAXY_RENDERER paquete=${CHOYA_PAQUETE:-ninguno}"

# Mando: gptokeyb solo para la salida limpia (Select+Start mata el proceso con SIGTERM, que el juego atiende).
$GPTOKEYB "galaxy-choya" -c "$GAMEDIR/galaxy-choya.gptk" &

# Sin overclock forzado. Si el CFW deja ajustes temporales (gobernador), se restauran al salir.
GOV_FILE=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
GOV_BEFORE=""
[ -r "$GOV_FILE" ] && GOV_BEFORE="$(cat "$GOV_FILE")"
restore() {
  if [ -n "$GOV_BEFORE" ] && [ -w "$GOV_FILE" ] && [ "$(cat "$GOV_FILE")" != "$GOV_BEFORE" ]; then
    echo "$GOV_BEFORE" > "$GOV_FILE"
  fi
  pm_finish 2>/dev/null
}
trap restore EXIT

pm_platform_helper "$GAMEDIR/galaxy-choya" 2>/dev/null
"$GAMEDIR/galaxy-choya" --disc "$DISC" --user "$GAMEDIR/saves"
status=$?
echo "[galaxy-choya] salió con código $status"
exit $status
