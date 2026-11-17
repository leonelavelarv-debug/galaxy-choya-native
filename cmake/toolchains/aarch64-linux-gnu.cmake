# galaxy-choya-native: compilación cruzada para la RG35XX H (Allwinner H700, Cortex-A53, Linux aarch64).
# Se usa dentro del contenedor de docker/Dockerfile.aarch64 (Debian 12: glibc 2.36, el mismo piso que usa Choya).
# Variables opcionales: GALAXY_SYSROOT (raíz del sysroot arm64), GALAXY_CLANG (clang de host, por defecto clang-18).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(_triple aarch64-linux-gnu)

if(DEFINED ENV{GALAXY_CLANG})
    set(_clang "$ENV{GALAXY_CLANG}")
else()
    set(_clang clang-18)
endif()
find_program(_cc NAMES ${_clang} clang REQUIRED)
find_program(_cxx NAMES ${_clang}++ clang++-18 clang++ REQUIRED)
set(CMAKE_C_COMPILER "${_cc}")
set(CMAKE_CXX_COMPILER "${_cxx}")
set(CMAKE_ASM_COMPILER "${_cc}")
set(CMAKE_C_COMPILER_TARGET ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_triple})

if(DEFINED ENV{GALAXY_SYSROOT})
    set(CMAKE_SYSROOT "$ENV{GALAXY_SYSROOT}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(PKG_CONFIG_LIBDIR "/usr/lib/${_triple}/pkgconfig:/usr/share/pkgconfig")
# pkg-config lee el entorno, no variables de CMake: sin esto encontraría las .pc de x86-64 del contenedor.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/${_triple}/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
# Bibliotecas multiarch de Debian (dpkg --add-architecture arm64) y nunca las de x86-64 del propio contenedor.
set(CMAKE_LIBRARY_ARCHITECTURE ${_triple})
set(CMAKE_IGNORE_PATH /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/include/x86_64-linux-gnu)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")

# Enlazador de LLVM (lld) y código para Cortex-A53. -O2 por omisión (Release lo pone CMake); LTO/PGO aparte.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a53")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a53")
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
