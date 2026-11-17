#!/bin/sh
# Compila el port. Uso:
#   scripts/build.sh host [libs]   # PC x86-64: bibliotecas y pruebas (libc++ propia en out/libcxx-host)
#   scripts/build.sh host app      # PC: ejecutable completo (Dawn de referencia; GALAXY_WEBGPU_NULO=ON para el nulo)
#   scripts/build.sh aarch64 app   # consola: ejecutable distribuible (WebGPU nulo, sin Dawn) + comprobación ELF
#   scripts/build.sh aarch64 libs  # consola: bibliotecas y pruebas (para correrlas en la consola)
# aarch64 se compila dentro del contenedor de docker/Dockerfile.aarch64 (Debian 12, glibc 2.36).
# Variables: JOBS, BUILD_TYPE (Release), GALAXY_GX=ON (libs con Aurora/GX), GALAXY_WEBGPU_NULO, GALAXY_GLIBC_MAX.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TARGET=${1:-host}
MODE=${2:-libs}
JOBS=${JOBS:-$(nproc)}
BUILD_TYPE=${BUILD_TYPE:-Release}
case "$MODE" in
  libs) GALAXY_GX=${GALAXY_GX:-OFF} ;;
  app) GALAXY_GX=ON ;;  # el ejecutable siempre lleva Aurora, el puente y el host de pantalla
  *) echo "modo desconocido: $MODE (libs | app)" >&2; exit 2 ;;
esac
if [ "$TARGET" = aarch64 ] && [ "$MODE" = app ]; then
  GALAXY_WEBGPU_NULO=ON  # en la consola no hay Dawn: obligatorio
fi
GALAXY_WEBGPU_NULO=${GALAXY_WEBGPU_NULO:-OFF}
FETCH="--aurora-extern"
[ "$GALAXY_GX" = ON ] || FETCH=""
[ "$GALAXY_WEBGPU_NULO" = ON ] && FETCH="$FETCH --dawn-headers"
# shellcheck disable=SC2086
python3 "$ROOT/scripts/fetch_deps.py" $FETCH

case "$TARGET" in
  host)
    LIBCXX="$ROOT/out/libcxx-host"
    [ -f "$LIBCXX/lib/libc++.a" ] || sh "$ROOT/scripts/build_libcxx.sh" "$LIBCXX"
    TOOLCHAIN_ARGS="-DCMAKE_C_COMPILER=${CC:-clang} -DCMAKE_CXX_COMPILER=${CXX:-clang++}"
    ;;
  aarch64)
    LIBCXX="$ROOT/out/libcxx-aarch64"
    TC="$ROOT/cmake/toolchains/aarch64-linux-gnu.cmake"
    [ -f "$LIBCXX/lib/libc++.a" ] || sh "$ROOT/scripts/build_libcxx.sh" "$LIBCXX" "$TC"
    TOOLCHAIN_ARGS="-DCMAKE_TOOLCHAIN_FILE=$TC"
    ;;
  *) echo "objetivo desconocido: $TARGET" >&2; exit 2 ;;
esac

