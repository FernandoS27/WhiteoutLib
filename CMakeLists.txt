cmake_minimum_required(VERSION 3.15)
project(WhiteoutLib VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Whiteout Library
add_library(whiteout_lib
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
)

target_include_directories(whiteout_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

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

# Enable testing
enable_testing()
