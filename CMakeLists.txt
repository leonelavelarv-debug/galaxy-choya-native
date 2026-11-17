# galaxy-choya-native: motor GLES de Choya (de work/choya), ruta directa, selector de backend y pruebas.
# La lógica del juego (MacioGalaxy) se compila aparte con scripts/build.sh (inyectando cmake/linux_port.cmake).
cmake_minimum_required(VERSION 3.25)
project(GalaxyChoyaNative LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    add_compile_options(-mcpu=cortex-a53)
endif()

set(GALAXY_WORK "${CMAKE_CURRENT_SOURCE_DIR}/work")
set(CHOYA_AURORA_DIR "${GALAXY_WORK}/choya/choya/data/aurora")
set(AURORA_INCLUDE "${GALAXY_WORK}/aurora/include")
foreach(_f "${CHOYA_AURORA_DIR}/choya_gx_core.cpp" "${AURORA_INCLUDE}/dolphin/gx/GXEnum.h")
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "Falta ${_f}: ejecuta python3 scripts/fetch_deps.py")
    endif()
endforeach()

# Núcleo GLES de Choya: independiente de Aurora salvo por los valores de GXEnum (de la Aurora única fijada).
# C++20 solo en este objetivo (lo pide el núcleo); el código propio queda en C++17.
add_library(choya_gx_core STATIC "${CHOYA_AURORA_DIR}/choya_gx_core.cpp")
target_include_directories(choya_gx_core PUBLIC "${CHOYA_AURORA_DIR}" "${AURORA_INCLUDE}")
target_compile_features(choya_gx_core PRIVATE cxx_std_20)
set_target_properties(choya_gx_core PROPERTIES CXX_STANDARD 20)
target_link_libraries(choya_gx_core PUBLIC dl pthread)

# Ruta directa (fase 2).
add_library(galaxy_direct STATIC direct/src/choya_direct.cpp direct/src/gx_display_list.cpp)
target_include_directories(galaxy_direct PUBLIC direct/include)
target_link_libraries(galaxy_direct PUBLIC choya_gx_core)

# Selector de backend (GALAXY_RENDERER) y su registro.
add_library(galaxy_renderer_select STATIC direct/src/renderer_select.cpp)
target_include_directories(galaxy_renderer_select PUBLIC direct/include)

include(CTest)
if(BUILD_TESTING)
    add_executable(galaxy_direct_mesa_test tests/direct_mesa_test.cpp)
    target_link_libraries(galaxy_direct_mesa_test PRIVATE galaxy_direct)
    add_test(NAME direct_mesa COMMAND galaxy_direct_mesa_test)
    set_tests_properties(direct_mesa PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless" SKIP_RETURN_CODE 2
                         PASS_REGULAR_EXPRESSION "PRUEBA DIRECTA OK")

    add_executable(galaxy_gx_display_list_test tests/gx_display_list_test.cpp)
    target_link_libraries(galaxy_gx_display_list_test PRIVATE galaxy_direct)
    add_test(NAME gx_display_list COMMAND galaxy_gx_display_list_test)

    add_executable(galaxy_renderer_select_test tests/renderer_select_test.cpp)
    target_link_libraries(galaxy_renderer_select_test PRIVATE galaxy_renderer_select)
    add_test(NAME renderer_select COMMAND galaxy_renderer_select_test)

    # Arnés original de Choya (motor_gles) compilado contra la Aurora única: confirma que el motor no depende de la
    # Aurora de BrawlRecompiled para dibujar.
    add_executable(choya_motor_harness "${GALAXY_WORK}/choya/tests/motor_gles/harness.cpp")
    target_link_libraries(choya_motor_harness PRIVATE choya_gx_core)
    set_target_properties(choya_motor_harness PROPERTIES CXX_STANDARD 20)
    add_test(NAME choya_motor_harness COMMAND choya_motor_harness)
    set_tests_properties(choya_motor_harness PROPERTIES ENVIRONMENT
        "EGL_PLATFORM=surfaceless;CHOYA_PRUEBA_VOLCADO=${CMAKE_CURRENT_BINARY_DIR}/cuadro.bin"
        SKIP_RETURN_CODE 2 PASS_REGULAR_EXPRESSION "PRUEBA OK")

    add_test(NAME portmaster_launcher_syntax COMMAND bash -n "${CMAKE_CURRENT_SOURCE_DIR}/portmaster/Galaxy-Choya.sh")
    add_test(NAME include_case_clean COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/include_case_test.py"
        "${GALAXY_WORK}/MacioGalaxy")
    add_test(NAME patches_verify COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch_deps.py" --verify)
endif()
