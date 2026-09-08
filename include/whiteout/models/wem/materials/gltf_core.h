// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file gltf_core.h
 * @brief `CommonMaterial` → glTF metallic-roughness (GLTF_DESIGN §5).
 *
 * `PbrDeferredBody` ⇄ `pbrMetallicRoughness` is the designed-for pairing and
 * crosses nearly term for term. The other three kinds flatten with declared,
 * counted loss: a WoW combiner chain is not expressible in metallic-roughness,
 * and the `LayerDropped` count says exactly how much shading the `.glb` is
 * missing. Blends beyond opaque/mask/blend export as BLEND with a
 * `LossyBlendMode` diagnostic each.
 *
 * The library never sees a pixel. Textures cross as image entries whose URIs
 * are suggested names (`image.name` carries `wem:texture:<index>` so the app
 * driver can find the `TextureRef` to resolve, decode and embed); channel
 * packing that needs pixels — assembling an ORM from separate Gloss/AO images
 * — is the driver's job and drops here with a diagnostic.
 */

#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/models/gltf/gltf.h>

#include "../diagnostics.h"
#include "../document.h"
#include "material.h"

namespace whiteout {
namespace models {
namespace wem {

/// What the mesh exporter needs to know about one exported material beyond its
/// index: glTF has no front-culling, so those primitives reverse winding, and
/// an `Invisible` material's primitives are not emitted at all.
struct GltfExportedMaterial {
    u32 material = gltf::kNone;
    bool reverseWinding = false;
    bool invisible = false;
    /// No colour texture crossed and at least one colour source was a
    /// replaceable slot (team colour, team glow) — pixels the game composites
    /// at run time. A primitive drawn with this material would be a flat white
    /// sheet, so the mesh exporter skips it (GLTF_DESIGN §5).
    bool gameComposited = false;
};

/// Shared state for one document's material export: the texture/sampler/image
/// dedupe tables, and the material cache keyed by source material.
class GltfMaterialExporter {
public:
    GltfMaterialExporter(const Document& document, gltf::Asset& asset);

    /// Exports @p material (or returns its cached index). @p where names the
    /// material in diagnostics.
    GltfExportedMaterial exportMaterial(const Material& material, const std::string& where,
                                        Diagnostics& diagnostics);

private:
    u32 textureIndexFor(const TextureInput& input, Diagnostics& diagnostics,
                        const std::string& where);
    gltf::TextureInfo textureInfoFor(const TextureInput& input, Diagnostics& diagnostics,
                                     const std::string& where);
    /// Whether @p input's texture would actually cross — a real document
    /// texture, not a replaceable slot, on explicit UVs. The stage/layer picks
    /// ask this so a texture-less plate cannot claim a slot a textured layer
    /// above it should fill.
    bool textureExportable(const TextureInput& input) const;
    void noteExtension(const char* name);

    const Document& document_;
    gltf::Asset& asset_;

    struct CachedMaterial {
        const Material* source = nullptr;
        GltfExportedMaterial exported;
    };
    std::vector<CachedMaterial> cache_;

    /// One image per document texture, created on first use.
    std::vector<u32> imageForTexture_;

    struct SamplerKey {
        WrapMode wrapU = WrapMode::Repeat;
        WrapMode wrapV = WrapMode::Repeat;
        u32 sampler = 0;
    };
    std::vector<SamplerKey> samplers_;

    struct TextureKeyEntry {
        u32 image = 0;
        u32 sampler = 0;
        u32 texture = 0;
    };
    std::vector<TextureKeyEntry> textures_;
};

/// The §5 crossing in reverse: glTF materials into `PBRDeferred` bodies with
/// `NativeSync::Absent`. Owns the (glTF image → `Document::textures`) dedupe.
class GltfMaterialImporter {
public:
    GltfMaterialImporter(const gltf::Asset& asset, Document& document);

    /// Builds the WEM material for @p materialIndex (`gltf::kNone` gives the
    /// spec's default material).
    Material importMaterial(u32 materialIndex, Diagnostics& diagnostics);

private:
    /// The document texture for a glTF texture, created on first use;
    /// `kInvalidIndex` when the reference resolves to nothing.
    u32 textureRefFor(u32 textureIndex);

    TextureInput inputFor(const gltf::TextureInfo& info, ColorSpace space,
                          Diagnostics& diagnostics, const std::string& where);

    const gltf::Asset& asset_;
    Document& document_;
    std::vector<u32> refForTexture_;
};

} // namespace wem
} // namespace models
} // namespace whiteout
