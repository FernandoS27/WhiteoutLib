cmake_minimum_required(VERSION 3.15)
project(WhiteoutLib VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Determine if we're built as a subproject (using add_subdirectory)
# or if this is the master project.
set(WHITEOUT_MASTER_PROJECT OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(WHITEOUT_MASTER_PROJECT ON)
endif()

option(WHITEOUT_ENABLE_CASC "Enable CASC archive support via CascLib" OFF)
option(WHITEOUT_BUILD_EXAMPLES "Build example programs" OFF)
option(WHITEOUT_BUILD_TESTS "Build test programs" OFF)
option(WHITEOUT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${WHITEOUT_MASTER_PROJECT})
option(WHITEOUT_BUILD_WASM_BINDINGS "Build Emscripten/Embind JavaScript bindings" ${EMSCRIPTEN})
option(WHITEOUT_BUILD_WASM_NODE_BINDINGS
    "Build the Node.js-flavoured Emscripten target (NODERAWFS + pthreads)." OFF)
option(WHITEOUT_BUILD_PYTHON_BINDINGS "Build native Python bindings via pybind11" OFF)
option(WHITEOUT_BUILD_C_BINDINGS "Build the flat-C ABI shared library (whiteout_c)" OFF)
option(WHITEOUT_BUILD_C_STATIC
       "Also build whiteout_c as a static library (whiteout_c_static). Rust links this by default." OFF)
option(WHITEOUT_BUILD_JNI_BINDINGS
    "Build the Java JNI bridge (whiteout_jni) for callback-direction interop" OFF)
option(WHITEOUT_ENABLE_CLANG_TIDY "Run clang-tidy on whiteout sources during build" OFF)
option(WHITEOUT_USE_ZLIBNG
    "Use zlib-ng for the DEFLATE codec instead of the built-in implementation" OFF)
option(WHITEOUT_USE_SYSTEM_ZLIBNG
    "Link an external zlib-ng via find_package instead of building from source (requires WHITEOUT_USE_ZLIBNG)" OFF)
option(WHITEOUT_ZLIBNG_SHARED
    "Build the bundled zlib-ng as a shared library instead of static" OFF)
set(WHITEOUT_ZLIBNG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/externals/zlib-ng"
    CACHE PATH "Path to the zlib-ng source tree to build (bundled submodule by default)")
# Declared here (used early by the zlib-ng wiring); consumed by the install
# section near the end of this file.
option(WHITEOUT_INSTALL
    "Generate install/export rules for find_package(WhiteoutLib)" ON)

# Always emit compile_commands.json — drives clangd, clang-tidy, IWYU, ...
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

if(EMSCRIPTEN)
    # WASM build covers models + textures. CASC stays off by default (it
    # pulls threading, HTTP, disk discovery — none of which are useful in
    # a browser). MPQ is opt-in via the caller (`-DWHITEOUT_ENABLE_MPQ=ON`)
    # because the JS bindings expose `Storage::create()` for in-memory
    # archive construction even when `-sFILESYSTEM=0` blocks path-based
    # `Storage::open(path)`.
    #
    # The Node-flavoured build (`-DWHITEOUT_BUILD_WASM_NODE_BINDINGS=ON`)
    # opts in to pthreads + NODERAWFS and *can* link CASC, so we only
    # force CASC OFF for the plain web build. The Node target honours
    # whatever the caller passed for `WHITEOUT_ENABLE_CASC`.
    if(NOT WHITEOUT_BUILD_WASM_NODE_BINDINGS)
        set(WHITEOUT_ENABLE_CASC OFF CACHE BOOL "" FORCE)
    endif()
    set(WHITEOUT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHITEOUT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHITEOUT_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
endif()

# Build a parallel `*_mt` variant of every whiteout library when the
# Node-flavoured WASM target is requested. Emscripten's pthread support
# changes the WASM ABI (shared memory + atomics), so a single archive
# can't satisfy both a non-pthread link (whiteout_wasm, web target) and
# a pthread link (whiteout_wasm_node). The two variants are otherwise
# identical — same sources, same public headers — with `-pthread`
# applied to the `_mt` form only.
#
# Sub-projects (bindings/wasm) check WHITEOUT_BUILD_WASM_NODE_BINDINGS
# and link against the `_mt` targets when it's ON.
function(whiteout_define_mt_variant target)
    if(NOT WHITEOUT_BUILD_WASM_NODE_BINDINGS OR NOT EMSCRIPTEN)
        return()
    endif()
    get_target_property(src     ${target} SOURCES)
    get_target_property(incdir  ${target} INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(deps    ${target} LINK_LIBRARIES)
    get_target_property(pubdefs ${target} INTERFACE_COMPILE_DEFINITIONS)
    get_target_property(prvdefs ${target} COMPILE_DEFINITIONS)
    add_library(${target}_mt STATIC ${src})
    if(incdir)
        target_include_directories(${target}_mt PUBLIC ${incdir})
    endif()
    if(pubdefs)
        target_compile_definitions(${target}_mt PUBLIC ${pubdefs})
    endif()
    if(prvdefs)
        target_compile_definitions(${target}_mt PRIVATE ${prvdefs})
    endif()
    if(deps)
        foreach(d ${deps})
            # Re-target inter-whiteout deps to the _mt equivalents so the
            # pthread ABI stays consistent across the dep graph.
            # Skip whiteout_strict_flags: it gets re-added PRIVATE on the
            # _mt variant below. Promoting it to PUBLIC here would leak
            # `-fno-exceptions` (and the rest of the warning posture) into
            # every consumer — bindings/wasm/* throws std::runtime_error
            # and breaks under -fno-exceptions.
            if(d STREQUAL "whiteout_strict_flags")
                continue()
            endif()
            if(TARGET ${d}_mt)
                target_link_libraries(${target}_mt PUBLIC ${d}_mt)
            else()
                target_link_libraries(${target}_mt PUBLIC ${d})
            endif()
        endforeach()
    endif()
    target_link_libraries(${target}_mt PRIVATE whiteout_strict_flags)
    target_compile_options(${target}_mt PUBLIC -pthread)
endfunction()

if(WHITEOUT_BUILD_TESTS)
    enable_testing()
endif()

if(MSVC)
    add_compile_options(/FS)
endif()

# =============================================================================
# Strict-warning interface library (modelled after Kepler3D).
# Only WhiteoutLib's own targets link this — externals (Catch2, CascLib) keep
# their own warning posture.
# =============================================================================
add_library(whiteout_strict_flags INTERFACE)

if(MSVC)
    # cl.exe and clang-cl both use MSVC-style flags here.
    # The library is exception-free — disable EH for all whiteout TUs.
    target_compile_options(whiteout_strict_flags INTERFACE
        /EHs-c-
        /W4
        /w34263 # Non-virtual member function hides base class virtual function
        /w44265 # Class has virtual functions, but destructor is not virtual
        /w34456 # Declaration of 'var' hides previous local declaration
        /w34457 # Declaration of 'var' hides function parameter
        /w34458 # Declaration of 'var' hides class member
        /w34459 # Declaration of 'var' hides global definition
        /w34946 # Reinterpret-cast between related types
        /wd4201 # Anonymous struct/union — used intentionally in vector_types
        /wd4592 # Symbol will be dynamically initialized (implementation limitation)
        /wd4702 # Unreachable code — constexpr-if false positives in templates
        /permissive- # Stricter C++ standards conformance
        /Zc:throwingNew # Assumes new never returns null
        /Zc:inline      # Omits inline functions from object-file output
        # Treat <angle-bracket> includes as external (system headers) and
        # silence warnings emanating from them — STL internals trigger
        # template-instantiation noise we can't fix in our code.
        /external:W0
        /external:anglebrackets
    )
    target_compile_definitions(whiteout_strict_flags INTERFACE
        NOMINMAX
        # MSVC STL warns on these but the alternatives are no safer.
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
        # Match /EHs-c- — tell the MSVC STL that exceptions are disabled so it
        # avoids EH-dependent paths (e.g. some _Throw_ helpers become abort()).
        _HAS_EXCEPTIONS=0
    )

    if(WHITEOUT_WARNINGS_AS_ERRORS)
        target_compile_options(whiteout_strict_flags INTERFACE /WX)
    endif()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # clang-cl driver — accept MSVC flags but trim noise from extra args
        # propagated through the build system.
        target_compile_options(whiteout_strict_flags INTERFACE
            -Qunused-arguments
            -Wno-missing-braces
        )
    endif()
else()
    target_compile_options(whiteout_strict_flags INTERFACE
        # The library is exception-free — disable EH for all whiteout TUs.
        -fno-exceptions
        -Wall
        -Wextra
        -Wcast-qual
        -pedantic
        -pedantic-errors
        -Wfatal-errors
        -Wno-missing-braces
        # Mirror MSVC's /wd4201 — vector_types and texture internals use
        # anonymous structs/unions intentionally for .x/.y/.z accessors.
        # Clang silences the warning via these flags; GCC has no per-warning
        # toggle, so the offending unions are prefixed with __extension__
        # at each site (GCC accepts that as "I know this is non-standard").
        -Wno-gnu-anonymous-struct
        -Wno-nested-anon-types
    )

    if(WHITEOUT_WARNINGS_AS_ERRORS)
        target_compile_options(whiteout_strict_flags INTERFACE -Werror)
        # GCC trips -Warray-bounds / -Wstringop-overflow / -Wmaybe-uninitialized
        # false positives inside libstdc++ vector / shared_ptr / variant inlines
        # at -O2/-O3 (well-known upstream issue). Keep them as warnings so the
        # build survives them across GCC versions.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(whiteout_strict_flags INTERFACE
                -Wno-error=array-bounds
                -Wno-error=stringop-overflow
                -Wno-error=maybe-uninitialized
                -Wno-error=free-nonheap-object
            )
        endif()
    endif()
endif()

# =============================================================================
# clang-tidy integration
#
# whiteout_apply_clang_tidy(<target>) sets CXX_CLANG_TIDY on <target> when
# WHITEOUT_ENABLE_CLANG_TIDY is ON and clang-tidy is found. External targets
# (Catch2, CascLib, zlib, ...) are deliberately left untouched.
#
# Disabled automatically with the clang-cl driver — CMake's
# __run_co_compile wrapper feeds clang-tidy a mangled invocation that
# trips clang-cl's "expected exactly one compiler job" check. On Windows,
# use the standalone flow against compile_commands.json instead:
#   clang-tidy -p build/clang src/whiteout/...
# =============================================================================
set(WHITEOUT_CLANG_TIDY_COMMAND "")
if(WHITEOUT_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(NOT CLANG_TIDY_EXE)
        message(WARNING "WHITEOUT_ENABLE_CLANG_TIDY=ON but clang-tidy not found in PATH")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND MSVC)
        message(STATUS "clang-tidy found (${CLANG_TIDY_EXE}) but in-build "
                       "integration disabled for clang-cl — run standalone "
                       "via compile_commands.json instead")
    else()
        message(STATUS "clang-tidy enabled: ${CLANG_TIDY_EXE}")
        set(WHITEOUT_CLANG_TIDY_COMMAND
            "${CLANG_TIDY_EXE}"
            "--use-color=false"
            "--quiet")
    endif()
endif()

function(whiteout_apply_clang_tidy target)
    if(WHITEOUT_CLANG_TIDY_COMMAND)
        set_target_properties(${target} PROPERTIES
            CXX_CLANG_TIDY "${WHITEOUT_CLANG_TIDY_COMMAND}")
    endif()
endfunction()

# =============================================================================
# zlib-ng — OPTIONAL DEFLATE backend (the single allowed third-party dependency).
# Enabled with WHITEOUT_USE_ZLIBNG=ON, in which case it backs
# whiteout::zlib_compress/decompress via common/deflate_zlibng.cpp (the only TU
# that includes <zlib-ng.h>). When OFF (default), the in-house codec
# (common/deflate.cpp + storages/common/inflate_fast_builtin.cpp) is used and no
# external dependency is pulled in.
#
# When enabled, two supply modes converge on find_package(zlib-ng) for
# *installed* consumers:
#   • From source (default) — built from WHITEOUT_ZLIBNG_SOURCE_DIR (the bundled
#     externals/zlib-ng submodule unless overridden). When WHITEOUT_INSTALL is
#     on it is installed alongside WhiteoutLib (its own zlib-ng CONFIG package)
#     so an installed WhiteoutLib resolves zng_ symbols. Static by default;
#     set WHITEOUT_ZLIBNG_SHARED=ON for a shared/DLL build.
#   • System (WHITEOUT_USE_SYSTEM_ZLIBNG=ON) — find_package(zlib-ng), e.g. the
#     vcpkg port, which declares zlib-ng as its own package dependency. Here the
#     external package decides static vs shared.
# The library target is captured in WHITEOUT_ZLIBNG_TARGET and linked PUBLIC so
# the dependency flows into WhiteoutLib's install/export interface; consumers
# rediscover it via find_dependency(zlib-ng) in WhiteoutLibConfig.cmake.
# =============================================================================
if(WHITEOUT_USE_ZLIBNG)
if(WHITEOUT_USE_SYSTEM_ZLIBNG)
    find_package(zlib-ng CONFIG REQUIRED)
    # Honour the requested linkage when the package exposes both; otherwise take
    # whichever target it provides.
    if(WHITEOUT_ZLIBNG_SHARED AND TARGET zlib-ng::zlib)
        set(WHITEOUT_ZLIBNG_TARGET zlib-ng::zlib)
    elseif(TARGET zlib-ng::zlibstatic)
        set(WHITEOUT_ZLIBNG_TARGET zlib-ng::zlibstatic)
    elseif(TARGET zlib-ng::zlib)
        set(WHITEOUT_ZLIBNG_TARGET zlib-ng::zlib)
    else()
        message(FATAL_ERROR
            "find_package(zlib-ng) succeeded but neither zlib-ng::zlibstatic "
            "nor zlib-ng::zlib target exists — check the zlib-ng package.")
    endif()
else()
    if(NOT EXISTS "${WHITEOUT_ZLIBNG_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "zlib-ng source not found at WHITEOUT_ZLIBNG_SOURCE_DIR=\n"
            "  ${WHITEOUT_ZLIBNG_SOURCE_DIR}\n"
            "Run `git submodule update --init --recursive` for the bundled copy, "
            "point WHITEOUT_ZLIBNG_SOURCE_DIR at your own zlib-ng tree, or "
            "configure with -DWHITEOUT_USE_SYSTEM_ZLIBNG=ON to use an installed one.")
    endif()

    # Shared zlib-ng is meaningless under Emscripten (no DLLs) — force static.
    set(_zlibng_shared ${WHITEOUT_ZLIBNG_SHARED})
    if(EMSCRIPTEN AND _zlibng_shared)
        message(STATUS "WhiteoutLib: ignoring WHITEOUT_ZLIBNG_SHARED under Emscripten (static only)")
        set(_zlibng_shared OFF)
    endif()

    set(ZLIB_COMPAT      OFF CACHE BOOL "" FORCE) # native zng_ API
    set(WITH_OPTIM       ON  CACHE BOOL "" FORCE)
    set(WITH_GZFILEOP    ON  CACHE BOOL "" FORCE)
    set(BUILD_TESTING    OFF CACHE BOOL "" FORCE) # library only — no zlib-ng tests/utils
    set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_GTEST       OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARKS  OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARK_APPS OFF CACHE BOOL "" FORCE)
    set(INSTALL_UTILS    OFF CACHE BOOL "" FORCE)
    # Install the bundled zlib-ng only when WhiteoutLib itself installs, so the
    # transitive find_dependency(zlib-ng) downstream resolves.
    if(WHITEOUT_INSTALL)
        set(SKIP_INSTALL_ALL OFF CACHE BOOL "" FORCE)
    else()
        set(SKIP_INSTALL_ALL ON  CACHE BOOL "" FORCE)
    endif()

    # Drive zlib-ng's linkage via BUILD_SHARED_LIBS, scoped so WhiteoutLib's own
    # target type is left untouched. An explicit binary dir lets
    # WHITEOUT_ZLIBNG_SOURCE_DIR point outside the project tree.
    if(DEFINED BUILD_SHARED_LIBS)
        set(_whiteout_saved_bsl "${BUILD_SHARED_LIBS}")
        set(_whiteout_had_bsl ON)
    else()
        set(_whiteout_had_bsl OFF)
    endif()
    set(BUILD_SHARED_LIBS ${_zlibng_shared})
    add_subdirectory(${WHITEOUT_ZLIBNG_SOURCE_DIR} ${CMAKE_BINARY_DIR}/zlib-ng)
    if(_whiteout_had_bsl)
        set(BUILD_SHARED_LIBS "${_whiteout_saved_bsl}")
    else()
        unset(BUILD_SHARED_LIBS)
    endif()

    # zlib-ng always defines `zlib-ng-static`: a real STATIC target in a shared
    # build, or an alias to the static `zlib-ng` otherwise. In a shared build the
    # `zlib-ng` target itself is the DLL.
    if(_zlibng_shared)
        set(WHITEOUT_ZLIBNG_TARGET zlib-ng)
    else()
        set(WHITEOUT_ZLIBNG_TARGET zlib-ng-static)
    endif()
endif()
endif() # WHITEOUT_USE_ZLIBNG

# Whiteout Library
add_library(whiteout_lib
    # Common
    src/whiteout/common_types.cpp
    src/whiteout/utils/vertex_buffer.cpp
    src/whiteout/utils/job_group.cpp
    src/whiteout/utils/timeline_semaphore.cpp
    src/whiteout/vector_types.cpp
    src/whiteout/common/bit_io.cpp
    src/whiteout/common/huffman_table.cpp

    # MDX
    src/whiteout/models/mdx/parser.cpp
    src/whiteout/models/mdx/writer.cpp
    src/whiteout/models/mdx/mdl_tokenizer.cpp
    src/whiteout/models/mdx/mdl_parser.cpp
    src/whiteout/models/mdx/mdl_converter.cpp
    src/whiteout/models/mdx/mdl_writer.cpp

    # M2
    src/whiteout/models/m2/parser.cpp
    src/whiteout/models/m2/chunk_parser.cpp
    src/whiteout/models/m2/writer.cpp
    src/whiteout/models/m2/binary_parse_visitor/base.cpp
    src/whiteout/models/m2/binary_parse_visitor/chunk.cpp
    src/whiteout/models/m2/binary_parse_visitor/extensions.cpp
    src/whiteout/models/m2/binary_parse_visitor/skin.cpp
    src/whiteout/models/m2/binary_parse_visitor/bone.cpp
    src/whiteout/models/m2/binary_parse_visitor/anim.cpp
    src/whiteout/models/m2/binary_writer_visitor/base.cpp
    src/whiteout/models/m2/binary_writer_visitor/chunk.cpp
    src/whiteout/models/m2/binary_writer_visitor/extensions.cpp
    src/whiteout/models/m2/binary_writer_visitor/skin.cpp
    src/whiteout/models/m2/binary_writer_visitor/bone.cpp
    src/whiteout/models/m2/binary_writer_visitor/anim.cpp
    src/whiteout/models/m2/file_system.cpp
    src/whiteout/models/m2/sequence_loader.cpp
    src/whiteout/models/m2/wow_file_system.cpp

    # M3
    src/whiteout/models/m3/types.cpp
    src/whiteout/models/m3/parser.cpp
    src/whiteout/models/m3/writer.cpp
    src/whiteout/models/m3/binary_parse_visitor.cpp
    src/whiteout/models/m3/binary_writer_visitor.cpp

    # WEM
    src/whiteout/models/wem/parser.cpp
    src/whiteout/models/wem/writer.cpp
    src/whiteout/models/wem/binary_parse_visitor.cpp
    src/whiteout/models/wem/binary_writer_visitor.cpp
    src/whiteout/models/wem/converter_registry.cpp
    src/whiteout/models/wem/converters/mdx_converter.cpp
    src/whiteout/models/wem/converters/m2_converter.cpp
    src/whiteout/models/wem/converters/m3_converter.cpp

    # Textures
    src/whiteout/textures/texture.cpp
    src/whiteout/textures/dds/parser.cpp
    src/whiteout/textures/dds/writer.cpp
    src/whiteout/textures/blp/parser.cpp
    src/whiteout/textures/blp/writer.cpp
    src/whiteout/textures/tex/parser.cpp
    src/whiteout/textures/tex/d4_parser.cpp
    src/whiteout/textures/tex/writer.cpp
    src/whiteout/textures/tex/tex_common.cpp
    src/whiteout/textures/d2r_texture/parser.cpp
    src/whiteout/textures/d2r_texture/writer.cpp
    src/whiteout/textures/bmp/parser.cpp
    src/whiteout/textures/bmp/writer.cpp
    src/whiteout/textures/tga/parser.cpp
    src/whiteout/textures/tga/writer.cpp
    src/whiteout/textures/tiff/parser.cpp
    src/whiteout/textures/tiff/writer.cpp
    src/whiteout/textures/tiff/packbits.cpp
    src/whiteout/textures/tiff/lzw.cpp
    src/whiteout/textures/gif/writer.cpp
    src/whiteout/textures/utils/quantize.cpp
    src/whiteout/textures/utils/blue_noise.cpp
    src/whiteout/textures/utils/pixel_convert.cpp
    src/whiteout/textures/utils/color_convert.cpp
    src/whiteout/textures/utils/srgb_linearize.cpp
    src/whiteout/textures/bcn.cpp
    src/whiteout/textures/bcn/bc1.cpp
    src/whiteout/textures/bcn/bc2.cpp
    src/whiteout/textures/bcn/bc3.cpp
    src/whiteout/textures/bcn/bc4.cpp
    src/whiteout/textures/bcn/bc5.cpp
    src/whiteout/textures/bcn/bc6h.cpp
    src/whiteout/textures/bcn/bc7.cpp
    src/whiteout/textures/bcn/bcn_common.cpp
    src/whiteout/textures/mipmap/filters.cpp
    src/whiteout/textures/mipmap/stages.cpp
    src/whiteout/textures/mipmap/generator.cpp
    src/whiteout/textures/jpeg/jpeg_decode.cpp
    src/whiteout/textures/jpeg/jpeg_encode.cpp
    src/whiteout/textures/jpeg/parser.cpp
    src/whiteout/textures/jpeg/writer.cpp
    # DEFLATE codec backend (deflate.cpp/deflate_zlibng.cpp) added conditionally
    # below, keyed on WHITEOUT_USE_ZLIBNG.
    src/whiteout/textures/png/parser.cpp
    src/whiteout/textures/png/writer.cpp

    # Diablo IV
    src/whiteout/sno/sno_types.cpp
    src/whiteout/sno/core_toc.cpp
    src/whiteout/sno/d4/sno_defs.cpp
    src/whiteout/sno/sno_value.cpp
    src/whiteout/sno/sno_reader.cpp

    # Diablo III
    src/whiteout/sno/d3/sno_defs.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/actor.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/anim.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/appearance.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/base.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/material.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/particle.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/physics.cpp
    src/whiteout/sno/d3/native/d3_native.cpp
)

if(NOT EMSCRIPTEN)
    # Host-only helpers: native threading, disk I/O, OS discovery, HTTP.
    # Models and textures only depend on these via abstract interfaces, so
    # the WASM build provides its own implementations in bindings/wasm/.
    target_sources(whiteout_lib PRIVATE
        src/whiteout/utils/simple_thread_pool.cpp
        src/whiteout/utils/os_file_system.cpp
        src/whiteout/utils/simple_http_handler.cpp
        src/whiteout/utils/blizzard_game_finder.cpp
        src/whiteout/utils/cdn_product_finder.cpp
    )
endif()

target_include_directories(whiteout_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(whiteout_lib PRIVATE whiteout_strict_flags)

# DEFLATE codec backend selection.
if(WHITEOUT_USE_ZLIBNG)
    # zlib-ng variant — the only TU that includes <zlib-ng.h>. Linked PUBLIC so
    # the dependency flows into WhiteoutLib's install/export interface (an
    # installed static lib must resolve zng_ symbols downstream). The target
    # carries its own include dirs (build/install interface) + ZLIBNG_NATIVE_API.
    target_sources(whiteout_lib PRIVATE src/whiteout/common/deflate_zlibng.cpp)
    target_link_libraries(whiteout_lib PUBLIC ${WHITEOUT_ZLIBNG_TARGET})
else()
    # Built-in codec — no external dependency.
    target_sources(whiteout_lib PRIVATE
        src/whiteout/common/deflate.cpp
        src/whiteout/storages/common/inflate_fast_builtin.cpp)
endif()

whiteout_apply_clang_tidy(whiteout_lib)
# These static libraries are linked into shared consumers — the C ABI's
# libwhiteout_native.so, the pybind11 module, the WASM build. On ELF
# targets that requires position-independent code: without it the linker
# rejects a TLS relocation (R_X86_64_TPOFF32) against a thread_local such
# as casc's s_lastError. Cheap on x86-64, and MSVC ignores the property.
set_property(TARGET whiteout_lib PROPERTY POSITION_INDEPENDENT_CODE ON)


if(WIN32 AND NOT EMSCRIPTEN)
    # MSVC pulls winhttp via #pragma comment(lib, ...). MinGW/clang-mingw need
    # it linked explicitly.
    target_link_libraries(whiteout_lib PRIVATE winhttp)
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
            target_link_libraries(whiteout_lib PRIVATE CURL::libcurl)
            target_compile_definitions(whiteout_lib PRIVATE WHITEOUT_HAVE_CURL=1)
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

whiteout_define_mt_variant(whiteout_lib)

# Binding subdirectories are added at the END of this file, after
# whiteout_casc / whiteout_mpq are defined so the pthread `_mt` variants
# exist before bindings/wasm/CMakeLists tries to link against them.

if(WHITEOUT_BUILD_EXAMPLES)

# Example Program
add_executable(mdx_loader_example
    examples/mdx_loader_example.cpp
)

target_link_libraries(mdx_loader_example PRIVATE whiteout_lib)

# Writer Example Program
add_executable(mdx_writer_example
    examples/mdx_writer_example.cpp
)

target_link_libraries(mdx_writer_example PRIVATE whiteout_lib)

# M2 Loader Example Program
add_executable(m2_loader_example
    examples/m2_loader_example.cpp
)

target_link_libraries(m2_loader_example PRIVATE whiteout_lib)

# M2 Writer Example Program
add_executable(m2_writer_example
    examples/m2_writer_example.cpp
)

target_link_libraries(m2_writer_example PRIVATE whiteout_lib)

# M3 Loader Example Program
add_executable(m3_loader_example
    examples/m3_loader_example.cpp
)

target_link_libraries(m3_loader_example PRIVATE whiteout_lib)

# M3 Writer Example Program
add_executable(m3_writer_example
    examples/m3_writer_example.cpp
)

target_link_libraries(m3_writer_example PRIVATE whiteout_lib)

# Texture Format Converter Example
add_executable(texture_convert_example
    examples/texture_convert_example.cpp
    examples/texture_converter.cpp
)

target_link_libraries(texture_convert_example PRIVATE whiteout_lib)

# Blue Noise Map Generator (prints std::array for blue_noise.cpp)
add_executable(blue_noise_generator
    examples/blue_noise_generator.cpp
)

target_link_libraries(blue_noise_generator PRIVATE whiteout_lib)

# WEM Round-Trip Test
add_executable(wem_round_trip_example
    examples/wem_round_trip_example.cpp
)

target_link_libraries(wem_round_trip_example PRIVATE whiteout_lib)

# Blizzard Game Finder Example
add_executable(game_finder_example
    examples/game_finder_example.cpp
)

target_link_libraries(game_finder_example PRIVATE whiteout_lib)

# MDX to WEM Converter Example
add_executable(mdx_to_wem_example
    examples/mdx_to_wem_example.cpp
)

target_link_libraries(mdx_to_wem_example PRIVATE whiteout_lib)

# M2 to WEM Converter Example
add_executable(m2_to_wem_example
    examples/m2_to_wem_example.cpp
)

target_link_libraries(m2_to_wem_example PRIVATE whiteout_lib)

# M3 to WEM Converter Example
add_executable(m3_to_wem_example
    examples/m3_to_wem_example.cpp
)

target_link_libraries(m3_to_wem_example PRIVATE whiteout_lib)

endif() # WHITEOUT_BUILD_EXAMPLES

# =============================================================================
# =============================================================================
# New CASC support (pure C++ implementation)
# =============================================================================
option(WHITEOUT_ENABLE_CASC "Enable CASC archive support" ON)

if(WHITEOUT_ENABLE_CASC)
    set(CASC_SOURCES
        src/whiteout/storages/common/mapped_file.cpp
        src/whiteout/storages/common/jenkins.cpp
        src/whiteout/storages/common/md5.cpp
        src/whiteout/storages/common/codecs/bzip2.cpp
        src/whiteout/storages/common/codecs/lzma.cpp
        src/whiteout/storages/casc/codec/crypto.cpp
        src/whiteout/storages/casc/codec/blte.cpp
        src/whiteout/storages/casc/tables/config.cpp
        src/whiteout/storages/casc/tables/index.cpp
        src/whiteout/storages/casc/tables/encoding.cpp
        src/whiteout/storages/casc/roots/root.cpp
        src/whiteout/storages/casc/roots/wow_root.cpp
        src/whiteout/storages/casc/roots/d3_root.cpp
        src/whiteout/storages/casc/roots/tvfs_root.cpp
        src/whiteout/storages/casc/roots/mndx_root.cpp
        src/whiteout/storages/casc/roots/d4_root.cpp
        src/whiteout/storages/casc/roots/wow_tvfs_root.cpp
        src/whiteout/storages/casc/roots/ow_root.cpp
        src/whiteout/storages/casc/roots/s1_root.cpp
        src/whiteout/storages/casc/roots/install_root.cpp
        src/whiteout/storages/casc/storage/progress_reporter.cpp
        src/whiteout/storages/casc/storage/storage_backend_impl.cpp
        src/whiteout/storages/casc/storage/storage_core.cpp
        src/whiteout/storages/casc/storage/storage_local.cpp
        src/whiteout/storages/casc/storage/storage_online.cpp
        src/whiteout/storages/casc/storage/storage_writable.cpp
        src/whiteout/storages/casc/storage/writer.cpp
        src/whiteout/storages/casc/cdn/cdn_cache.cpp
        src/whiteout/storages/casc/cdn/cdn_fetcher.cpp
        src/whiteout/storages/casc/cdn/online_index.cpp
        src/whiteout/storages/casc/cdn/memory_cache.cpp
        src/whiteout/utils/casc_file_system.cpp
    )

    add_library(whiteout_casc STATIC ${CASC_SOURCES})
    # See the note on whiteout_lib above.
    set_property(TARGET whiteout_casc PROPERTY POSITION_INDEPENDENT_CODE ON)

    target_include_directories(whiteout_casc PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(whiteout_casc PUBLIC whiteout_lib)
    target_link_libraries(whiteout_casc PRIVATE whiteout_strict_flags)
    whiteout_apply_clang_tidy(whiteout_casc)

    target_compile_definitions(whiteout_casc PUBLIC WHITEOUT_HAS_CASC=1)

    whiteout_define_mt_variant(whiteout_casc)

    if(WHITEOUT_BUILD_EXAMPLES)

    # CASC Storage Loader Example
    add_executable(casc_loader_example examples/casc_loader_example.cpp)
    target_link_libraries(casc_loader_example PRIVATE whiteout_casc)

    add_executable(sno_browser_example examples/sno_browser_example.cpp)
    target_link_libraries(sno_browser_example PRIVATE whiteout_casc)

    # CASC Explorer Example
    add_executable(casc_explorer_example examples/casc_explorer_example.cpp)
    target_link_libraries(casc_explorer_example PRIVATE whiteout_casc)

    # CASC Writer Example
    add_executable(casc_writer_example examples/casc_writer_example.cpp)
    target_link_libraries(casc_writer_example PRIVATE whiteout_casc)

    endif() # WHITEOUT_BUILD_EXAMPLES

endif() # WHITEOUT_ENABLE_CASC

# =============================================================================
# Optional MPQ support
# =============================================================================
option(WHITEOUT_ENABLE_MPQ "Enable MPQ archive support" OFF)

if(WHITEOUT_ENABLE_MPQ)
    add_library(whiteout_mpq
        # Shared storages/common
        src/whiteout/storages/common/mapped_file.cpp
        src/whiteout/storages/common/jenkins.cpp
        src/whiteout/storages/common/md5.cpp

        # MPQ core
        src/whiteout/storages/mpq/crypto.cpp
        src/whiteout/storages/mpq/tables/header.cpp
        src/whiteout/storages/mpq/tables/hash_table.cpp
        src/whiteout/storages/mpq/tables/block_table.cpp
        src/whiteout/storages/mpq/codecs/compression.cpp
        src/whiteout/storages/mpq/codecs/pkware.cpp
        src/whiteout/storages/mpq/codecs/sparse.cpp
        src/whiteout/storages/mpq/codecs/adpcm.cpp
        src/whiteout/storages/mpq/codecs/huffman.cpp
        src/whiteout/storages/common/codecs/bzip2.cpp
        src/whiteout/storages/common/codecs/lzma.cpp
        src/whiteout/storages/mpq/file_data.cpp
        src/whiteout/storages/mpq/special_files.cpp
        src/whiteout/storages/mpq/writer.cpp
        src/whiteout/storages/mpq/storage.cpp

        # MPQ filesystem utility
        src/whiteout/utils/mpq_file_system.cpp
    )
    # See the note on whiteout_lib above.
    set_property(TARGET whiteout_mpq PROPERTY POSITION_INDEPENDENT_CODE ON)

    target_include_directories(whiteout_mpq PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(whiteout_mpq
        PUBLIC whiteout_lib
    )
    target_link_libraries(whiteout_mpq PRIVATE whiteout_strict_flags)
    whiteout_apply_clang_tidy(whiteout_mpq)

    target_compile_definitions(whiteout_mpq PUBLIC WHITEOUT_HAS_MPQ=1)

    whiteout_define_mt_variant(whiteout_mpq)

    if(WHITEOUT_BUILD_EXAMPLES)

    # MPQ Loader Example
    add_executable(mpq_loader_example
        examples/mpq_loader_example.cpp
    )

    target_link_libraries(mpq_loader_example PRIVATE whiteout_mpq)

    # MPQ Explorer Example
    add_executable(mpq_explorer_example
        examples/mpq_explorer_example.cpp
    )

    target_link_libraries(mpq_explorer_example PRIVATE whiteout_mpq)

    endif() # WHITEOUT_BUILD_EXAMPLES
endif() # WHITEOUT_ENABLE_MPQ

# =============================================================================
# Optional client-database (DBC / DB2) support
# =============================================================================
option(WHITEOUT_ENABLE_DATABASE "Enable WoW client database (DBC/DB2) support" OFF)

if(WHITEOUT_ENABLE_DATABASE)
    add_library(whiteout_database
        src/whiteout/database/parser.cpp
        src/whiteout/database/wdbc_reader.cpp
        src/whiteout/database/wdb_reader.cpp
        src/whiteout/database/wdc_reader.cpp
        src/whiteout/database/table.cpp
        src/whiteout/database/row.cpp
        src/whiteout/database/schema.cpp
    )
    # See the note on whiteout_lib above.
    set_property(TARGET whiteout_database PROPERTY POSITION_INDEPENDENT_CODE ON)

    target_include_directories(whiteout_database PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(whiteout_database PUBLIC whiteout_lib)
    target_link_libraries(whiteout_database PRIVATE whiteout_strict_flags)
    whiteout_apply_clang_tidy(whiteout_database)

    target_compile_definitions(whiteout_database PUBLIC WHITEOUT_HAS_DATABASE=1)

    whiteout_define_mt_variant(whiteout_database)

    if(WHITEOUT_BUILD_EXAMPLES)

    # DB2 Table Dumper Example
    add_executable(db2_dump_example examples/db2_dump_example.cpp)
    target_link_libraries(db2_dump_example PRIVATE whiteout_database)

    endif() # WHITEOUT_BUILD_EXAMPLES
endif() # WHITEOUT_ENABLE_DATABASE

# =============================================================================
# Bindings subdirectories — added last so they can link against
# whiteout_lib / whiteout_casc / whiteout_mpq AND their `_mt` variants.
# =============================================================================
if(WHITEOUT_BUILD_WASM_BINDINGS)
    add_subdirectory(bindings/wasm)
endif()

if(WHITEOUT_BUILD_PYTHON_BINDINGS)
    add_subdirectory(bindings/python)
endif()

if(WHITEOUT_BUILD_C_BINDINGS)
    # The C bindings CMakeLists also absorbs the JNI bridge sources
    # into the same DLL when WHITEOUT_BUILD_JNI_BINDINGS=ON, so no
    # separate add_subdirectory(bindings/java/jni) is needed anymore.
    # It also carries the Rust value ABI (whiteout_v.*), which shares the
    # translation-unit set and needs no separate target.
    add_subdirectory(bindings/c)
endif()

# =============================================================================
# Tests
# =============================================================================
if(WHITEOUT_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# =============================================================================
# Install + export — enables `find_package(WhiteoutLib)` for downstream
# CMake consumers (vcpkg, FetchContent overlays, system installs, …).
#
# Opt out with `-DWHITEOUT_INSTALL=OFF` if you're using this repo strictly
# as a build subtree and don't want the install targets generated.
# (The WHITEOUT_INSTALL option is declared near the top of this file because
# the zlib-ng wiring needs it before the library targets are defined.)
# =============================================================================
if(WHITEOUT_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(_whiteout_install_targets whiteout_lib whiteout_strict_flags)
    if(TARGET whiteout_casc)
        list(APPEND _whiteout_install_targets whiteout_casc)
    endif()
    if(TARGET whiteout_mpq)
        list(APPEND _whiteout_install_targets whiteout_mpq)
    endif()
    if(TARGET whiteout_database)
        list(APPEND _whiteout_install_targets whiteout_database)
    endif()

    install(TARGETS ${_whiteout_install_targets}
        EXPORT WhiteoutLibTargets
        ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(DIRECTORY include/whiteout
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.hpp"
            PATTERN "*.inl"
    )

    set(_whiteout_cmake_install_dir
        ${CMAKE_INSTALL_LIBDIR}/cmake/WhiteoutLib)

    install(EXPORT WhiteoutLibTargets
        FILE       WhiteoutLibTargets.cmake
        NAMESPACE  WhiteoutLib::
        DESTINATION ${_whiteout_cmake_install_dir}
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WhiteoutLibConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/WhiteoutLibConfig.cmake
        INSTALL_DESTINATION ${_whiteout_cmake_install_dir}
    )

    # Version is keyed off the project() call at the top of this file —
    # bump it there, not here. The version file is consumed by the
    # `find_package(WhiteoutLib X.Y.Z)` version-matching machinery.
    if(NOT PROJECT_VERSION)
        set(PROJECT_VERSION "0.0.0")
    endif()
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/WhiteoutLibConfigVersion.cmake
        VERSION       ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/WhiteoutLibConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/WhiteoutLibConfigVersion.cmake
        DESTINATION ${_whiteout_cmake_install_dir}
    )

    install(FILES
        LICENSE
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/WhiteoutLib
    )
endif()
