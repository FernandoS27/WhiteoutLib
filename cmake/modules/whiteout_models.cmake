# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# whiteout_models: every raw model format (MDX/MDL, M2, WMO, M3, glTF, FBX, OBJ)
# and the Diablo III model reader. WHITEOUT_ENABLE_MODELS.

if(NOT WHITEOUT_ENABLE_MODELS)
    return()
endif()

set(_whiteout_models_sources
    src/whiteout/models/fbx/ascii.cpp
    src/whiteout/models/fbx/binary.cpp
    src/whiteout/models/fbx/eval.cpp
    src/whiteout/models/fbx/fbx.cpp
    src/whiteout/models/fbx/scene.cpp

    src/whiteout/models/gltf/gltf.cpp
    src/whiteout/models/gltf/json.cpp
    src/whiteout/models/gltf/parser.cpp
    src/whiteout/models/gltf/writer.cpp

    src/whiteout/models/m2/animation_names.cpp

    src/whiteout/models/m2/binary_parse_visitor/anim.cpp
    src/whiteout/models/m2/binary_parse_visitor/base.cpp
    src/whiteout/models/m2/binary_parse_visitor/chunk.cpp
    src/whiteout/models/m2/binary_parse_visitor/extensions.cpp
    src/whiteout/models/m2/binary_parse_visitor/phys.cpp
    src/whiteout/models/m2/binary_parse_visitor/skin.cpp

    src/whiteout/models/m2/binary_writer_visitor/anim.cpp
    src/whiteout/models/m2/binary_writer_visitor/base.cpp
    src/whiteout/models/m2/binary_writer_visitor/chunk.cpp
    src/whiteout/models/m2/binary_writer_visitor/extensions.cpp
    src/whiteout/models/m2/binary_writer_visitor/phys.cpp
    src/whiteout/models/m2/binary_writer_visitor/skin.cpp

    src/whiteout/models/m2/bone_file.cpp
    src/whiteout/models/m2/chunk_parser.cpp
    src/whiteout/models/m2/file_system.cpp
    src/whiteout/models/m2/parser.cpp
    src/whiteout/models/m2/phys_file.cpp
    src/whiteout/models/m2/sequence_loader.cpp
    src/whiteout/models/m2/wow_file_system.cpp
    src/whiteout/models/m2/writer.cpp

    src/whiteout/models/m3/binary_parse_visitor.cpp
    src/whiteout/models/m3/binary_writer_visitor.cpp
    src/whiteout/models/m3/data_driven_material.cpp
    src/whiteout/models/m3/data_driven_to_standard.cpp
    src/whiteout/models/m3/engine_compat.cpp
    src/whiteout/models/m3/parser.cpp
    src/whiteout/models/m3/physics_cook.cpp
    src/whiteout/models/m3/physics_upgrade.cpp
    src/whiteout/models/m3/types.cpp
    src/whiteout/models/m3/writer.cpp

    src/whiteout/models/mdx/loadable.cpp
    src/whiteout/models/mdx/mdl_converter.cpp
    src/whiteout/models/mdx/mdl_parser.cpp
    src/whiteout/models/mdx/mdl_tokenizer.cpp
    src/whiteout/models/mdx/mdl_writer.cpp
    src/whiteout/models/mdx/parser.cpp
    src/whiteout/models/mdx/prune.cpp
    src/whiteout/models/mdx/writer.cpp

    src/whiteout/models/obj/parser.cpp
    src/whiteout/models/obj/writer.cpp

    src/whiteout/models/wow/wmo/cull.cpp
    src/whiteout/models/wow/wmo/parser.cpp
    src/whiteout/models/wow/wmo/runtime.cpp

    src/whiteout/sno/d3/native/binary_parse_visitor/actor.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/anim.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/appearance.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/base.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/common.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/material.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/particle.cpp
    src/whiteout/sno/d3/native/binary_parse_visitor/physics.cpp

    src/whiteout/sno/d3/native/character.cpp
    src/whiteout/sno/d3/native/d3_native.cpp
)

whiteout_add_module(whiteout_models
    SOURCES ${_whiteout_models_sources}
    PUBLIC_LINK whiteout_core
    DEFINE WHITEOUT_HAS_MODELS=1)
