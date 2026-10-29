# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# whiteout_wem: the WEM core: the document, reading and writing .wem, validating
# and playing it, the document primitives, the geometry kernel and moving a
# document between profiles. WHITEOUT_ENABLE_MODELS.

if(NOT WHITEOUT_ENABLE_MODELS)
    return()
endif()

set(_whiteout_wem_sources
    src/whiteout/models/wem/anim/animator.cpp
    src/whiteout/models/wem/anim/channel.cpp
    src/whiteout/models/wem/anim/clip.cpp
    src/whiteout/models/wem/anim/crossing.cpp
    src/whiteout/models/wem/anim/curve_keys.cpp
    src/whiteout/models/wem/anim/curves.cpp
    src/whiteout/models/wem/anim/detach.cpp
    src/whiteout/models/wem/anim/key_exact.cpp
    src/whiteout/models/wem/anim/m3_math.cpp
    src/whiteout/models/wem/anim/m3_uv.cpp
    src/whiteout/models/wem/anim/mdx_uv.cpp
    src/whiteout/models/wem/anim/pose.cpp
    src/whiteout/models/wem/anim/rests.cpp
    src/whiteout/models/wem/anim/stages.cpp
    src/whiteout/models/wem/anim/track_read.cpp
    src/whiteout/models/wem/anim/transition.cpp

    src/whiteout/models/wem/binary_read_visitor.cpp
    src/whiteout/models/wem/binary_write_visitor.cpp
    src/whiteout/models/wem/diagnostics.cpp
    src/whiteout/models/wem/document_io.cpp

    src/whiteout/models/wem/geometry/attributes.cpp
    src/whiteout/models/wem/geometry/builder.cpp
    src/whiteout/models/wem/geometry/checks.cpp
    src/whiteout/models/wem/geometry/element_set.cpp
    src/whiteout/models/wem/geometry/kernel.cpp
    src/whiteout/models/wem/geometry/merge.cpp
    src/whiteout/models/wem/geometry/mesh.cpp
    src/whiteout/models/wem/geometry/prepare.cpp
    src/whiteout/models/wem/geometry/rebuild.cpp
    src/whiteout/models/wem/geometry/render_view.cpp
    src/whiteout/models/wem/geometry/repair.cpp
    src/whiteout/models/wem/geometry/shading.cpp
    src/whiteout/models/wem/geometry/skin.cpp
    src/whiteout/models/wem/geometry/topology.cpp
    src/whiteout/models/wem/geometry/triangulation.cpp

    src/whiteout/models/wem/materials/common.cpp
    src/whiteout/models/wem/materials/material.cpp
    src/whiteout/models/wem/materials/ops.cpp
    src/whiteout/models/wem/materials/texture.cpp

    src/whiteout/models/wem/meshes/merge.cpp
    src/whiteout/models/wem/meshes/remove.cpp

    src/whiteout/models/wem/model.cpp

    src/whiteout/models/wem/native/mdx_bag.cpp

    src/whiteout/models/wem/native_bag.cpp

    src/whiteout/models/wem/nodes/node.cpp
    src/whiteout/models/wem/nodes/presence.cpp
    src/whiteout/models/wem/nodes/remove.cpp
    src/whiteout/models/wem/nodes/tree.cpp
    src/whiteout/models/wem/nodes/visitor.cpp

    src/whiteout/models/wem/physics/materials.cpp
    src/whiteout/models/wem/physics/physics.cpp
    src/whiteout/models/wem/physics/references.cpp
    src/whiteout/models/wem/physics/soft.cpp
    src/whiteout/models/wem/physics/switch_rule.cpp

    src/whiteout/models/wem/profile.cpp
    src/whiteout/models/wem/rescale.cpp
    src/whiteout/models/wem/retarget.cpp

    src/whiteout/models/wem/rigging/engine_names.cpp
    src/whiteout/models/wem/rigging/ik.cpp
    src/whiteout/models/wem/rigging/record.cpp

    src/whiteout/models/wem/skeleton_retarget.cpp

    src/whiteout/models/wem/skinning/deform.cpp
    src/whiteout/models/wem/skinning/points.cpp
    src/whiteout/models/wem/skinning/quantize.cpp

    src/whiteout/models/wem/text_dump.cpp
    src/whiteout/models/wem/validate.cpp
)

whiteout_add_module(whiteout_wem
    SOURCES ${_whiteout_wem_sources}
    PUBLIC_LINK whiteout_core
    PRIVATE_LINK whiteout_models
    DEFINE WHITEOUT_HAS_WEM=1)
