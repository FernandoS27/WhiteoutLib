# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Native Python bindings for whiteout_lib via pybind11.
#
# Activated by `-DWHITEOUT_BUILD_PYTHON_BINDINGS=ON`. Requires Python +
# pybind11 (auto-detected via the active venv or the system installation).

if(NOT WHITEOUT_BUILD_PYTHON_BINDINGS)
    return()
endif()

# Find a Python interpreter and the pybind11 it ships, in that order:
#   1. The active virtualenv (resolved via Python_EXECUTABLE).
#   2. Whatever Python_FIND_VIRTUALENV/Python_FIND_REGISTRY pick up.
find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)

# Locate pybind11's CMake helpers via the Python package itself —
# avoids needing pybind11 installed system-wide.
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c
            "import pybind11, sys; sys.stdout.write(pybind11.get_cmake_dir())"
    OUTPUT_VARIABLE pybind11_DIR
    RESULT_VARIABLE _pybind11_exit
)
if(NOT _pybind11_exit EQUAL 0)
    message(FATAL_ERROR "pybind11 not found in ${Python_EXECUTABLE}; "
                        "install it with `pip install pybind11`")
endif()

find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(whiteout MODULE
    module.cpp
    mdx_bindings.cpp
    m2_bindings.cpp
    m3_bindings.cpp
    textures_bindings.cpp
)

target_link_libraries(whiteout PRIVATE whiteout_lib)
target_include_directories(whiteout PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Match the rest of the project's C++ standard.
target_compile_features(whiteout PRIVATE cxx_std_20)

# Stage the resulting .pyd / .so into a flat directory next to the source
# so a Python user can `sys.path.insert(0, '<repo>/bindings/python')` and
# `import whiteout` without an install step.
set_target_properties(whiteout PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python-dist"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python-dist"
)
