# SPDX-License-Identifier: BSD-3-Clause
"""M2 (World of Warcraft) module config for the codegen.

Uses auto_bind so we don't have to mark every type explicitly. Per-type
overrides go on the C++ declaration via `/// @bind value_object` etc.;
internal helper types that should NOT be exposed are listed in
`auto_bind_skip`.
"""

from tools.codegen.ir import ModuleConfig

CONFIG = ModuleConfig(
    name='m2',
    cpp_namespace='whiteout::m2',
    js_prefix='M2',
    embind_block='m2',
    headers=[
        'include/whiteout/vector_types.h',
        'include/whiteout/models/m2/types.h',
        'include/whiteout/models/m2/structures/base.h',
        'include/whiteout/models/m2/structures/extensions.h',
        'include/whiteout/models/m2/structures/phys.h',
        'include/whiteout/models/m2/structures/bone_overrides.h',
        'include/whiteout/models/m2/structures/skin.h',
        'include/whiteout/models/m2/structures.h',
        'include/whiteout/models/m2/parser.h',
        'include/whiteout/models/m2/phys_file.h',
        'include/whiteout/models/m2/bone_file.h',
        'include/whiteout/models/m2/writer.h',
    ],
    output_path='bindings/wasm/m2_bindings.cpp',
    include_dirs=['include'],
    skip_vector_js_names=[
        'VectorU8', 'VectorString',
        # Math types — registered by mdx_bindings.cpp.
        'VectorVector2f', 'VectorVector3f', 'VectorVector4f', 'VectorQuaternion',
        'VectorU16', 'VectorU32', 'VectorF32',
    ],
    # Shared math classes / value_objects are owned by mdx_bindings.cpp.
    skip_class_js_names=['Vector2f', 'Vector3f', 'Vector4f', 'Quaternion'],
    auto_bind=True,
    auto_bind_skip=[
        # Format is a top-level enum used by the parser; not a public Model
        # field. Keep internal.
        'Format',
    ],
)
