# SPDX-License-Identifier: BSD-3-Clause
"""Diablo III SNO module config for the codegen.

This module exists for one backend — `wem-native` — and that is why its `@wem`
directives are a **sidecar table** here rather than annotations in the headers:
`include/whiteout/sno/d3/native/*.h` are themselves machine-written by
`gen_d3_native.py` in the sibling repo, so an annotation added there is deleted
by the next regeneration (design §15.2).

The table is also the right home for the renames, for a reason beyond mechanics.
Three of D3's field names are simply wrong about what the field holds:

    dwSlotIndex   @0x00  is the texture *type*, and it is the key of the entry
    dwTextureFlags @0x0C is the UV transform *mode*, 0..6 (82.7% are mode 2)
    dwTextureType @0x98  is a UV *flags* word; bits 0 and 1 are the address modes

Renaming them is WEM's opinion about D3's data, arrived at by reading the
shipped `.app` corpus and the 2.8.x binary. It belongs in WEM's config, not
upstream in someone else's generator, and the WEM converter does the renaming —
which is the cheapest place to contain a known-bad name.
"""

from tools.codegen.ir import ModuleConfig, WemNative

CONFIG = ModuleConfig(
    name='d3',
    cpp_namespace='whiteout::sno::d3::native',
    js_prefix='D3',
    embind_block='d3',
    headers=[
        'include/whiteout/vector_types.h',
        'include/whiteout/sno/d3/native/types.h',
    ],
    # No bindings module ships for D3 today; `wem-native` is the only consumer.
    # The path is here because ModuleConfig requires one, not because anything
    # writes it.
    output_path='bindings/wasm/d3_bindings.cpp',
    include_dirs=['include'],
    skip_class_js_names=['Vector2f', 'Vector3f', 'Vector4f', 'Quaternion'],
    auto_bind=True,
    wem_native=WemNative(
        prefix='D3',
        header_path='include/whiteout/models/wem/native/d3_native.h',
        roots=[
            'UberMaterial',   # the per-look embedded material
            'Material',       # the standalone Material asset (group 57)
            'RenderPass',     # the render state, which in D3 lives on `Shaders`
        ],
        # `D3Material` — the wrapper that pairs the material with the two whole
        # `Shaders` assets a sub-object swaps on `alpha < 1.0` — is authored, in
        # `native/d3_material.h`. There is no single D3 struct to mirror for it,
        # because the render state is on a *different asset*.
        tags={},
        overrides={
            'AssetRef': {
                '_type': {'rename': 'D3AssetRef'},
            },
            'MaterialColors': {
                '_type': {'rename': 'D3MaterialColors'},
                'vDiffuse': {'rename': 'diffuse'},
                'vSpecular': {'rename': 'specular'},
                'vEmissive': {'rename': 'emissive'},
                'vAmbient': {'rename': 'ambient'},
                'flShininess': {'rename': 'shininess'},
                'dwMaterialFlags': {'rename': 'materialFlags'},
            },
            'MaterialTextureAnim': {
                '_type': {'rename': 'D3TexAnim'},
                'flAmount': {'rename': 'amount'},
                'flRate0': {'rename': 'rate0'},
                'flRate1': {'rename': 'rate1'},
            },
            'MaterialTextureEntry': {
                '_type': {'rename': 'D3TextureEntry'},
                # The three lying names. See the module docstring.
                'dwSlotIndex': {'rename': 'type'},
                'dwTextureFlags': {'rename': 'uvTransformMode'},
                'dwTextureType': {'rename': 'uvFlags'},
                'snoTexture': {'rename': 'texture'},
                'vUvRow0': {'rename': 'uvRow0'},
                'vUvRow1': {'rename': 'uvRow1'},
                'vUvRow2': {'rename': 'uvRow2'},
                'vUvRow3': {'rename': 'uvRow3'},
                'tAnimU': {'rename': 'animU'},
                'tAnimV': {'rename': 'animV'},
                'tAnimRotate': {'rename': 'animRotate'},
                'tAnim3': {'rename': 'anim3'},
                'tAnim4': {'rename': 'anim4'},
                'tAnim5': {'rename': 'anim5'},
            },
            'UberMaterial': {
                '_type': {'rename': 'D3UberMaterial'},
                'snoShaderMap': {'rename': 'shaderMap'},
                'tColors': {'rename': 'colors'},
                'arTextures': {'rename': 'textures'},
            },
            'Material': {
                '_type': {'rename': 'D3MaterialAsset'},
                'dwSnoId': {'rename': 'snoId'},
                'arShaderParams': {'rename': 'shaderParams'},
                'tMaterial': {'rename': 'uber'},
            },
            'TagMapEntry': {
                '_type': {'rename': 'D3TagValue'},
                'nValueType': {'rename': 'valueType'},
                'dwTagId': {'rename': 'tagId'},
                'dwValue': {'rename': 'value'},
            },
            'ShaderTagMapEntry': {
                '_type': {'rename': 'D3ShaderTagValue'},
                'nValueType': {'rename': 'valueType'},
                'dwTagId': {'rename': 'tagId'},
                'dwValue': {'rename': 'value'},
            },
            'RenderParams': {
                '_type': {'rename': 'D3RenderParams'},
            },
            'TextureStageParams': {
                '_type': {'rename': 'D3TextureStage'},
                # The address modes are on the PASS, not the material — which is
                # why `uvFlags` above and these coexist rather than duplicating.
                'dwTextureType': {'rename': 'contentStage'},
            },
            'RenderPass': {
                '_type': {'rename': 'D3RenderState'},
                'tRenderParams': {'rename': 'renderParams'},
                'dwTextureStageCount': {'rename': 'textureStageCount'},
                'arTextureStages': {'rename': 'textureStages'},
                'dwPassFlags': {'rename': 'passFlags'},
                'szEffectFile': {'rename': 'effectFile'},
                'szVertexShaderEntry': {'rename': 'vertexShaderEntry'},
                'szPixelShaderEntry': {'rename': 'pixelShaderEntry'},
                'arShaderParams': {'rename': 'shaderParams'},
            },
        },
    ),
)
