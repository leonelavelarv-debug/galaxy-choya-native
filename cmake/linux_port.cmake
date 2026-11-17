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
        if(BUILD_TESTING)
            # J3DVertexData real (su constructor) con arreglos nativos y tamaños de mNativeArrayBytes.
            add_executable(galaxy_j3d_bridge_test "${GALAXY_ROOT}/tests/j3d_bridge_test.cpp"
                "${CMAKE_SOURCE_DIR}/src/JSystem/J3DGraphBase/J3DVertex.cpp")
            target_link_libraries(galaxy_j3d_bridge_test PRIVATE galaxy_direct_j3d)
            # Solo hace falta el constructor de J3DVertexData: el resto de J3DVertex.cpp (que usa j3dSys) se descarta.
            target_compile_options(galaxy_j3d_bridge_test PRIVATE -ffunction-sections -fdata-sections)
            target_link_options(galaxy_j3d_bridge_test PRIVATE -Wl,--gc-sections)
            add_test(NAME galaxy_j3d_bridge COMMAND galaxy_j3d_bridge_test)
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

# --- Puente Aurora única → motor GLES de Choya (choya_bridge/). Se aplica sobre las fuentes de Aurora tal como las
# dejan los parches de MacioGalaxy (la última versión de cada archivo en aurora_gx), y añade el puente y el núcleo de
# Choya al mismo objetivo. Solo actúa con la fase 1 (PETARI_BUILD_GX_PROBE=ON). Cada gancho es obligatorio.
option(GALAXY_CHOYA_BRIDGE "Puente de Aurora hacia el motor GLES de Choya (se activa en ejecución con CHOYA_GLES=1)" ON)
function(galaxy_add_choya_bridge)
    if(NOT GALAXY_CHOYA_BRIDGE OR NOT TARGET aurora_gx)
        return()
    endif()
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    get_target_property(_aurora_dir aurora_gx SOURCE_DIR)
    set(_bridge "${GALAXY_ROOT}/choya_bridge")
    set(_choya "${GALAXY_ROOT}/work/choya/choya/data/aurora")
    if(NOT EXISTS "${_choya}/choya_gx_core.cpp")
        message(FATAL_ERROR "galaxy-choya-native: falta ${_choya}/choya_gx_core.cpp (python3 scripts/fetch_deps.py)")
    endif()
    # Fuentes finales (ya con los parches de MacioGalaxy) de aurora_gx y aurora_core. Las de aurora_core son las de la
    # presentación sin superficie WebGPU cuando Choya presenta (gpu, aurora, window).
    foreach(_pair "aurora_gx:frame:lib/gfx/frame.cpp" "aurora_gx:recording:lib/gfx/recording.cpp"
            "aurora_gx:command_processor:lib/gx/command_processor.cpp" "aurora_gx:texture:lib/gfx/texture.cpp"
            "aurora_gx:framebuffer:lib/dolphin/gx/GXFrameBuffer.cpp" "aurora_gx:gxtexture:lib/gx/texture.cpp"
            "aurora_core:gpu:lib/webgpu/gpu.cpp"
            "aurora_core:aurora:lib/aurora.cpp" "aurora_core:window:lib/window.cpp")
        string(REPLACE ":" ";" _pair "${_pair}")
        list(GET _pair 0 _tgt)
        list(GET _pair 1 _kind)
        list(GET _pair 2 _rel)
        get_target_property(_sources ${_tgt} SOURCES)
        set(_found "")
        foreach(_s IN LISTS _sources)
            if(_s STREQUAL "${_rel}" OR _s MATCHES "/${_rel}$")
                set(_found "${_s}")
            endif()
        endforeach()
        if(NOT _found)
            # Algunos parches de MacioGalaxy dejan la fuente sin la ruta lib/... (p. ej. native/gx/geometry/frame.cpp):
            # se acepta por nombre si es la única con ese nombre fuera de lib/.
            get_filename_component(_name "${_rel}" NAME)
            set(_cands "")
            foreach(_s IN LISTS _sources)
                get_filename_component(_n "${_s}" NAME)
                if(_n STREQUAL "${_name}" AND NOT _s MATCHES "/lib/")
                    list(APPEND _cands "${_s}")
                endif()
            endforeach()
            list(LENGTH _cands _nc)
            if(_nc EQUAL 1)
                set(_found "${_cands}")
            else()
                message(FATAL_ERROR "galaxy-choya-native: no encontré ${_rel} en ${_tgt} (${_nc} candidatos)")
            endif()
        endif()
        set(_input "${_found}")
        if(NOT IS_ABSOLUTE "${_input}")
            set(_input "${_aurora_dir}/${_input}")
        endif()
        set(_output "${CMAKE_BINARY_DIR}/choya-bridge/${_rel}")
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${_bridge}/patch_aurora_bridge.py" ${_kind} "${_input}" "${_output}"
                        COMMAND_ERROR_IS_FATAL ANY)
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${_input}" "${_bridge}/patch_aurora_bridge.py")
        get_source_file_property(_opts "${_found}" TARGET_DIRECTORY ${_tgt} COMPILE_OPTIONS)
        if(NOT _opts)
            set(_opts "")
        endif()
        get_filename_component(_orig_dir "${_aurora_dir}/${_rel}" DIRECTORY)
        list(APPEND _opts "-iquote${_orig_dir}" "-iquote${_aurora_dir}/lib")
        list(REMOVE_ITEM _sources "${_found}")
        list(APPEND _sources "${_output}")
        set_property(TARGET ${_tgt} PROPERTY SOURCES "${_sources}")
        set_source_files_properties("${_output}" TARGET_DIRECTORY ${_tgt} PROPERTIES COMPILE_OPTIONS "${_opts}")
        message(STATUS "galaxy-choya-native: puente Choya en ${_rel}")
    endforeach()
    target_include_directories(aurora_core PRIVATE "${_bridge}")
    get_target_property(_sources aurora_gx SOURCES)
    list(APPEND _sources "${_bridge}/galaxy_choya_bridge.cpp" "${_bridge}/galaxy_choya_host.cpp"
         "${_choya}/choya_gx_core.cpp")
    set_property(TARGET aurora_gx PROPERTY SOURCES "${_sources}")
    set_source_files_properties("${_bridge}/galaxy_choya_bridge.cpp" "${_bridge}/galaxy_choya_host.cpp"
                                TARGET_DIRECTORY aurora_gx PROPERTIES COMPILE_OPTIONS "-iquote${_aurora_dir}/lib")
    target_include_directories(aurora_gx PRIVATE "${_bridge}" "${_choya}")
    target_link_libraries(aurora_gx PRIVATE dl)
    # Prueba de extremo a extremo: GX real → Aurora (backend nulo) → puente → núcleo en Mesa.
    add_executable(galaxy_bridge_gx_test EXCLUDE_FROM_ALL "${GALAXY_ROOT}/tests/bridge_gx_test.cpp")
    target_compile_features(galaxy_bridge_gx_test PRIVATE cxx_std_20)
    target_include_directories(galaxy_bridge_gx_test PRIVATE "${_bridge}" "${_choya}")
    target_link_libraries(galaxy_bridge_gx_test PRIVATE aurora::core aurora::gx aurora::vi petari_memory dl)
    # Integración del host de pantalla: el contexto lo crea el host (como en la app), no la prueba.
    add_executable(galaxy_host_gx_test EXCLUDE_FROM_ALL "${GALAXY_ROOT}/tests/host_gx_test.cpp")
    target_compile_features(galaxy_host_gx_test PRIVATE cxx_std_20)
    target_include_directories(galaxy_host_gx_test PRIVATE "${_bridge}" "${_choya}")
    target_link_libraries(galaxy_host_gx_test PRIVATE petari_platform_vi aurora::core aurora::gx aurora::vi petari_memory dl)
    # pbuffer con SDL "offscreen": sin servidor gráfico ni superficie WebGPU (como en la consola). 2 = omitida.
    add_test(NAME galaxy_host_gx_pbuffer COMMAND galaxy_host_gx_test pbuffer)
    set_tests_properties(galaxy_host_gx_pbuffer PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless"
                         SKIP_RETURN_CODE 2 PASS_REGULAR_EXPRESSION "PRUEBA HOST OK")
    # assets.wlpk por la ruta de la app: un paquete sintético con el formato del conversor de Choya.
    add_executable(galaxy_pack_gx_test EXCLUDE_FROM_ALL "${GALAXY_ROOT}/tests/pack_gx_test.cpp")
    target_compile_features(galaxy_pack_gx_test PRIVATE cxx_std_20)
    target_include_directories(galaxy_pack_gx_test PRIVATE "${_bridge}" "${_choya}")
    target_link_libraries(galaxy_pack_gx_test PRIVATE petari_platform_vi aurora::core aurora::gx aurora::vi petari_memory dl)
    add_test(NAME galaxy_pack_gx COMMAND galaxy_pack_gx_test)
    set_tests_properties(galaxy_pack_gx PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless"
                         SKIP_RETURN_CODE 2 PASS_REGULAR_EXPRESSION "PRUEBA PAQUETE OK")
    # Aurora necesita una ventana SDL aunque use el backend nulo de Dawn: en PC sin pantalla se usa xvfb-run.
    find_program(GALAXY_XVFB_RUN xvfb-run)
    if(GALAXY_XVFB_RUN)
      # sdl: contexto GLES de SDL sobre la ventana de Aurora (SDL_WINDOW_OPENGL), en X11 bajo xvfb.
      add_test(NAME galaxy_host_gx_sdl COMMAND ${GALAXY_XVFB_RUN} -a $<TARGET_FILE:galaxy_host_gx_test> sdl)
      set_tests_properties(galaxy_host_gx_sdl PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=x11"
                           SKIP_RETURN_CODE 2 PASS_REGULAR_EXPRESSION "PRUEBA HOST OK")
    endif()
    if(GALAXY_XVFB_RUN)
      add_test(NAME galaxy_bridge_gx COMMAND ${GALAXY_XVFB_RUN} -a $<TARGET_FILE:galaxy_bridge_gx_test>)
    else()
      add_test(NAME galaxy_bridge_gx COMMAND galaxy_bridge_gx_test)
    endif()
    set_tests_properties(galaxy_bridge_gx PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless;SDL_VIDEO_DRIVER=x11"
                         SKIP_RETURN_CODE 2 PASS_REGULAR_EXPRESSION "PRUEBA PUENTE OK")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL galaxy_add_choya_bridge)
