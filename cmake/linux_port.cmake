# galaxy-choya-native: se inyecta en el proyecto de MacioGalaxy (PetariNative) justo después de su project(), con
#   -DCMAKE_PROJECT_PetariNative_INCLUDE=<repo>/cmake/linux_port.cmake
# Así el CMake de MacioGalaxy queda intacto salvo los parches versionados de patches/MacioGalaxy.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

get_filename_component(GALAXY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
message(STATUS "galaxy-choya-native: port Linux de MacioGalaxy (${CMAKE_SYSTEM_PROCESSOR})")

# --- libc++ (MacioGalaxy la exige: native/include/petari/game_compat.hpp). Estática para no depender de la
# libstdc++ del firmware (KNULLI/muOS traen versiones distintas).
set(GALAXY_LIBCXX_ROOT "" CACHE PATH "Prefijo de una libc++ estática (include/c++/v1, lib/libc++.a)")
if(GALAXY_LIBCXX_ROOT)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>"
                        "$<$<COMPILE_LANGUAGE:CXX>:-isystem${GALAXY_LIBCXX_ROOT}/include/c++/v1>")
    add_link_options(-stdlib=libc++ -L${GALAXY_LIBCXX_ROOT}/lib -static-libstdc++ -pthread)
    # libc++.a incluye libc++abi (LIBCXX_ENABLE_STATIC_ABI_LIBRARY); el desenrollado es libgcc_s, como el de glibc.
else()
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>")
    add_link_options(-stdlib=libc++ -pthread)
endif()
# Aurora usa std::jthread/std::stop_token (C++20). En libc++ 18 siguen tras -fexperimental-library (libc++experimental.a).
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>")
add_link_options(-fexperimental-library)

# --- Código de CPU. Cortex-A53 (Allwinner H700 de la RG35XX H). Sin -ffast-math: la lógica del juego depende de la
# semántica IEEE (MacioGalaxy ya compila con -ffp-contract=off).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    add_compile_options(-mcpu=cortex-a53)
endif()
# El planificador de MacioGalaxy recorre la pila por punteros de marco para preempción forzada y diagnósticos
# (en arm64 de Apple siempre existen; en Linux hay que pedirlos).
add_compile_options(-fno-omit-frame-pointer $<$<CXX_COMPILER_ID:Clang>:-mno-omit-leaf-frame-pointer>)
# dladdr de Darwin también encuentra los símbolos no exportados del ejecutable (tabla LC_SYMTAB); el de glibc solo los
# de la tabla dinámica. El planificador nombra hilos y clasifica código con dladdr, así que se envuelve: si glibc no
# encuentra nombre, la capa Darwin lo busca en la .symtab del ejecutable (ver darwin_compat.cpp).
add_link_options(-Wl,--wrap=dladdr)

option(GALAXY_LTO "LTO (ThinLTO) en el binario final" OFF)
if(GALAXY_LTO)
    add_compile_options(-flto=thin)
    add_link_options(-flto=thin -fuse-ld=lld)
endif()
set(GALAXY_PGO_PROFILE "" CACHE FILEPATH "Perfil .profdata para PGO (scripts/pgo.sh)")
option(GALAXY_PGO_GENERATE "Instrumentar para PGO" OFF)
if(GALAXY_PGO_GENERATE)
    add_compile_options(-fprofile-generate)
    add_link_options(-fprofile-generate)
elseif(GALAXY_PGO_PROFILE)
    add_compile_options(-fprofile-use=${GALAXY_PGO_PROFILE} -Wno-profile-instr-unprofiled)
    add_link_options(-fprofile-use=${GALAXY_PGO_PROFILE})
endif()

# --- Capa Darwin → Linux. Las cabeceras van primero en la búsqueda de <mach/mach.h>, <pthread/qos.h>, etc.
add_library(galaxy_darwin_compat STATIC "${GALAXY_ROOT}/compat/darwin/src/darwin_compat.cpp")
target_include_directories(galaxy_darwin_compat PUBLIC "${GALAXY_ROOT}/compat/darwin/include")
target_link_libraries(galaxy_darwin_compat PUBLIC pthread dl)
include_directories(BEFORE "${GALAXY_ROOT}/compat/darwin/include")
# La biblioteca se enlaza al final de la configuración solo a los objetivos de MacioGalaxy (no a los de Aurora y sus
# dependencias, que se exportan con install(EXPORT) y no deben depender de ella).

