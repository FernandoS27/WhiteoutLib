# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# whiteout_wem_edit: authoring: modelling, retopology, fracture, UV, rigging,
# skinning, and all physics simulation and its bakes. WHITEOUT_WEM_EDIT.

if(NOT WHITEOUT_WEM_EDIT)
    return()
endif()

set(_whiteout_wem_edit_sources
    src/whiteout/models/wem/anim/key_reduce_tolerance.cpp
    src/whiteout/models/wem/anim/stage_bake.cpp

    src/whiteout/models/wem/geometry/bvh.cpp
    src/whiteout/models/wem/geometry/cdt.cpp
    src/whiteout/models/wem/geometry/fit.cpp

    src/whiteout/models/wem/geometry/fracture/cells.cpp
    src/whiteout/models/wem/geometry/fracture/cut.cpp
    src/whiteout/models/wem/geometry/fracture/fracture.cpp
    src/whiteout/models/wem/geometry/fracture/seeds.cpp
    src/whiteout/models/wem/geometry/fracture/winding.cpp

    src/whiteout/models/wem/geometry/hull.cpp
    src/whiteout/models/wem/geometry/interpolate.cpp
    src/whiteout/models/wem/geometry/lattice.cpp
    src/whiteout/models/wem/geometry/modelling.cpp
    src/whiteout/models/wem/geometry/ops.cpp
    src/whiteout/models/wem/geometry/primitives.cpp
    src/whiteout/models/wem/geometry/refine.cpp

    src/whiteout/models/wem/geometry/retopo/cross_field.cpp
    src/whiteout/models/wem/geometry/retopo/extract.cpp
    src/whiteout/models/wem/geometry/retopo/field_extract.cpp
    src/whiteout/models/wem/geometry/retopo/layout.cpp
    src/whiteout/models/wem/geometry/retopo/patch_map.cpp
    src/whiteout/models/wem/geometry/retopo/position_field.cpp
    src/whiteout/models/wem/geometry/retopo/quantize.cpp
    src/whiteout/models/wem/geometry/retopo/retopology.cpp
    src/whiteout/models/wem/geometry/retopo/sparse.cpp
    src/whiteout/models/wem/geometry/retopo/surface.cpp
    src/whiteout/models/wem/geometry/retopo/work_mesh.cpp

    src/whiteout/models/wem/geometry/select.cpp

    src/whiteout/models/wem/geometry/uv/checks.cpp
    src/whiteout/models/wem/geometry/uv/flatten.cpp
    src/whiteout/models/wem/geometry/uv/islands.cpp
    src/whiteout/models/wem/geometry/uv/layout.cpp
    src/whiteout/models/wem/geometry/uv/seams.cpp
    src/whiteout/models/wem/geometry/uv/transfer.cpp

    src/whiteout/models/wem/physics/bake.cpp
    src/whiteout/models/wem/physics/cloth_bake.cpp
    src/whiteout/models/wem/physics/cloth_cage.cpp
    src/whiteout/models/wem/physics/crossing.cpp
    src/whiteout/models/wem/physics/soft_bake.cpp
    src/whiteout/models/wem/physics/soft_contact.cpp
    src/whiteout/models/wem/physics/soft_drip.cpp
    src/whiteout/models/wem/physics/soft_fields.cpp
    src/whiteout/models/wem/physics/soft_fit.cpp
    src/whiteout/models/wem/physics/soft_lumps.cpp
    src/whiteout/models/wem/physics/soft_modes.cpp
    src/whiteout/models/wem/physics/soft_pass.cpp
    src/whiteout/models/wem/physics/soft_region.cpp
    src/whiteout/models/wem/physics/soft_run.cpp
    src/whiteout/models/wem/physics/soft_shapes.cpp
    src/whiteout/models/wem/physics/soft_step.cpp
    src/whiteout/models/wem/physics/switches.cpp

    src/whiteout/models/wem/rigging/detect.cpp
    src/whiteout/models/wem/rigging/limbs.cpp
    src/whiteout/models/wem/rigging/names.cpp
    src/whiteout/models/wem/rigging/rebind.cpp
    src/whiteout/models/wem/rigging/shape.cpp
    src/whiteout/models/wem/rigging/tpose.cpp

    src/whiteout/models/wem/scene_ops.cpp

    src/whiteout/models/wem/skinning/fit.cpp
    src/whiteout/models/wem/skinning/generate.cpp
    src/whiteout/models/wem/skinning/generate_geodesic.cpp
    src/whiteout/models/wem/skinning/generate_heat.cpp
    src/whiteout/models/wem/skinning/mirror.cpp
    src/whiteout/models/wem/skinning/ops.cpp
    src/whiteout/models/wem/skinning/reduce.cpp
    src/whiteout/models/wem/skinning/solid.cpp
    src/whiteout/models/wem/skinning/transfer.cpp
)

whiteout_add_module(whiteout_wem_edit
    SOURCES ${_whiteout_wem_edit_sources}
    PUBLIC_LINK whiteout_wem whiteout_textures
    PRIVATE_LINK whiteout_models
    DEFINE WHITEOUT_HAS_WEM_EDIT=1)
