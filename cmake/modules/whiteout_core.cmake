# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# whiteout_core: the common types, vector maths, the DEFLATE codec and, off the
# web, the host helpers (threads, files, HTTP, game finders). Every other module
# builds on it; it is always built.

set(_whiteout_core_sources
    src/whiteout/common/bit_io.cpp
    src/whiteout/common/huffman_table.cpp
    src/whiteout/common/specular.cpp
    src/whiteout/common/text.cpp

    src/whiteout/common_types.cpp

    src/whiteout/utils/job_group.cpp
    src/whiteout/utils/timeline_semaphore.cpp
    src/whiteout/utils/vertex_buffer.cpp

    src/whiteout/vector_types.cpp
)

if(NOT EMSCRIPTEN)
    # Host-only helpers: native threading, disk I/O, OS discovery, HTTP.
    # Models and textures only depend on these via abstract interfaces, so
    # the WASM build provides its own implementations in bindings/wasm/.
    list(APPEND _whiteout_core_sources
        src/whiteout/utils/simple_thread_pool.cpp
        src/whiteout/utils/os_file_system.cpp
        src/whiteout/utils/simple_http_handler.cpp
        src/whiteout/utils/blizzard_game_finder.cpp
        src/whiteout/utils/cdn_product_finder.cpp)
endif()

# DEFLATE codec backend selection.
set(_whiteout_core_public_link "")
if(WHITEOUT_USE_ZLIBNG)
    # zlib-ng variant — the only TU that includes <zlib-ng.h>. Linked PUBLIC so
    # the dependency flows into WhiteoutLib's install/export interface (an
    # installed static lib must resolve zng_ symbols downstream). The target
    # carries its own include dirs (build/install interface) + ZLIBNG_NATIVE_API.
    list(APPEND _whiteout_core_sources src/whiteout/common/deflate_zlibng.cpp)
    list(APPEND _whiteout_core_public_link ${WHITEOUT_ZLIBNG_TARGET})
else()
    # Built-in codec — no external dependency.
    list(APPEND _whiteout_core_sources
        src/whiteout/common/deflate.cpp
        src/whiteout/storages/common/inflate_fast_builtin.cpp)
endif()

set(_whiteout_core_private_link "")
set(_whiteout_core_private_define "")
if(WIN32 AND NOT EMSCRIPTEN)
    # MSVC pulls winhttp via #pragma comment(lib, ...). MinGW/clang-mingw need
    # it linked explicitly.
    list(APPEND _whiteout_core_private_link winhttp)
elseif(NOT EMSCRIPTEN)
    # Non-Windows: SimpleHttpHandler can optionally use libcurl as its backend.
    # If libcurl headers are absent the class still compiles, but its methods
    # return an error response — apps that need real HTTP on these platforms
    # should provide their own HttpHandler implementation.
    option(WHITEOUT_ENABLE_CURL_HTTP
        "Enable libcurl backend for SimpleHttpHandler on non-Windows hosts" ON)
    if(WHITEOUT_ENABLE_CURL_HTTP)
        find_package(CURL)
        if(CURL_FOUND)
            list(APPEND _whiteout_core_private_link CURL::libcurl)
            list(APPEND _whiteout_core_private_define WHITEOUT_HAVE_CURL=1)
            message(STATUS "WhiteoutLib: SimpleHttpHandler using libcurl ${CURL_VERSION_STRING}")
        else()
            message(STATUS
                "WhiteoutLib: libcurl not found — SimpleHttpHandler will stub "
                "HTTP requests on this platform. Install libcurl-dev "
                "(libcurl4-openssl-dev on Debian/Ubuntu, libcurl-devel on RHEL) "
                "or provide your own HttpHandler implementation.")
        endif()
    endif()
endif()

whiteout_add_module(whiteout_core
    SOURCES ${_whiteout_core_sources}
    PUBLIC_LINK ${_whiteout_core_public_link}
    PRIVATE_LINK ${_whiteout_core_private_link}
    PRIVATE_DEFINE ${_whiteout_core_private_define})
