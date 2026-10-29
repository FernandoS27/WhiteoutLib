# =============================================================================
# Tests — Catch2 v3
# =============================================================================

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# Helper: define a test executable linked to Catch2 + the whiteout modules it
# tests, and auto-register it with CTest. LINK defaults to the section's
# WHITEOUT_TEST_LINK; a test whose module this configure does not build is left
# out. A link error here is an upward edge (WHITEOUTLIB_MODULES_DESIGN.md §5):
# move what both sides need down a module rather than widen LINK.
function(whiteout_add_test TARGET SOURCE)
    cmake_parse_arguments(ARG "" "" "LINK" ${ARGN})
    if(NOT ARG_LINK)
        set(ARG_LINK ${WHITEOUT_TEST_LINK})
    endif()
    foreach(_link ${ARG_LINK})
        if(NOT TARGET ${_link})
            return()
        endif()
    endforeach()
    add_executable(${TARGET} ${SOURCE})
    target_link_libraries(${TARGET} PRIVATE ${ARG_LINK} Catch2::Catch2WithMain)
    catch_discover_tests(${TARGET} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

# A white-box test reaches build-internal headers through src/. One that
# whiteout_add_test left out has nothing to add the path to.
function(whiteout_test_src TARGET)
    if(TARGET ${TARGET})
        target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    endif()
endfunction()

# The module graph (tools/module_graph.py check): no file includes a header of a
# module its own may not depend on, and every source is in its module's list.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME module_graph
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/module_graph.py check)
endif()

# =============================================================================
# Core Tests
# =============================================================================
set(WHITEOUT_TEST_LINK whiteout_core)

whiteout_add_test(matrix_inverse_test matrix_inverse_test.cpp)
whiteout_add_test(vertex_buffer_test vertex_buffer_test.cpp)
whiteout_add_test(deflate_test deflate_test.cpp)
whiteout_test_src(deflate_test)

whiteout_add_test(unicode_path_test unicode_path_test.cpp LINK whiteout_textures whiteout_models)
whiteout_test_src(unicode_path_test)

whiteout_add_test(unicode_test unicode_test.cpp)
whiteout_test_src(unicode_test)

whiteout_add_test(blizzard_game_finder_test blizzard_game_finder_test.cpp)
whiteout_test_src(blizzard_game_finder_test)

