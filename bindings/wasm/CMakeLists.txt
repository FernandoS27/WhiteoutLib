# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Emscripten / Embind bindings for whiteout_lib.
#
# Two parallel targets (both built from the same source tree):
#
#   whiteout_wasm       — minimum-footprint web+node build. Single-threaded,
#                          no filesystem, no native I/O. Output:
#                          wasm-dist/whiteout.{js,wasm}.
#
#   whiteout_wasm_node  — Node.js-only build. NODERAWFS for direct disk
#                          access via std::filesystem / std::ifstream,
#                          pthreads for std::thread-backed parallelism
#                          (mipmap gen, BC7, CASC/MPQ decode). Output:
#                          wasm-dist/whiteout-node.{js,wasm}. Opt-in via
#                          -DWHITEOUT_BUILD_WASM_NODE_BINDINGS=ON.

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "bindings/wasm requires the Emscripten toolchain (emcmake)")
endif()

# ----- Shared sources -------------------------------------------------------
# Both targets compile these. `in_memory_fs.cpp` is web-only — the Node
# target gets the OS-backed VirtualPathFileSystem via OsFileSystem.
set(_WHITEOUT_WASM_BASE_SOURCES
    bindings.cpp
    mdx_bindings.cpp
    m2_bindings.cpp
    m3_bindings.cpp
    textures_bindings.cpp
    utils_bindings.cpp
)
if(WHITEOUT_ENABLE_MPQ)
    list(APPEND _WHITEOUT_WASM_BASE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/mpq_bindings.cpp)
endif()

# ============================================================================
# whiteout_wasm — web+node, single-threaded, FILESYSTEM=0
# ============================================================================
add_executable(whiteout_wasm
    ${_WHITEOUT_WASM_BASE_SOURCES}
    in_memory_fs.cpp
)
target_link_libraries(whiteout_wasm PRIVATE whiteout_lib)
if(WHITEOUT_ENABLE_MPQ)
    target_link_libraries(whiteout_wasm PRIVATE whiteout_mpq)
    target_compile_definitions(whiteout_wasm PRIVATE WHITEOUT_HAS_MPQ=1)
endif()

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
    "-sEXPORTED_RUNTIME_METHODS=getExceptionMessage,decrementExceptionRefcount"
)
target_compile_options(whiteout_wasm PRIVATE -fwasm-exceptions)
set_target_properties(whiteout_wasm PROPERTIES
    OUTPUT_NAME "whiteout"
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/wasm-dist"
)

# ============================================================================
# whiteout_wasm_node — Node-only, NODERAWFS + pthreads
# ============================================================================
# WHITEOUT_BUILD_WASM_NODE_BINDINGS is declared in the root CMakeLists so
# that the EMSCRIPTEN-block can branch on it (whether to force CASC OFF).
set(WHITEOUT_WASM_NODE_PTHREAD_POOL_SIZE "8" CACHE STRING
    "Pre-spawned pthread (worker_thread) pool size for whiteout_wasm_node.")

if(WHITEOUT_BUILD_WASM_NODE_BINDINGS)
    # The host-only utilities normally excluded from whiteout_lib under
    # EMSCRIPTEN — they rely on std::thread / std::filesystem, both of
    # which work under Node-flavoured emscripten with -pthread + NODERAWFS.
    # We compile them straight into the executable so whiteout_lib stays
    # unchanged (and the web build keeps its slim surface).
    set(_WHITEOUT_NODE_HOST_SOURCES
        ${CMAKE_SOURCE_DIR}/src/whiteout/utils/simple_thread_pool.cpp
        ${CMAKE_SOURCE_DIR}/src/whiteout/utils/os_file_system.cpp
    )

    add_executable(whiteout_wasm_node
        ${_WHITEOUT_WASM_BASE_SOURCES}
        node_bindings.cpp
        ${_WHITEOUT_NODE_HOST_SOURCES}
    )
    # Link the pthread-flavoured `_mt` variants (declared in the root
    # CMakeLists). Mixing pthread + non-pthread .o files in a link that
    # requests --shared-memory is rejected by wasm-ld, so the Node
    # target must use the matching pthread builds end-to-end.
    target_link_libraries(whiteout_wasm_node PRIVATE whiteout_lib_mt)
    # Gates the InMemoryFileSystem class binding and its `#include` out
    # of the shared bindings.cpp — the Node build uses OsFileSystem and
    # doesn't need the in-memory shim.
    target_compile_definitions(whiteout_wasm_node PRIVATE
        WHITEOUT_WASM_NODE_BUILD=1)
    if(WHITEOUT_ENABLE_MPQ)
        target_link_libraries(whiteout_wasm_node PRIVATE whiteout_mpq_mt)
        target_compile_definitions(whiteout_wasm_node PRIVATE WHITEOUT_HAS_MPQ=1)
    endif()
    if(WHITEOUT_ENABLE_CASC)
        target_link_libraries(whiteout_wasm_node PRIVATE whiteout_casc_mt)
        target_compile_definitions(whiteout_wasm_node PRIVATE WHITEOUT_HAS_CASC=1)
    endif()

    target_link_options(whiteout_wasm_node PRIVATE
        "--bind"
        "-fwasm-exceptions"
        "-pthread"
        "-sMODULARIZE=1"
        "-sEXPORT_ES6=1"
        "-sEXPORT_NAME=WhiteoutNode"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=128MB"
        "-sMAXIMUM_MEMORY=4GB"
        "-sENVIRONMENT=node,worker"
        "-sNO_EXIT_RUNTIME=1"
        "-sNODERAWFS=1"
        "-sPTHREAD_POOL_SIZE=${WHITEOUT_WASM_NODE_PTHREAD_POOL_SIZE}"
        "-sEXPORTED_RUNTIME_METHODS=getExceptionMessage,decrementExceptionRefcount"
    )
    target_compile_options(whiteout_wasm_node PRIVATE
        -fwasm-exceptions
        -pthread
    )
    set_target_properties(whiteout_wasm_node PROPERTIES
        OUTPUT_NAME "whiteout-node"
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/wasm-dist"
    )
endif()
