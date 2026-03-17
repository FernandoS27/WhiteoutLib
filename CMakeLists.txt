cmake_minimum_required(VERSION 3.15)
project(WhiteoutLib VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(WHITEOUT_ENABLE_CASC "Enable CASC archive support via CascLib" OFF)
option(WHITEOUT_BUILD_EXAMPLES "Build example programs" OFF)

if(MSVC)
    add_compile_options(/FS)
endif()

# Whiteout Library
add_library(whiteout_lib
    # Common
    src/whiteout/common_types.cpp
    # src/whiteout/utils/vertex_buffer.cpp
    src/whiteout/utils/job_group.cpp
    src/whiteout/utils/simple_thread_pool.cpp
    src/whiteout/utils/timeline_semaphore.cpp
    src/whiteout/utils/os_file_system.cpp
    src/whiteout/vector_types.cpp
    src/whiteout/common/bit_io.cpp
    src/whiteout/common/huffman_table.cpp

    # MDX
    src/whiteout/mdx/parser.cpp
    src/whiteout/mdx/writer.cpp

    # M2
    src/whiteout/m2/parser.cpp
    src/whiteout/m2/writer.cpp
    src/whiteout/m2/binary_parse_visitor/base.cpp
    src/whiteout/m2/binary_parse_visitor/chunk.cpp
    src/whiteout/m2/binary_parse_visitor/skin.cpp
    src/whiteout/m2/binary_parse_visitor/bone.cpp
    src/whiteout/m2/binary_parse_visitor/anim.cpp
    src/whiteout/m2/binary_writer_visitor/base.cpp
    src/whiteout/m2/binary_writer_visitor/chunk.cpp
    src/whiteout/m2/binary_writer_visitor/skin.cpp
    src/whiteout/m2/binary_writer_visitor/bone.cpp
    src/whiteout/m2/binary_writer_visitor/anim.cpp
    src/whiteout/m2/file_system.cpp

    # M3
    src/whiteout/m3/types.cpp
    src/whiteout/m3/parser.cpp
    src/whiteout/m3/writer.cpp
    src/whiteout/m3/binary_parse_visitor.cpp
    src/whiteout/m3/binary_writer_visitor.cpp

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
    src/whiteout/textures/png/deflate.cpp
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

endif() # WHITEOUT_BUILD_EXAMPLES

# =============================================================================
# Optional CASC support (via CascLib)
# =============================================================================
option(WHITEOUT_ENABLE_CASC "Enable CASC archive support via CascLib" ON)

if(WHITEOUT_ENABLE_CASC)
    include(FetchContent)
    FetchContent_Declare(CascLib
        GIT_REPOSITORY https://github.com/ladislav-zezula/CascLib.git
        GIT_TAG        master
    )
    set(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(CASC_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
    # CascLib's CMakeLists.txt uses an old cmake_minimum_required version.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(CascLib)

    add_library(whiteout_casc
        src/whiteout/casc/storage.cpp
        src/whiteout/casc/casc_file_system.cpp
    )

    target_include_directories(whiteout_casc PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(whiteout_casc
        PUBLIC whiteout_lib
        PRIVATE casc_static
    )

    target_compile_definitions(whiteout_casc PUBLIC WHITEOUT_HAS_CASC=1)

    if(WHITEOUT_BUILD_EXAMPLES)

    # Interactive SNO browser example
    add_executable(sno_browser_example
        examples/sno_browser_example.cpp
    )

    target_link_libraries(sno_browser_example PRIVATE whiteout_casc)

    endif() # WHITEOUT_BUILD_EXAMPLES
endif() # WHITEOUT_ENABLE_CASC