add_executable(mapped_file_test
    mapped_file_test.cpp
    ${CMAKE_SOURCE_DIR}/src/whiteout/storages/common/mapped_file.cpp
)
target_link_libraries(mapped_file_test PRIVATE whiteout_core Catch2::Catch2WithMain)
catch_discover_tests(mapped_file_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Texture Tests
set(WHITEOUT_TEST_LINK whiteout_textures)
whiteout_add_test(bc2_test bc2_test.cpp)
whiteout_add_test(bc7_roundtrip_test bc7_roundtrip_test.cpp)
whiteout_add_test(bc6h_test bc6h_test.cpp)
whiteout_add_test(bc7_block_decode_test bc7_block_decode_test.cpp)
whiteout_add_test(blp0_test blp0_test.cpp)
whiteout_add_test(jpeg_test jpeg_test.cpp)
whiteout_add_test(tiff_test tiff_test.cpp)
whiteout_add_test(d2r_texture_test d2r_texture_test.cpp)
whiteout_add_test(txtr_synth_test txtr_synth_test.cpp)
whiteout_add_test(dds_legacy_format_test dds_legacy_format_test.cpp)
whiteout_add_test(tiff_packbits_test tiff_packbits_test.cpp)
whiteout_test_src(tiff_packbits_test)
whiteout_add_test(tiff_lzw_test tiff_lzw_test.cpp)
whiteout_test_src(tiff_lzw_test)

whiteout_add_test(apng_test apng_test.cpp)
whiteout_test_src(apng_test)

whiteout_add_test(quantize_test quantize_test.cpp)
whiteout_add_test(texture_array_test texture_array_test.cpp)
whiteout_add_test(pbr_bake_test pbr_bake_test.cpp)
whiteout_add_test(environment_map_test environment_map_test.cpp)

# Model Tests
set(WHITEOUT_TEST_LINK whiteout_models)
whiteout_add_test(m3_upgrade_corpus_test m3_upgrade_corpus_test.cpp)
whiteout_add_test(m3_physics_cook_test m3_physics_cook_test.cpp)
whiteout_add_test(m3_data_driven_material_test m3_data_driven_material_test.cpp)
whiteout_add_test(m3_engine_compat_test m3_engine_compat_test.cpp)
whiteout_add_test(m2_corpus_test m2_corpus_test.cpp)
whiteout_add_test(m2_diag_test m2_diag_test.cpp)
whiteout_add_test(m2_legacy_version_test m2_legacy_version_test.cpp)
whiteout_add_test(m2_phys_test m2_phys_test.cpp)
whiteout_add_test(m2_bone_test m2_bone_test.cpp)
whiteout_add_test(m2_load_flags_test m2_load_flags_test.cpp)
whiteout_add_test(wmo_parser_test wmo_parser_test.cpp)
whiteout_add_test(wmo_runtime_test wmo_runtime_test.cpp)
whiteout_add_test(wmo_cull_test wmo_cull_test.cpp)

# WEM Tests
set(WHITEOUT_TEST_LINK whiteout_wem)
whiteout_add_test(gltf_json_test gltf_json_test.cpp LINK whiteout_wem_convert)
whiteout_add_test(wem_profile_test wem_profile_test.cpp)
whiteout_add_test(wem_diagnostics_test wem_diagnostics_test.cpp)
whiteout_add_test(wem_topology_test wem_topology_test.cpp)
whiteout_add_test(wem_builder_test wem_builder_test.cpp)
whiteout_add_test(wem_node_test wem_node_test.cpp)
whiteout_add_test(wem_skeleton_retarget_test wem_skeleton_retarget_test.cpp)
whiteout_add_test(wem_geometry_corpus_test wem_geometry_corpus_test.cpp)
whiteout_add_test(wem_ops_test wem_ops_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_add_test(wem_shading_test wem_shading_test.cpp LINK whiteout_wem_edit)
# G18: merging meshes into one geoset, the mesh-axis referencer table, and the
# converter's say on whether toMdx could write the result.
whiteout_add_test(wem_mesh_ops_test wem_mesh_ops_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
# Optimize on conversion (EDIT_MODE_OPTIMIZE_DESIGN.md §6): merges, node
# reduction and material merges, each posed against the source it came from.
whiteout_add_test(wem_optimize_test wem_optimize_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
# The key reducer (EDIT_MODE_KEY_OPTIMIZE_DESIGN.md §8): every channel read the
# same, directly and through the written file.
whiteout_add_test(wem_key_reduce_test wem_key_reduce_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
# G29's drop arm (EDIT_MODE_MODELLING_PLAN.md MX1): a document holds LOD 0 and
# every-level meshes only, and every mesh referencer follows the drop.
whiteout_add_test(wem_lod_drop_test wem_lod_drop_test.cpp LINK whiteout_wem_convert)
# G22's canonical arms and G24's blends (EDIT_MODE_MODELLING_PLAN.md ML0).
whiteout_add_test(wem_canonical_test wem_canonical_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_add_test(wem_interpolate_test wem_interpolate_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_triangulation_test wem_triangulation_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_test_src(wem_triangulation_test)
whiteout_add_test(wem_prepare_test wem_prepare_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_add_test(wem_select_test wem_select_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_uv_islands_test wem_uv_islands_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_uv_flatten_test wem_uv_flatten_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_uv_layout_test wem_uv_layout_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_uv_seams_test wem_uv_seams_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_add_test(wem_uv_transfer_test wem_uv_transfer_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
# What is wrong with a layout (EDIT_MODE_UV_REDESIGN_PLAN.md R3).
whiteout_add_test(wem_uv_checks_test wem_uv_checks_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_modelling_tools_test wem_modelling_tools_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_add_test(wem_refine_test wem_refine_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_render_identity_test wem_render_identity_test.cpp)
whiteout_add_test(wem_material_kind_test wem_material_kind_test.cpp)
whiteout_add_test(wem_material_ops_test wem_material_ops_test.cpp)
# SameReflected / ReflectBytes: the value comparison the editor's undo and
# preview stand on, over every part of a material.
whiteout_add_test(wem_reflect_bytes_test wem_reflect_bytes_test.cpp LINK whiteout_wem_convert)
# The Materials workspace's WEM half (EDIT_MODE_MATERIALS_DESIGN.md §6.1, §5.4):
# a material edited through its native block, and a texture removed over the
# §7.4 referencer table, both read back through toMdx.
whiteout_add_test(wem_material_block_test wem_material_block_test.cpp LINK whiteout_wem_convert)
whiteout_add_test(wem_texture_ops_test wem_texture_ops_test.cpp LINK whiteout_wem_convert)
whiteout_add_test(wem_document_test wem_document_test.cpp)
whiteout_add_test(wem_anim_test wem_anim_test.cpp)
whiteout_add_test(wem_anim_mdx_test wem_anim_mdx_test.cpp LINK whiteout_wem_convert)
# A layer's fixed UV transform and constant rate on the way to an `.mdx`.
whiteout_add_test(wem_mdx_uv_test wem_mdx_uv_test.cpp LINK whiteout_wem_convert)
whiteout_add_test(wem_anim_tcb_test wem_anim_tcb_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_anim_tcb_test)
whiteout_add_test(wem_anim_m2_test wem_anim_m2_test.cpp LINK whiteout_wem_convert)
whiteout_add_test(wem_anim_m3_test wem_anim_m3_test.cpp LINK whiteout_wem_convert)
# EDIT_MODE_SC2_PROFILE_PLAN.md S1: the records a shipped .m3 holds, and the .wem before them.
whiteout_add_test(wem_sc2_carry_test wem_sc2_carry_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_test_src(wem_sc2_carry_test)
whiteout_add_test(wem_anim_d3_test wem_anim_d3_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_add_test(wem_anim_rule_test wem_anim_rule_test.cpp)
whiteout_add_test(wem_anim_animator_test wem_anim_animator_test.cpp LINK whiteout_wem_convert)
whiteout_add_test(wem_anim_crossing_test wem_anim_crossing_test.cpp LINK whiteout_wem_convert)
whiteout_add_test(wem_anim_convert_test wem_anim_convert_test.cpp LINK whiteout_wem_convert)
whiteout_add_test(wem_anim_stage_test wem_anim_stage_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_anim_physics_test wem_anim_physics_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_rigging_ik_test wem_rigging_ik_test.cpp LINK whiteout_wem_edit)
whiteout_test_src(wem_anim_stage_test)
whiteout_test_src(wem_anim_rule_test)
whiteout_test_src(wem_anim_d3_test)
whiteout_add_test(wem_profile_derive_test wem_profile_derive_test.cpp)
whiteout_add_test(wem_rescale_test wem_rescale_test.cpp)
whiteout_add_test(wem_emitter_nodes_test wem_emitter_nodes_test.cpp LINK whiteout_wem_convert)

# The material converter cores live under src/ — they are P5's converters'
# internals, not public API — so these three reach them through the src include
# path, the same way deflate_test reaches the codec.
whiteout_add_test(wem_material_collapse_test wem_material_collapse_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_material_collapse_test)
whiteout_add_test(wem_two_profile_mdx_test wem_two_profile_mdx_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_two_profile_mdx_test)
whiteout_add_test(wem_native_mdx_test wem_native_mdx_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_native_mdx_test)
whiteout_add_test(wem_native_m3_test wem_native_m3_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_native_m3_test)
whiteout_add_test(wem_native_m2_test wem_native_m2_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_native_m2_test)
whiteout_add_test(wem_native_d3_test wem_native_d3_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_native_d3_test)

# P4 step 1: the reflection visitors against the hand-written v2 pair. Reaches
# into src/ for both visitors -- the oracle is not public API and must not
# become public API on its way to being deleted at P5.
whiteout_add_test(wem_v3_roundtrip_test wem_v3_roundtrip_test.cpp)
whiteout_test_src(wem_v3_roundtrip_test)
whiteout_add_test(wem_unknown_chunk_test wem_unknown_chunk_test.cpp)
whiteout_test_src(wem_unknown_chunk_test)
whiteout_add_test(wem_convert_mdx_test wem_convert_mdx_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_test_src(wem_convert_mdx_test)
whiteout_add_test(wem_convert_m2_test wem_convert_m2_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_convert_m2_test)
whiteout_add_test(wem_convert_wmo_test wem_convert_wmo_test.cpp LINK whiteout_wem_convert)
whiteout_add_test(wem_convert_m3_test wem_convert_m3_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_convert_m3_test)
whiteout_add_test(wem_physics_m3_test wem_physics_m3_test.cpp LINK whiteout_wem_convert)
whiteout_add_test(wem_physics_test wem_physics_test.cpp LINK whiteout_wem_edit)
whiteout_test_src(wem_physics_test)
# Physics baked into clips (EDIT_MODE_PHYSICS_BAKE_DESIGN.md): the switch rule, the
# played-once names, the lead-ins and the clip's settings.
whiteout_add_test(wem_physics_bake_test wem_physics_bake_test.cpp LINK whiteout_wem_edit)
whiteout_test_src(wem_physics_bake_test)
# Workshop ▸ Soft Body (EDIT_MODE_SOFTBODY_PLAN.md): the region, the lumps,
# the lanes, the skin a file holds.
whiteout_add_test(wem_soft_test wem_soft_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_test_src(wem_soft_test)
# Its fit (plan S3): what the lumps follow exactly, and the fast solve against the reference.
whiteout_add_test(wem_soft_fit_test wem_soft_fit_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_soft_bake_test wem_soft_bake_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_add_test(wem_soft_modes_test wem_soft_modes_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_soft_shapes_test wem_soft_shapes_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_soft_drip_test wem_soft_drip_test.cpp LINK whiteout_wem_edit)
whiteout_test_src(wem_soft_modes_test)
whiteout_add_test(wem_physics_materials_test wem_physics_materials_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_physics_m2_test wem_physics_m2_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_physics_m2_test)
whiteout_add_test(wem_physics_d3_test wem_physics_d3_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_test_src(wem_physics_d3_test)
whiteout_test_src(wem_physics_m3_test)
whiteout_add_test(wem_convert_d3_test wem_convert_d3_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_convert_d3_test)
whiteout_add_test(wem_convert_gltf_test wem_convert_gltf_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_convert_gltf_test)
# FBX_OBJ_DESIGN O1/O2: the OBJ/MTL codec and its WEM crossing.
whiteout_add_test(wem_convert_obj_test wem_convert_obj_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
# FBX_OBJ_DESIGN F1: the FBX record codec, binary both ways and ASCII in.
whiteout_add_test(fbx_codec_test fbx_codec_test.cpp LINK whiteout_models)
# FBX_OBJ_DESIGN F2-F7: FbxConverter both ways.
whiteout_add_test(wem_convert_fbx_test wem_convert_fbx_test.cpp LINK whiteout_wem_convert)
# FBX_OBJ_DESIGN S1/S5/S7: the flat surface, the derive's factors, posed normals.
whiteout_add_test(wem_interchange_shared_test wem_interchange_shared_test.cpp LINK whiteout_textures whiteout_wem_convert)
whiteout_add_test(mdx_m3_effects_test mdx_m3_effects_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(mdx_m3_effects_test)
# EDIT_MODE_SKIN S1: what a Warcraft III file holds for each vertex's skin.
whiteout_add_test(wem_skin_export_test wem_skin_export_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_skin_export_test)
# EDIT_MODE_SKIN S2/S3/S4: the weight algebra, the points and the BVH.
whiteout_add_test(wem_skin_ops_test wem_skin_ops_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_skin_fit_test wem_skin_fit_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_skin_reduce_test wem_skin_reduce_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_skin_points_test wem_skin_points_test.cpp LINK whiteout_wem_edit)
whiteout_add_test(wem_bvh_test wem_bvh_test.cpp LINK whiteout_wem_edit)
# S7: Rigid per Island, and the share it agrees with the artist on.
whiteout_add_test(wem_skin_generate_test wem_skin_generate_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_add_test(wem_skin_transfer_test wem_skin_transfer_test.cpp LINK whiteout_wem_edit)
# EDIT_MODE_RETOPOLOGY_DESIGN.md R1-R6: the retopology's stages.
whiteout_add_test(wem_retopo_remesh_test wem_retopo_remesh_test.cpp LINK whiteout_wem_edit)
whiteout_test_src(wem_retopo_remesh_test)
whiteout_add_test(wem_retopo_field_test wem_retopo_field_test.cpp LINK whiteout_wem_edit)
whiteout_test_src(wem_retopo_field_test)
whiteout_add_test(wem_retopo_layout_test wem_retopo_layout_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_test_src(wem_retopo_layout_test)
whiteout_add_test(wem_retopo_extract_test wem_retopo_extract_test.cpp LINK whiteout_wem_edit)
whiteout_test_src(wem_retopo_extract_test)
whiteout_add_test(wem_retopo_test wem_retopo_test.cpp LINK whiteout_wem_edit)
whiteout_test_src(wem_retopo_test)
whiteout_add_test(wem_fracture_test wem_fracture_test.cpp LINK whiteout_wem_convert whiteout_wem_edit)
whiteout_add_test(wem_retopo_position_test wem_retopo_position_test.cpp LINK whiteout_wem_edit)
whiteout_test_src(wem_retopo_position_test)
whiteout_add_test(wem_skin_setup_test wem_skin_setup_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_skin_setup_test)
whiteout_add_test(wem_convert_corpus_test wem_convert_corpus_test.cpp LINK whiteout_wem_convert)
whiteout_test_src(wem_convert_corpus_test)
# The expected-loss goldens live beside the test sources, and the sweep has to
# find them whatever directory CTest runs it from.
if(TARGET wem_convert_corpus_test)
    target_compile_definitions(wem_convert_corpus_test
        PRIVATE WHITEOUT_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# SNO / Diablo Tests
set(WHITEOUT_TEST_LINK whiteout_sno)
# The D3 [install] arms read the game through CASC (d3_install.h); without it
# they compile out and the synthetic cases still run.
set(WHITEOUT_D3_INSTALL_LINK "")
if(WHITEOUT_ENABLE_CASC)
    set(WHITEOUT_D3_INSTALL_LINK whiteout_casc)
endif()
whiteout_add_test(d3_ani_corpus_test d3_ani_corpus_test.cpp)
whiteout_add_test(d3_payload_test d3_payload_test.cpp)
whiteout_add_test(d3_graphics_sno_test d3_graphics_sno_test.cpp)
whiteout_add_test(d3_sno_group_test d3_sno_group_test.cpp)
whiteout_add_test(d3_character_test d3_character_test.cpp LINK whiteout_models)
whiteout_add_test(d3_gamebalance_test d3_gamebalance_test.cpp LINK whiteout_models)
whiteout_add_test(d3_stringlist_test d3_stringlist_test.cpp LINK whiteout_models)
whiteout_add_test(d3_native_test d3_native_test.cpp LINK whiteout_sno whiteout_models ${WHITEOUT_D3_INSTALL_LINK})
# The public D3 native headers live in include/ and need no help. This is for
# the white-box half of the test, which includes the build-internal layout.h to
# check parsed values against the raw on-disk image.
whiteout_test_src(d3_native_test)
whiteout_add_test(d3_app_corpus_test d3_app_corpus_test.cpp)
whiteout_add_test(d3_effect_test d3_effect_test.cpp LINK whiteout_models)
# Same reason as d3_native_test: the structural half of the .efg gate reads
# the packed layout::EffectGroup image to check the parsed values against the
# bytes they came from.
whiteout_test_src(d3_effect_test)
whiteout_add_test(d4_sno_diag_test d4_sno_diag_test.cpp)
whiteout_add_test(d4_binary_types_test d4_binary_types_test.cpp)
whiteout_test_src(d4_binary_types_test)
whiteout_add_test(d4_tex_corpus_test d4_tex_corpus_test.cpp LINK whiteout_textures)
whiteout_add_test(d4_tex_synth_test d4_tex_synth_test.cpp LINK whiteout_textures)
# White-box like d3_native_test: the cube arm checks the written mip table
# against the build-internal tex_internal.h constants.
whiteout_add_test(d3_tex_test d3_tex_test.cpp LINK whiteout_textures ${WHITEOUT_D3_INSTALL_LINK})
whiteout_test_src(d3_tex_test)
whiteout_add_test(d3_light_test d3_light_test.cpp LINK whiteout_models ${WHITEOUT_D3_INSTALL_LINK})

# MDL Tests
set(WHITEOUT_TEST_LINK whiteout_models)
whiteout_add_test(mdl_parser_test mdl_parser_test.cpp)
whiteout_test_src(mdl_parser_test)

whiteout_add_test(mdl_converter_test mdl_converter_test.cpp)
whiteout_test_src(mdl_converter_test)

whiteout_add_test(mdl_writer_test mdl_writer_test.cpp)
whiteout_test_src(mdl_writer_test)

whiteout_add_test(mdx_mdl_roundtrip_test mdx_mdl_roundtrip_test.cpp)
whiteout_test_src(mdx_mdl_roundtrip_test)

# What Warcraft III's loader requires of an .mdx, asserted on the bytes.
whiteout_add_test(mdx_loadable_test mdx_loadable_test.cpp)
whiteout_test_src(mdx_loadable_test)

# =============================================================================
# CASC Tests
# =============================================================================
if(WHITEOUT_ENABLE_CASC)

    whiteout_add_test(casc_crypto_test casc_crypto_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_blte_test casc_blte_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_config_test casc_config_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_static_layout_test casc_static_layout_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_index_test casc_index_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_encoding_test casc_encoding_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_wow_root_test casc_wow_root_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_d3_root_test casc_d3_root_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_tvfs_root_test casc_tvfs_root_test.cpp LINK whiteout_casc)
    whiteout_add_test(wmo_casc_test wmo_casc_test.cpp LINK whiteout_models whiteout_casc)
    whiteout_add_test(casc_wow_tvfs_root_test casc_wow_tvfs_root_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_ow_root_test casc_ow_root_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_ow_provider_test casc_ow_provider_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_ow_resource_graph_test casc_ow_resource_graph_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_ow_type_survey_test casc_ow_type_survey_test.cpp LINK whiteout_casc)
    whiteout_add_test(txtr_corpus_test txtr_corpus_test.cpp LINK whiteout_textures whiteout_casc)
    whiteout_add_test(casc_open_recovery_test casc_open_recovery_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_progress_test casc_progress_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_corpus_test casc_corpus_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_parallel_test casc_parallel_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_round_trip_test casc_round_trip_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_reserve_file_id_test casc_reserve_file_id_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_write_health_test casc_write_health_test.cpp LINK whiteout_casc)
    whiteout_add_test(casc_writer_accuracy_test casc_writer_accuracy_test.cpp LINK whiteout_casc)
    whiteout_add_test(online_storage_test online_storage_test.cpp LINK whiteout_casc)

    add_executable(online_cross_validate_test online_cross_validate_test.cpp)
    target_link_libraries(online_cross_validate_test PRIVATE whiteout_casc Catch2::Catch2WithMain)
    target_include_directories(online_cross_validate_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    catch_discover_tests(online_cross_validate_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_executable(casc_race_test casc_race_test.cpp)
    target_link_libraries(casc_race_test PRIVATE whiteout_casc Catch2::Catch2WithMain)
    target_include_directories(casc_race_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    catch_discover_tests(casc_race_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # Performance heatmap test — standalone executable with custom main().
    # Usage: casc_perf_heatmap_test <casc_path> [output_dir]
    add_executable(casc_perf_heatmap_test casc_perf_heatmap_test.cpp)
    target_link_libraries(casc_perf_heatmap_test PRIVATE whiteout_casc)

    # Performance cross-reference test — standalone executable with custom main().
    # Compares WhiteoutLib CASC against CascLib for every measurable operation.
    # Usage: casc_perf_crossref_test <casc_path> [output_csv]
    add_executable(casc_perf_crossref_test casc_perf_crossref_test.cpp)
    target_link_libraries(casc_perf_crossref_test PRIVATE whiteout_casc)

    # Storage-open phase profiler — standalone executable with custom main().
    # Usage: casc_open_bench <casc_path> [--listfile <path>] [--reps N] ...
    add_executable(casc_open_bench casc_open_bench.cpp)
    target_link_libraries(casc_open_bench PRIVATE whiteout_casc)
    target_include_directories(casc_open_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)

    # Overwatch root-open profiler — standalone executable with custom main().
    # Usage: casc_ow_open_bench <ow_path> [--reps N] [--threads N] [--no-e2e]
    add_executable(casc_ow_open_bench casc_ow_open_bench.cpp)
    target_link_libraries(casc_ow_open_bench PRIVATE whiteout_casc)
    target_include_directories(casc_ow_open_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)

    # Diablo IV root-open profiler — standalone executable with custom main().
    # Usage: casc_d4_open_bench <d4_path> [--reps N] [--threads N] [--no-e2e]
    add_executable(casc_d4_open_bench casc_d4_open_bench.cpp)
    target_link_libraries(casc_d4_open_bench PRIVATE whiteout_casc)
    target_include_directories(casc_d4_open_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)

    # Diagnostic for the D4 readBatch cliff — standalone, public API only.
    # Usage: casc_d4_batch_probe [casc_path] [--sample N] [--chunk N]
    add_executable(casc_d4_batch_probe casc_d4_batch_probe.cpp)
    target_link_libraries(casc_d4_batch_probe PRIVATE whiteout_casc)
    target_include_directories(casc_d4_batch_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)

    # Open-then-read benchmark. Public API only, so the same source builds
    # against an unoptimised tree for A/B comparison.
    # Usage: casc_read_bench <casc_path> --listfile <path> [--files N] [--reps N]
    add_executable(casc_read_bench casc_read_bench.cpp)
    target_link_libraries(casc_read_bench PRIVATE whiteout_casc)

    # Online performance cross-reference test — standalone executable with custom main().
    # Benchmarks Storage::openOnline (CDN-backed) read performance.
    # Usage: online_perf_crossref_test <product> [options]
    add_executable(online_perf_crossref_test online_perf_crossref_test.cpp)
    target_link_libraries(online_perf_crossref_test PRIVATE whiteout_casc)

    # =========================================================================
    # Optional CascLib cross-validation target
    # Fetches CascLib for byte-identical comparison against the new
    # pure-C++ CASC implementation via the CascLib C API directly.
    # =========================================================================
    option(WHITEOUT_ENABLE_CASCLIB_CROSSVAL
           "Build CascLib-based cross-validation test (requires internet for FetchContent)" OFF)

    if(WHITEOUT_ENABLE_CASCLIB_CROSSVAL)
        include(FetchContent)
        FetchContent_Declare(
            casclib
            GIT_REPOSITORY https://github.com/ladislav-zezula/CascLib.git
            GIT_TAG        master
        )
        # CascLib's CMakeLists.txt uses an old cmake_minimum_required;
        # set the policy compat floor so CMake 4.x doesn't reject it.
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
        # CascLib builds casc_static as its static library target.
        set(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
        set(CASC_BUILD_STATIC_LIB ON  CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(casclib)

        # Cross-validate test: links new pure-C++ module and CascLib C API directly.
        add_executable(casc_cross_validate_test casc_cross_validate_test.cpp)
        target_link_libraries(casc_cross_validate_test PRIVATE whiteout_casc casc_static Catch2::Catch2WithMain)
        target_compile_definitions(casc_cross_validate_test PRIVATE WHITEOUT_HAS_CASCLIB_CROSSVAL=1)
        catch_discover_tests(casc_cross_validate_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

        # CascLib interop test: write with WhiteoutLib, read with CascLib.
        add_executable(casc_casclib_interop_test casc_casclib_interop_test.cpp)
        target_link_libraries(casc_casclib_interop_test PRIVATE whiteout_casc casc_static Catch2::Catch2WithMain)
        target_compile_definitions(casc_casclib_interop_test PRIVATE WHITEOUT_HAS_CASCLIB_CROSSVAL=1)
        catch_discover_tests(casc_casclib_interop_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

        # Overwatch online cross-validation: compare enumerated paths from
        # WhiteoutLib (Storage::openOnline) vs CascLib (CascOpenOnlineStorage).
        add_executable(casc_ow_online_crossval_test casc_ow_online_crossval_test.cpp)
        target_link_libraries(casc_ow_online_crossval_test PRIVATE whiteout_casc casc_static Catch2::Catch2WithMain)
        target_compile_definitions(casc_ow_online_crossval_test PRIVATE WHITEOUT_HAS_CASCLIB_CROSSVAL=1)
        catch_discover_tests(casc_ow_online_crossval_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

        # Performance cross-reference with CascLib comparison.
        target_link_libraries(casc_perf_crossref_test PRIVATE casc_static)
        target_compile_definitions(casc_perf_crossref_test PRIVATE WHITEOUT_HAS_CASCLIB_CROSSVAL=1)

        # Online performance cross-reference with CascLib comparison.
        target_link_libraries(online_perf_crossref_test PRIVATE casc_static)
        target_compile_definitions(online_perf_crossref_test PRIVATE WHITEOUT_HAS_CASCLIB_CROSSVAL=1)

    else()
        # Build the cross-validate test without CascLib (internal consistency only).
        add_executable(casc_cross_validate_test casc_cross_validate_test.cpp)
        target_link_libraries(casc_cross_validate_test PRIVATE whiteout_casc Catch2::Catch2WithMain)
        catch_discover_tests(casc_cross_validate_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    endif()

endif() # WHITEOUT_ENABLE_CASC

# =============================================================================
# MPQ Tests
# =============================================================================
if(WHITEOUT_ENABLE_MPQ)

    add_executable(mpq_race_test mpq_race_test.cpp)
    target_link_libraries(mpq_race_test PRIVATE whiteout_mpq Catch2::Catch2WithMain)
    target_include_directories(mpq_race_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    catch_discover_tests(mpq_race_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    whiteout_add_test(mpq_corpus_test mpq_corpus_test.cpp LINK whiteout_mpq)
    whiteout_add_test(mpq_test mpq_test.cpp LINK whiteout_mpq)

    whiteout_add_test(mpq_update_test mpq_update_test.cpp LINK whiteout_mpq)
    whiteout_test_src(mpq_update_test)

    add_executable(mpq_codec_test mpq_codec_test.cpp)
    target_link_libraries(mpq_codec_test PRIVATE whiteout_mpq Catch2::Catch2WithMain)
    target_include_directories(mpq_codec_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    catch_discover_tests(mpq_codec_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_executable(mpq_parallel_test mpq_parallel_test.cpp)
    target_link_libraries(mpq_parallel_test PRIVATE whiteout_mpq Catch2::Catch2WithMain)
    target_include_directories(mpq_parallel_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    catch_discover_tests(mpq_parallel_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

endif() # WHITEOUT_ENABLE_MPQ

# =============================================================================
# Database (DBC / DB2) Tests
# =============================================================================
if(WHITEOUT_ENABLE_DATABASE)

    whiteout_add_test(db2_test db2_test.cpp LINK whiteout_database)
    whiteout_add_test(db2_corpus_test db2_corpus_test.cpp LINK whiteout_database)

endif() # WHITEOUT_ENABLE_DATABASE
