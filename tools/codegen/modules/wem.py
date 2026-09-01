# SPDX-License-Identifier: BSD-3-Clause
"""WEM v3 module config for the codegen.

Unlike `mdx`/`m2`/`m3`, this module binds a *model*, not a file format: the
headers here are the edit-time document, and the two things a host actually
calls are `Parser` and `Writer`.

### The exported surface is decided here, once

Adding an overload to a bound method renames the existing C and Rust symbols for
**both** of them, so the method set is settled in one pass rather than grown one
call at a time. What that means concretely:

- `Parser::parse` and `Writer::write` each have two overloads (path and bytes /
  path and buffer). Both are bound, and both therefore carry suffixed C names.
  They are the whole reason to review this list at all — every other bound
  method is unique.
- The geometry kernel's editing ops (`geometry/ops.h`, `nodes/remove.h`,
  `materials/ops.h`) are **not** bound. They take handles and out-parameters and
  are the wrong shape for a value-marshalling ABI; a host that wants them wants
  the C++ library.
- `reflect()` is a template and is skipped automatically. It is what makes these
  structs describable, but it is not part of anyone's API.

### Why `auto_bind` is on

Every type here is public by construction — the design's whole point is a
document a host can walk. The exceptions below are the ones that are *not* a
document: half-edge handles, the attribute blob's internals, and the visitor
plumbing.
"""

from tools.codegen.ir import ModuleConfig

CONFIG = ModuleConfig(
    name='wem',
    cpp_namespace='whiteout::models::wem',
    js_prefix='Wem',
    embind_block='wem',
    headers=[
        'include/whiteout/vector_types.h',
        'include/whiteout/models/wem/profile.h',
        'include/whiteout/models/wem/diagnostics.h',
        'include/whiteout/models/wem/bounds.h',
        'include/whiteout/models/wem/asset_key.h',
        'include/whiteout/models/wem/native_bag.h',
        'include/whiteout/models/wem/materials/texture.h',
        'include/whiteout/models/wem/materials/common.h',
        'include/whiteout/models/wem/materials/features.h',
        'include/whiteout/models/wem/materials/looks.h',
        'include/whiteout/models/wem/materials/material.h',
        'include/whiteout/models/wem/geometry/attributes.h',
        'include/whiteout/models/wem/geometry/skin.h',
        'include/whiteout/models/wem/geometry/mesh.h',
        'include/whiteout/models/wem/nodes/node.h',
        'include/whiteout/models/wem/nodes/tree.h',
        'include/whiteout/models/wem/anim/channel.h',
        'include/whiteout/models/wem/anim/clip.h',
        'include/whiteout/models/wem/model.h',
        'include/whiteout/models/wem/document.h',
        'include/whiteout/models/wem/parser.h',
        'include/whiteout/models/wem/writer.h',
        'include/whiteout/models/wem/validate.h',
    ],
    output_path='bindings/wasm/wem_bindings.cpp',
    include_dirs=['include'],
    # bindings.cpp registers these — don't double-register here.
    skip_vector_js_names=[
        'VectorU8', 'VectorString',
        'VectorVector2f', 'VectorVector3f', 'VectorVector4f', 'VectorQuaternion',
        'VectorU16', 'VectorU32', 'VectorF32',
    ],
    skip_class_js_names=['Vector2f', 'Vector3f', 'Vector4f', 'Quaternion', 'Matrix44f'],
    auto_bind=True,
    auto_bind_skip=[
        # The half-edge kernel's own vocabulary. `Topology` is a set of parallel
        # index arrays whose invariants only hold as a whole, and its handles are
        # strong typedefs over u32 that mean nothing outside it — a host reads
        # geometry through `BuildRenderMesh`, which is what that function is for.
        'Topology', 'FaceSet', 'HalfedgeId', 'VertexId', 'FaceId', 'EdgeId',
        'BuildResult', 'RepairLog', 'RepairResult', 'VertexSplit', 'FaceRecord',
        # The attribute store's internals. `AttrLayer` is a typed byte blob and
        # `AttributeSet`'s accessors are templates over the element type; neither
        # marshals as a value.
        'AttrLayer', 'AttributeSet', 'ReservedLayer',
        # Visitor plumbing (`reflect.h`), not model surface.
        'ProbeVisitor',
    ],
)
