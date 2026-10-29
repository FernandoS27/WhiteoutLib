# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# whiteout_wem_convert: whatever knows a file format: the converters, the
# per-game material cores, models/cross and the export passes that ask what an
# export would write (optimize, draw order). WHITEOUT_WEM_CONVERT.

if(NOT WHITEOUT_WEM_CONVERT)
    return()
endif()

set(_whiteout_wem_convert_sources
    src/whiteout/models/cross/m2_wc3_attachments.cpp
    src/whiteout/models/cross/m2_wc3_emitters.cpp
    src/whiteout/models/cross/m2_wc3_lights.cpp
    src/whiteout/models/cross/m2_wc3_sequences.cpp
    src/whiteout/models/cross/mdx_m3_effects.cpp
    src/whiteout/models/cross/wc3_sc2_emitters.cpp

    src/whiteout/models/wem/converter_registry.cpp

    src/whiteout/models/wem/converters/d3_anim.cpp
    src/whiteout/models/wem/converters/d3_converter.cpp
    src/whiteout/models/wem/converters/d3_physics.cpp
    src/whiteout/models/wem/converters/export_sections.cpp
    src/whiteout/models/wem/converters/fbx_export.cpp
    src/whiteout/models/wem/converters/fbx_import.cpp
    src/whiteout/models/wem/converters/gltf_anim.cpp
    src/whiteout/models/wem/converters/gltf_converter.cpp
    src/whiteout/models/wem/converters/interchange_mesh.cpp
    src/whiteout/models/wem/converters/m2_anim.cpp
    src/whiteout/models/wem/converters/m2_converter.cpp
    src/whiteout/models/wem/converters/m2_physics.cpp
    src/whiteout/models/wem/converters/m3_anim.cpp
    src/whiteout/models/wem/converters/m3_behaviors.cpp
    src/whiteout/models/wem/converters/m3_converter.cpp
    src/whiteout/models/wem/converters/m3_emitters.cpp
    src/whiteout/models/wem/converters/m3_physics.cpp
    src/whiteout/models/wem/converters/m3_track_sink.cpp
    src/whiteout/models/wem/converters/mdx_anim.cpp
    src/whiteout/models/wem/converters/mdx_converter.cpp
    src/whiteout/models/wem/converters/obj_converter.cpp
    src/whiteout/models/wem/converters/wmo_converter.cpp

    src/whiteout/models/wem/game_minimum.cpp
    src/whiteout/models/wem/inline_models.cpp
    src/whiteout/models/wem/interchange.cpp

    src/whiteout/models/wem/materials/d3_core.cpp
    src/whiteout/models/wem/materials/draw_order.cpp
    src/whiteout/models/wem/materials/gltf_core.cpp
    src/whiteout/models/wem/materials/m2_core.cpp
    src/whiteout/models/wem/materials/m2_shader_table.cpp
    src/whiteout/models/wem/materials/m3_core.cpp
    src/whiteout/models/wem/materials/mdx_core.cpp
    src/whiteout/models/wem/materials/surface_flatten.cpp

    src/whiteout/models/wem/optimize.cpp
)

whiteout_add_module(whiteout_wem_convert
    SOURCES ${_whiteout_wem_convert_sources}
    PUBLIC_LINK whiteout_wem whiteout_models
    DEFINE WHITEOUT_HAS_WEM_CONVERT=1)
