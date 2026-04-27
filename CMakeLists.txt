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
    target_compile_options(whiteout_strict_flags INTERFACE
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
        -Wall
        -Wextra
        -Wcast-qual
        -pedantic
        -pedantic-errors
        -Wfatal-errors
        -Wno-missing-braces
    )

    if(WHITEOUT_WARNINGS_AS_ERRORS)
        target_compile_options(whiteout_strict_flags INTERFACE -Werror)
    endif()
endif()

# Whiteout Library
add_library(whiteout_lib
    # Common
    src/whiteout/common_types.cpp
    src/whiteout/utils/vertex_buffer.cpp
    src/whiteout/utils/job_group.cpp
    src/whiteout/utils/simple_thread_pool.cpp
    src/whiteout/utils/timeline_semaphore.cpp
    src/whiteout/utils/os_file_system.cpp
    src/whiteout/utils/simple_http_handler.cpp
    src/whiteout/utils/blizzard_game_finder.cpp
    src/whiteout/utils/cdn_product_finder.cpp
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
    src/whiteout/textures/bmp/parser.cpp
    src/whiteout/textures/bmp/writer.cpp
    src/whiteout/textures/tga/parser.cpp
    src/whiteout/textures/tga/writer.cpp
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
    src/whiteout/common/deflate.cpp
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
)

target_include_directories(whiteout_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(whiteout_lib PRIVATE whiteout_strict_flags)

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

    target_include_directories(whiteout_casc PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(whiteout_casc PUBLIC whiteout_lib)
    target_link_libraries(whiteout_casc PRIVATE whiteout_strict_flags)

    target_compile_definitions(whiteout_casc PUBLIC WHITEOUT_HAS_CASC=1)

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

    target_include_directories(whiteout_mpq PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(whiteout_mpq
        PUBLIC whiteout_lib
    )
    target_link_libraries(whiteout_mpq PRIVATE whiteout_strict_flags)

    target_compile_definitions(whiteout_mpq PUBLIC WHITEOUT_HAS_MPQ=1)

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
# Tests
# =============================================================================
if(WHITEOUT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
