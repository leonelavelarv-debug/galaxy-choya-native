#!/bin/sh
# Compila una libc++ estática (con libc++abi dentro; el desenrollado es el de libgcc_s, el mismo que usa glibc en
# pthread_exit/pthread_cancel: mezclar libunwind de LLVM con libgcc_s rompe el desenrollado forzado) desde third_party/llvm-runtimes.
#   scripts/build_libcxx.sh <prefijo> [archivo-toolchain-cmake]
# Sin toolchain: para la PC (pruebas). Con cmake/toolchains/aarch64-linux-gnu.cmake: para la consola.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${1:?prefijo de instalación}
TOOLCHAIN=${2:-}
SRC="$ROOT/third_party/llvm-runtimes/runtimes"
[ -d "$SRC" ] || { echo "falta $SRC: ejecuta python3 scripts/fetch_deps.py" >&2; exit 1; }
B="$PREFIX.build"
set -- -S "$SRC" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
  -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
  -DLIBCXXABI_USE_LLVM_UNWINDER=OFF -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
  -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$PREFIX"
if [ -n "$TOOLCHAIN" ]; then
  cmake "$@" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
else
  cmake "$@" -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
fi
cmake --build "$B" --target install
echo "libc++ en $PREFIX"
