# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# whiteout_textures: every texture format, the BCn codecs, mipmaps, the PBR bake
# and environment maps. WHITEOUT_ENABLE_TEXTURES.

if(NOT WHITEOUT_ENABLE_TEXTURES)
    return()
endif()

set(_whiteout_textures_sources
    src/whiteout/textures/bcn.cpp

    src/whiteout/textures/bcn/bc1.cpp
    src/whiteout/textures/bcn/bc2.cpp
    src/whiteout/textures/bcn/bc3.cpp
    src/whiteout/textures/bcn/bc4.cpp
    src/whiteout/textures/bcn/bc5.cpp
    src/whiteout/textures/bcn/bc6h.cpp
    src/whiteout/textures/bcn/bc7.cpp
    src/whiteout/textures/bcn/bcn_common.cpp

    src/whiteout/textures/blp/parser.cpp
    src/whiteout/textures/blp/writer.cpp

    src/whiteout/textures/bmp/parser.cpp
    src/whiteout/textures/bmp/writer.cpp

    src/whiteout/textures/d2r_texture/parser.cpp
    src/whiteout/textures/d2r_texture/writer.cpp

    src/whiteout/textures/dds/parser.cpp
    src/whiteout/textures/dds/writer.cpp

    src/whiteout/textures/environment_map.cpp

    src/whiteout/textures/gif/writer.cpp

    src/whiteout/textures/jpeg/jpeg_decode.cpp
    src/whiteout/textures/jpeg/jpeg_encode.cpp
    src/whiteout/textures/jpeg/parser.cpp
    src/whiteout/textures/jpeg/writer.cpp

    src/whiteout/textures/mipmap/filters.cpp
    src/whiteout/textures/mipmap/generator.cpp
    src/whiteout/textures/mipmap/stages.cpp

    src/whiteout/textures/pbr_bake.cpp

    src/whiteout/textures/png/parser.cpp
    src/whiteout/textures/png/writer.cpp

    src/whiteout/textures/tex/parser.cpp
    src/whiteout/textures/tex/tex_common.cpp
    src/whiteout/textures/tex/writer.cpp

    src/whiteout/textures/texture.cpp

    src/whiteout/textures/tga/parser.cpp
    src/whiteout/textures/tga/writer.cpp

    src/whiteout/textures/tiff/lzw.cpp
    src/whiteout/textures/tiff/packbits.cpp
    src/whiteout/textures/tiff/parser.cpp
    src/whiteout/textures/tiff/writer.cpp

    src/whiteout/textures/txtr/parser.cpp

    src/whiteout/textures/utils/blue_noise.cpp
    src/whiteout/textures/utils/color_convert.cpp
    src/whiteout/textures/utils/pixel_convert.cpp
    src/whiteout/textures/utils/quantize.cpp
    src/whiteout/textures/utils/srgb_linearize.cpp
)

# Diablo IV .tex reads its metadata from an SNO; without the reader, a D4 .tex
# parses as a failure does (`d4_parser_none.cpp`) and no SNO table is built.
set(_whiteout_textures_private_link "")
if(WHITEOUT_TEXTURES_D4)
    list(APPEND _whiteout_textures_sources src/whiteout/textures/tex/d4_parser.cpp)
    list(APPEND _whiteout_textures_private_link whiteout_sno)
else()
    list(APPEND _whiteout_textures_sources src/whiteout/textures/tex/d4_parser_none.cpp)
endif()

whiteout_add_module(whiteout_textures
    SOURCES ${_whiteout_textures_sources}
    PUBLIC_LINK whiteout_core
    PRIVATE_LINK ${_whiteout_textures_private_link}
    DEFINE WHITEOUT_HAS_TEXTURES=1)
