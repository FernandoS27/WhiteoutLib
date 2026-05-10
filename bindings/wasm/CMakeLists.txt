# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Emscripten / Embind bindings for whiteout_lib.
# Builds whiteout.js + whiteout.wasm exposing models and textures to JS.

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "bindings/wasm requires the Emscripten toolchain (emcmake)")
endif()

add_executable(whiteout_wasm
    bindings.cpp
    mdx_bindings.cpp
    m2_bindings.cpp
    m3_bindings.cpp
    textures_bindings.cpp
    in_memory_fs.cpp
)

target_link_libraries(whiteout_wasm PRIVATE whiteout_lib)

# Embind is shipped with Emscripten; --bind links it in.
target_link_options(whiteout_wasm PRIVATE
    "--bind"
    "-fwasm-exceptions"
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=Whiteout"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=64MB"
    "-sMAXIMUM_MEMORY=2GB"
    "-sENVIRONMENT=web,node"
    "-sNO_EXIT_RUNTIME=1"
    "-sFILESYSTEM=0"
    # Make WebAssembly.Exception decodable from JS so the facade can
    # surface useful error messages instead of opaque WASM exceptions.
    "-sEXPORTED_RUNTIME_METHODS=getExceptionMessage,decrementExceptionRefcount"
)

target_compile_options(whiteout_wasm PRIVATE
    -fwasm-exceptions
)

# Place outputs at a predictable location for the JS package layer.
set_target_properties(whiteout_wasm PROPERTIES
    OUTPUT_NAME "whiteout"
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/wasm-dist"
)
