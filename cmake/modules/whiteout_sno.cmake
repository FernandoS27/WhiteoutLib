# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# whiteout_sno: the SNO container and the Diablo III/IV definition tables (the
# Diablo III model reader, sno/d3/native, is whiteout_models'). Built when CASC
# or the Diablo IV .tex reader (WHITEOUT_TEXTURES_D4) needs it.

if(NOT (WHITEOUT_ENABLE_CASC OR (WHITEOUT_ENABLE_TEXTURES AND WHITEOUT_TEXTURES_D4)))
    return()
endif()

set(_whiteout_sno_sources
    src/whiteout/sno/core_toc.cpp

    src/whiteout/sno/d3/sno_defs.cpp

    src/whiteout/sno/d4/binary_types.cpp
    src/whiteout/sno/d4/sno_defs.cpp

    src/whiteout/sno/sno_reader.cpp
    src/whiteout/sno/sno_types.cpp
    src/whiteout/sno/sno_value.cpp
)

whiteout_add_module(whiteout_sno
    SOURCES ${_whiteout_sno_sources}
    PUBLIC_LINK whiteout_core
    DEFINE WHITEOUT_HAS_SNO=1)
