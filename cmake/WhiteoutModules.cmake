# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# whiteout_add_module(<name> SOURCES ... [PUBLIC_LINK ...] [PRIVATE_LINK ...]
#                     [DEFINE ...] [PRIVATE_DEFINE ...])
#
# One static library of WhiteoutLib's: the public include directory, the strict
# warning flags, position-independent code, clang-tidy and, for the Node WASM
# build, its `_mt` twin. Everything the module needs is passed here, because
# the twin copies the target as it stands at the end of this call.
#
# DEFINE is PUBLIC: every module states WHITEOUT_HAS_<MODULE>=1 for its
# consumers. No library source or header tests one (a define that changes a
# header between two translation units is an ODR break).

function(whiteout_add_module name)
    cmake_parse_arguments(M "" "" "SOURCES;PUBLIC_LINK;PRIVATE_LINK;DEFINE;PRIVATE_DEFINE" ${ARGN})
    add_library(${name} STATIC ${M_SOURCES})
    target_include_directories(${name} PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(${name} PUBLIC ${M_PUBLIC_LINK} PRIVATE ${M_PRIVATE_LINK} whiteout_strict_flags)
    if(M_DEFINE)
        target_compile_definitions(${name} PUBLIC ${M_DEFINE})
    endif()
    if(M_PRIVATE_DEFINE)
        target_compile_definitions(${name} PRIVATE ${M_PRIVATE_DEFINE})
    endif()
    # These static libraries are linked into shared consumers — the C ABI's
    # libwhiteout_native.so, the pybind11 module, the WASM build. On ELF
    # targets that requires position-independent code: without it the linker
    # rejects a TLS relocation (R_X86_64_TPOFF32) against a thread_local such
    # as casc's s_lastError. Cheap on x86-64, and MSVC ignores the property.
    set_property(TARGET ${name} PROPERTY POSITION_INDEPENDENT_CODE ON)
    whiteout_apply_clang_tidy(${name})
    whiteout_define_mt_variant(${name})
endfunction()
