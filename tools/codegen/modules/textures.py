# SPDX-License-Identifier: BSD-3-Clause
"""Texture-format module config.

PixelFormat / TextureType (in `whiteout::textures`) plus the per-format
Parser/Writer pairs (in `whiteout::textures::<fmt>`). All bound types
have explicit `/// @bind` annotations because we DON'T use auto_bind here
— the texture surface is small and selective.
"""

from tools.codegen.ir import ModuleConfig

CONFIG = ModuleConfig(
    name='textures',
    cpp_namespace='whiteout::textures',
    js_prefix='',                  # explicit per-type js_name= annotations
    embind_block='textures',
    headers=[
        'include/whiteout/common_types.h',
        'include/whiteout/textures/texture.h',
        'include/whiteout/textures/blp/parser.h',
        'include/whiteout/textures/blp/writer.h',
        'include/whiteout/textures/png/parser.h',
        'include/whiteout/textures/png/writer.h',
        'include/whiteout/textures/jpeg/parser.h',
        'include/whiteout/textures/jpeg/writer.h',
        'include/whiteout/textures/dds/parser.h',
        'include/whiteout/textures/dds/writer.h',
        'include/whiteout/textures/bmp/parser.h',
        'include/whiteout/textures/bmp/writer.h',
        'include/whiteout/textures/tga/parser.h',
        'include/whiteout/textures/tga/writer.h',
        'include/whiteout/textures/gif/writer.h',
    ],
    output_path='bindings/wasm/textures_bindings.cpp',
    pybind_output_path='bindings/python/textures_bindings.cpp',
    include_dirs=['include'],
    skip_vector_js_names=[
        'VectorU8', 'VectorString',
        'VectorVector3f',  # registered by mdx_bindings
    ],
    auto_bind=False,
)
