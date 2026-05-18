# SPDX-License-Identifier: BSD-3-Clause
"""M3 (StarCraft II / Heroes of the Storm) module config for the codegen.

Same auto_bind pattern as M2 — every top-level type in `whiteout::m3` is
bound by default; per-type overrides via `/// @bind ...` annotations on
the C++ declaration.
"""

from tools.codegen.ir import ModuleConfig

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
)
