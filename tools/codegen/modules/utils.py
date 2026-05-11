# SPDX-License-Identifier: BSD-3-Clause
"""Codegen config for the `whiteout::utils` namespace.

Currently covers `vertex_buffer.h` — `VertexBuffer` + the
`VertexBufferBuilder` builder. `mpq_file_system.h` also lives in this
namespace but is exposed through the `mpq` module instead (alongside
the rest of MPQ).
"""

from tools.codegen.ir import ModuleConfig

CONFIG = ModuleConfig(
    name='utils',
    cpp_namespace='whiteout::utils',
    js_prefix='Utils',
    embind_block='utils',
    headers=[
        'include/whiteout/utils/vertex_buffer.h',
    ],
    output_path='bindings/wasm/utils_bindings.cpp',
    include_dirs=['include'],
    # VectorU8 / VectorString are bound at module root. The math-type
    # vectors (VectorVector2f / 3f / 4f, VectorQuaternion, VectorF32,
    # VectorU32) are bound by mdx_bindings.cpp — pybind11 registers them
    # in the global type registry once, so all submodules see them.
    skip_vector_js_names=[
        'VectorU8', 'VectorString',
        'VectorVector2f', 'VectorVector3f', 'VectorVector4f',
        'VectorQuaternion', 'VectorF32', 'VectorU32',
    ],
    auto_bind=True,
    auto_bind_skip=[
        # `vertex_traits` is an internal compile-time helper struct with
        # many template specialisations (one per supported vertex element
        # type). They share a name and aren't user-facing.
        'vertex_traits',
    ],
)
