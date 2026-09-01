# SPDX-License-Identifier: BSD-3-Clause
"""M3 (StarCraft II / Heroes of the Storm) module config for the codegen.

Same auto_bind pattern as M2 — every top-level type in `whiteout::m3` is
bound by default; per-type overrides via `/// @bind ...` annotations on
the C++ declaration.
"""

from tools.codegen.ir import ModuleConfig, WemNative

CONFIG = ModuleConfig(
    name='m3',
    cpp_namespace='whiteout::m3',
    js_prefix='M3',
    embind_block='m3',
    headers=[
        'include/whiteout/vector_types.h',
        'include/whiteout/models/m3/types.h',
        'include/whiteout/models/m3/structures/anim.h',
        'include/whiteout/models/m3/structures/base.h',
        'include/whiteout/models/m3/structures/effect.h',
        'include/whiteout/models/m3/structures/material.h',
        'include/whiteout/models/m3/structures/mesh.h',
        'include/whiteout/models/m3/structures/misc.h',
        'include/whiteout/models/m3/structures/physics.h',
        'include/whiteout/models/m3/structures/scene.h',
        'include/whiteout/models/m3/structures.h',
        'include/whiteout/models/m3/parser.h',
        'include/whiteout/models/m3/writer.h',
    ],
    output_path='bindings/wasm/m3_bindings.cpp',
    include_dirs=['include'],
    skip_vector_js_names=[
        'VectorU8', 'VectorString',
        'VectorVector2f', 'VectorVector3f', 'VectorVector4f', 'VectorQuaternion',
        'VectorU16', 'VectorU32', 'VectorF32',
    ],
    skip_class_js_names=['Vector2f', 'Vector3f', 'Vector4f', 'Quaternion'],
    auto_bind=True,
    auto_bind_skip=[
        # AnimBlock/Reference/Flag are runtime parsing primitives, not
        # Model surface area. (AnimRef is now bound via @bind value_template
        # and instantiated per-T from the structs that use it.)
        'Reference', 'AnimBlock', 'Flag',
        # VertexBuffer is a binary blob with format flags; M3 vertex data is
        # better consumed via raw bytes than a per-attribute wrapper.
        'VertexBuffer',
    ],
    # `--backend wem-native`: the SC2/Heroes half of the WEM native material
    # blocks (design §7.3). The roots are the material bodies themselves; the
    # `M3Material` wrapper that unions them, carries `sourceVersion` and holds
    # the two MADD derivations is authored, because a variant over eleven
    # alternatives is a decision and not a mirror of anything.
    wem_native=WemNative(
        prefix='M3',
        header_path='include/whiteout/models/wem/native/m3_native.h',
        roots=[
            'StandardMaterial', 'DisplacementMaterial', 'CompositeMaterial',
            'TerrainMaterial', 'VolumeMaterial', 'VolumeNoiseMaterial',
            'CreepMaterial', 'STBMaterial', 'ReflectionMaterial', 'LensFlare',
            'DataDrivenMaterial',
            # The decoded MADD blob. Not a chunk — MADD stores it as bytes —
            # but WEM keeps the decode so a consumer never has to re-run it,
            # and §7.3 says the block keeps the authored groups.
            'DataDrivenProperties',
            'TextureLayer',
        ],
        # HAI_ is defunct: the parser header says "always null" and the corpus
        # agrees, so the variant omits it and a non-null hairMaterials array
        # diagnoses rather than parsing (§7.3). StandardMaterialConversion is a
        # conversion *report*, not shipped data.
        exclude=['HairMaterial', 'StandardMaterialConversion'],
        # The kind discriminator. Nothing the generator can reach names it —
        # `M3Material`, which does, is authored.
        extra_enums=['MaterialType'],
        # Every body is inline in the authored `M3Material`'s NM3_ chunk.
        tags={},
    ),
)