# --- Opciones de enlace de ld64 (macOS) en los CMakeLists de MacioGalaxy. Al terminar la configuración se traducen
# en todos los objetivos a su equivalente GNU/LLD: -dead_strip → --gc-sections (con secciones por función) y los
# símbolos de -u pierden el '_' inicial de Mach-O.
add_compile_options(-ffunction-sections -fdata-sections)
function(galaxy_translate_darwin_link_options dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()
        get_target_property(_srcdir ${_t} SOURCE_DIR)
        if(NOT _t STREQUAL "galaxy_darwin_compat" AND _type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|OBJECT_LIBRARY|MODULE_LIBRARY)$"
           AND _srcdir MATCHES "^${CMAKE_SOURCE_DIR}" AND NOT _srcdir MATCHES "/_deps/")
            target_link_libraries(${_t} PRIVATE galaxy_darwin_compat)
        endif()
        foreach(_prop LINK_OPTIONS INTERFACE_LINK_OPTIONS)
            get_target_property(_opts ${_t} ${_prop})
            if(_opts)
                set(_new)
                foreach(_o IN LISTS _opts)
                    if(_o STREQUAL "-Wl,-dead_strip")
                        set(_o "-Wl,--gc-sections")
                    elseif(_o MATCHES "^-Wl,-u,_(.+)$")
                        set(_o "-Wl,-u,${CMAKE_MATCH_1}")
                    elseif(_o MATCHES "^-Wl,-(exported_symbol|sectcreate|stack_size|pagezero_size)")
                        message(FATAL_ERROR "galaxy-choya-native: opción de ld64 sin traducir en ${_t}: ${_o}")
                    endif()
                    list(APPEND _new "${_o}")
                endforeach()
                set_target_properties(${_t} PROPERTIES ${_prop} "${_new}")
            endif()
        endforeach()
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_d IN LISTS _subdirs)
        galaxy_translate_darwin_link_options("${_d}")
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL galaxy_translate_darwin_link_options "${CMAKE_SOURCE_DIR}")

# --- Ruta directa: puente J3D (compilado con las cabeceras y la configuración de MacioGalaxy).
function(galaxy_add_direct_j3d)
    if(TARGET petari_native_config AND NOT TARGET galaxy_direct_j3d)
        add_library(galaxy_direct_j3d STATIC
            "${GALAXY_ROOT}/direct/src/j3d_shape_bridge.cpp" "${GALAXY_ROOT}/direct/src/gx_display_list.cpp")
        target_include_directories(galaxy_direct_j3d PUBLIC "${GALAXY_ROOT}/direct/include")
        target_link_libraries(galaxy_direct_j3d PUBLIC petari_native_config)
        if(TARGET aurora_gx)
            target_link_libraries(galaxy_direct_j3d PRIVATE aurora::gx)
        endif()
    endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL galaxy_add_direct_j3d)

# El paquete precompilado de Dawn (solo referencia en la PC, AURORA_DAWN_PROVIDER=package) está compilado con
# libstdc++. Su API es C (webgpu.h), así que convive con la libc++ del port: se añade libstdc++ al final de la línea de
# enlace solo en la compilación de la PC con Aurora. En la consola no hay Dawn (docs/BLOQUEOS.md, B2).
if(PETARI_BUILD_GX_PROBE AND NOT CMAKE_CROSSCOMPILING)
    # Ruta completa (con -stdlib=libc++ clang no busca en la carpeta de GCC). Compartida, no estática: la estática
    # duplica los símbolos de libsupc++ que ya da la libc++abi del port.
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -stdlib=libstdc++ -print-file-name=libstdc++.so
                    OUTPUT_VARIABLE _galaxy_libstdcxx OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT EXISTS "${_galaxy_libstdcxx}")
        message(FATAL_ERROR "galaxy-choya-native: el Dawn precompilado de la PC necesita libstdc++.so (no encontrada)")
    endif()
    set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} ${_galaxy_libstdcxx}")
endif()
