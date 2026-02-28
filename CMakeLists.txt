cmake_minimum_required(VERSION 3.15)
project(WhiteoutLib VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MDX Library
add_library(mdx_lib
    src/mdx/mdx_parser.cpp
    src/mdx/mdx_writer.cpp
)

target_include_directories(mdx_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# M2 Library
add_library(m2_lib
    src/m2/m2_parser.cpp
    src/m2/m2_writer.cpp
    src/m2/m2_binary_parse_visitor.cpp    src/m2/m2_binary_writer_visitor.cpp)

target_include_directories(m2_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Example Program
add_executable(mdx_loader_example
    examples/mdx_loader_example.cpp
)

target_link_libraries(mdx_loader_example PRIVATE mdx_lib)

# Writer Example Program
add_executable(mdx_writer_example
    examples/mdx_writer_example.cpp
)

target_link_libraries(mdx_writer_example PRIVATE mdx_lib)

# M2 Loader Example Program
add_executable(m2_loader_example
    examples/m2_loader_example.cpp
)

target_link_libraries(m2_loader_example PRIVATE m2_lib)

# M2 Writer Example Program
add_executable(m2_writer_example
    examples/m2_writer_example.cpp
)

target_link_libraries(m2_writer_example PRIVATE m2_lib)

# Enable testing
enable_testing()
