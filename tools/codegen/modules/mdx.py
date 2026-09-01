# SPDX-License-Identifier: BSD-3-Clause
"""MDX (Warcraft III) module config for the codegen.

Lists the headers to scan and the per-module naming/output conventions.
Every other binding decision lives as a `@bind` annotation in the headers
themselves.
"""

from tools.codegen.ir import ModuleConfig, WemNative

CONFIG = ModuleConfig(
    name='mdx',
    cpp_namespace='whiteout::mdx',
    js_prefix='Mdx',
    embind_block='mdx',
    headers=[
        'include/whiteout/vector_types.h',
        'include/whiteout/models/mdx/types.h',
        'include/whiteout/models/mdx/structures.h',
        'include/whiteout/models/mdx/parser.h',
        'include/whiteout/models/mdx/writer.h',
    ],
    output_path='bindings/wasm/mdx_bindings.cpp',
    include_dirs=['include'],
    # bindings.cpp registers these — don't double-register here.
    skip_vector_js_names=['VectorU8', 'VectorString'],
    auto_bind=True,
    auto_bind_skip=[
        # TrackHeader is an internal binary-layout struct (chunk tag + count +
        # interpolation type), not a Model field. The Track<T> *template*
        # is excluded automatically by the codegen.
        'TrackHeader',
    ],
    # `--backend wem-native`: the WC3 half of the WEM native material blocks
    # (design §7.3). One block covers both WC3 profiles, because on disk the
    # classic/Reforged distinction is per *layer* (`Layer::is_hd`), not per
    # material — which is also what makes `DeriveProfile(Reforged -> Classic)`
    # a layer filter rather than a re-derivation.
    wem_native=WemNative(
        prefix='Mdx',
        header_path='include/whiteout/models/wem/native/mdx_native.h',
        roots=['Material'],
        versioned_roots=['Material'],
        # `Material` is a chunk; a layer and a sub-texture are inline data
        # inside it, so they take no tag of their own.
        tags={'Material': 'NMDX'},
    ),
)
