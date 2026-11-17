# galaxy-choya-native: "Dawn" para AURORA_DAWN_PROVIDER=system cuando GALAXY_WEBGPU_NULO=ON.
# dawn::webgpu_dawn es el WebGPU nulo de Choya (sin GPU, sin Dawn): Aurora graba sus cuadros y el motor GLES de Choya
# los dibuja. Lo usa la compilación de la consola (no hay Dawn para la Mali) y, para probarlo, la de la PC.
if(TARGET dawn::webgpu_dawn)
    return()
endif()
if(NOT GALAXY_ROOT OR NOT GALAXY_DAWN_HEADERS)
    message(FATAL_ERROR "galaxy-choya-native: DawnConfig nulo sin GALAXY_ROOT/GALAXY_DAWN_HEADERS")
endif()
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(_sin_dawn "${GALAXY_ROOT}/work/choya/choya/data/aurora/sin_dawn")
set(_out "${CMAKE_BINARY_DIR}/webgpu-nulo")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${GALAXY_ROOT}/nulo/generate.py" "${_sin_dawn}" "${GALAXY_DAWN_HEADERS}"
                        "${_out}" COMMAND_ERROR_IS_FATAL ANY)
add_library(galaxy_webgpu_nulo STATIC "${_out}/webgpu_nulo.cpp" "${GALAXY_ROOT}/nulo/dawn_native_nulo.cpp")
target_include_directories(galaxy_webgpu_nulo PUBLIC "${GALAXY_DAWN_HEADERS}")
target_compile_features(galaxy_webgpu_nulo PUBLIC cxx_std_20)
set_property(TARGET galaxy_webgpu_nulo PROPERTY POSITION_INDEPENDENT_CODE ON)
add_library(dawn::webgpu_dawn ALIAS galaxy_webgpu_nulo)
set(Dawn_FOUND TRUE)
message(STATUS "galaxy-choya-native: WebGPU nulo de Choya en lugar de Dawn (${GALAXY_DAWN_HEADERS})")