B="$ROOT/build/$TARGET"
GX_ARGS=""
if [ "$GALAXY_GX" = ON ]; then
  # Fase 1: Aurora única (work/aurora) y sus dependencias fijadas, sin red al configurar.
  E="$ROOT/third_party/aurora-extern"
  GX_ARGS="-DPETARI_BUILD_GX_PROBE=ON -DFETCHCONTENT_SOURCE_DIR_AURORA=$ROOT/work/aurora -DAURORA_SDL3_PROVIDER=vendor \
    -DFETCHCONTENT_SOURCE_DIR_ABSEIL-CPP=$E/abseil-cpp -DFETCHCONTENT_SOURCE_DIR_XXHASH=$E/xxhash \
    -DFETCHCONTENT_SOURCE_DIR_FMT=$E/fmt -DFETCHCONTENT_SOURCE_DIR_IMGUI=$E/imgui -DFETCHCONTENT_SOURCE_DIR_TRACY=$E/tracy \
    -DFETCHCONTENT_SOURCE_DIR_SDL=$E/SDL -DFETCHCONTENT_SOURCE_DIR_ZSTD=$E/zstd \
    -DSDL_WAYLAND=OFF -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF"
  if [ "$TARGET" = aarch64 ]; then
    # Consola: KMS/DRM + ALSA, sin servidor gráfico.
    GX_ARGS="$GX_ARGS -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_KMSDRM=ON -DSDL_ALSA=ON -DGALAXY_CHOYA_BRIDGE=ON"
  else
    # PC: X11 mínimo, solo para que la prueba del puente cree su ventana bajo xvfb-run.
    GX_ARGS="$GX_ARGS -DSDL_UNIX_CONSOLE_BUILD=OFF -DSDL_X11=ON -DSDL_X11_XCURSOR=OFF -DSDL_X11_XDBE=OFF \
      -DSDL_X11_XFIXES=OFF -DSDL_X11_XINPUT=OFF -DSDL_X11_XRANDR=OFF -DSDL_X11_XSCRNSAVER=OFF -DSDL_X11_XSHAPE=OFF \
      -DSDL_X11_XSYNC=OFF -DSDL_X11_XTEST=OFF -DGALAXY_CHOYA_BRIDGE=ON"
  fi
  GX_ARGS="$GX_ARGS -DGALAXY_WEBGPU_NULO=$GALAXY_WEBGPU_NULO"
fi

if [ "$MODE" = app ]; then
  # Distribuible: solo el ejecutable y lo que necesita (sin pruebas), en su propia carpeta.
  # shellcheck disable=SC2086
  cmake -S "$ROOT/work/MacioGalaxy" -B "$B/app" -G Ninja $TOOLCHAIN_ARGS $GX_ARGS \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBUILD_TESTING=OFF \
    -DCMAKE_PROJECT_PetariNative_INCLUDE="$ROOT/cmake/linux_port.cmake" \
    -DGALAXY_LIBCXX_ROOT="$LIBCXX"
  cmake --build "$B/app" -j "$JOBS" --target petari
  BIN="$B/app/native/app/Petari"
  [ -x "$BIN" ] || { echo "no se generó $BIN" >&2; exit 1; }
  if [ "$TARGET" = aarch64 ]; then
    # Las de --allow van en libs.aarch64/ del port (package_portmaster.py --libs), copiadas del sysroot arm64.
    python3 "$ROOT/scripts/check_elf.py" --arch aarch64 --glibc-max "${GALAXY_GLIBC_MAX:-2.36}" \
      --allow "${GALAXY_ELF_ALLOW:-libsqlite3.so.0,libpng16.so.16,libfreetype.so.6,libz.so.1}" "$BIN"
  else
    python3 "$ROOT/scripts/check_elf.py" --arch x86_64 --glibc-max "${GALAXY_GLIBC_MAX:-2.39}" \
      --allow "${GALAXY_ELF_ALLOW:-libsqlite3.so.0,libpng16.so.16,libfreetype.so.6,libz.so.1}" "$BIN"
  fi
  echo "ejecutable: $BIN"
  exit 0
fi
# shellcheck disable=SC2086
cmake -S "$ROOT/work/MacioGalaxy" -B "$B/macio" -G Ninja $TOOLCHAIN_ARGS $GX_ARGS \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBUILD_TESTING=ON \
  -DCMAKE_PROJECT_PetariNative_INCLUDE="$ROOT/cmake/linux_port.cmake" \
  -DGALAXY_LIBCXX_ROOT="$LIBCXX"
cmake --build "$B/macio" -j "$JOBS"
if [ "$GALAXY_GX" = ON ]; then
  cmake --build "$B/macio" -j "$JOBS" --target petari
  # Prueba del puente Aurora → Choya (solo PC: necesita Mesa con EGL sin superficie).
  [ "$TARGET" = host ] && cmake --build "$B/macio" -j "$JOBS" --target galaxy_bridge_gx_test
fi

# Motor GLES de Choya, adaptador y API directa (independientes de Aurora).
# shellcheck disable=SC2086
cmake -S "$ROOT" -B "$B/choya" -G Ninja $TOOLCHAIN_ARGS -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$B/choya" -j "$JOBS"
echo "listo: $B"
