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
    utils_bindings.cpp
    $<$<BOOL:${WHITEOUT_ENABLE_MPQ}>:${CMAKE_CURRENT_SOURCE_DIR}/mpq_bindings.cpp>
)

target_link_libraries(whiteout PRIVATE whiteout_lib)
if(WHITEOUT_ENABLE_MPQ)
    target_link_libraries(whiteout PRIVATE whiteout_mpq)
    target_compile_definitions(whiteout PRIVATE WHITEOUT_HAS_MPQ=1)
endif()
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

# When building a wheel via scikit-build-core, also generate Python type
# stubs and install everything (extension + stubs) into the wheel layout.
if(WHITEOUT_BUILD_PYTHON_WHEEL)
    set(_stubs_dir "${CMAKE_CURRENT_BINARY_DIR}/whiteout-stubs")
    file(MAKE_DIRECTORY ${_stubs_dir})

    # Codegen: produce <stub>.pyi for every module. Runs at configure time
    # so the generated files are present when install() rules fire below.
    foreach(_mod IN ITEMS mdx m2 m3 textures)
        execute_process(
            COMMAND ${Python_EXECUTABLE} -m tools.codegen.codegen ${_mod}
                    --backend pyi
                    --repo-root ${CMAKE_SOURCE_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE _codegen_out
            ERROR_VARIABLE  _codegen_err
            RESULT_VARIABLE _codegen_rc
        )
        if(NOT _codegen_rc EQUAL 0)
            message(FATAL_ERROR
                "pyi codegen for '${_mod}' failed (exit ${_codegen_rc}):\n"
                "${_codegen_err}")
        endif()
        # Codegen emits to packages/python/whiteout-stubs/<mod>.pyi by
        # default — pull it into the build dir for install().
        configure_file(
            "${CMAKE_SOURCE_DIR}/packages/python/whiteout-stubs/${_mod}.pyi"
            "${_stubs_dir}/${_mod}.pyi"
            COPYONLY
        )
    endforeach()

    # Hand-written stub package files (tracked in templates/).
    file(COPY
        "${CMAKE_SOURCE_DIR}/bindings/templates/python/whiteout-stubs/__init__.pyi"
        "${CMAKE_SOURCE_DIR}/bindings/templates/python/whiteout-stubs/py.typed"
        DESTINATION ${_stubs_dir}
    )

    # Install rules — scikit-build-core picks these up to assemble the wheel:
    #
    #   site-packages/whiteout.cp<ver>-<plat>.pyd
    #   site-packages/whiteout-stubs/{__init__.pyi, py.typed, mdx.pyi, ...}
    install(TARGETS whiteout LIBRARY DESTINATION . RUNTIME DESTINATION .)
    install(DIRECTORY ${_stubs_dir}/
            DESTINATION whiteout-stubs)
endif()
