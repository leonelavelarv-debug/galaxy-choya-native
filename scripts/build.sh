#!/bin/sh
# Compila el port. Uso:
#   scripts/build.sh host      # PC Linux x86-64 (pruebas; libc++ propia en out/libcxx-host)
#   scripts/build.sh aarch64   # consola; dentro del contenedor de docker/Dockerfile.aarch64 (sysroot Debian 12)
# Variables: JOBS, BUILD_TYPE (Release), GALAXY_GX=ON para Aurora/GX (fase 1, ver docs/BLOQUEOS.md).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TARGET=${1:-host}
JOBS=${JOBS:-$(nproc)}
BUILD_TYPE=${BUILD_TYPE:-Release}
GALAXY_GX=${GALAXY_GX:-OFF}
if [ "$GALAXY_GX" = ON ]; then
  python3 "$ROOT/scripts/fetch_deps.py" --aurora-extern
else
  python3 "$ROOT/scripts/fetch_deps.py"
fi

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
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF -DSDL_JACK=OFF \
    -DSDL_SNDIO=OFF"
  [ "$TARGET" = aarch64 ] && GX_ARGS="$GX_ARGS -DSDL_KMSDRM=ON -DSDL_ALSA=ON"
fi
# shellcheck disable=SC2086
cmake -S "$ROOT/work/MacioGalaxy" -B "$B/macio" -G Ninja $TOOLCHAIN_ARGS $GX_ARGS \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBUILD_TESTING=ON \
  -DCMAKE_PROJECT_PetariNative_INCLUDE="$ROOT/cmake/linux_port.cmake" \
  -DGALAXY_LIBCXX_ROOT="$LIBCXX"
cmake --build "$B/macio" -j "$JOBS"

# Motor GLES de Choya, adaptador y API directa (independientes de Aurora).
# shellcheck disable=SC2086
cmake -S "$ROOT" -B "$B/choya" -G Ninja $TOOLCHAIN_ARGS -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$B/choya" -j "$JOBS"
echo "listo: $B"
