// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/materials/gltf_core.h"

#include <whiteout/textures/pbr_bake.h>

#include <cmath>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// Specular exponent to roughness. glTF's roughness is perceptual — the BRDF
/// squares it into alpha, as Reforged's `ggxNDF` does — so this is the bake's
/// `sqrt(sqrt(2 / (n + 2)))`, not the lobe-width alpha itself: the alpha put
/// exponent 20, most of StarCraft II, at 0.30 instead of 0.55 and exported every
/// surface lacquered. An exponent of 0 is "no specular highlight", roughness 1.
f32 RoughnessFromExponent(f32 exponent) {
    if (exponent <= 0.0f) {
        return 1.0f;
    }
    return textures::pbr::RoughnessFromExponent(exponent);
}

gltf::WrapMode WrapToGltf(WrapMode mode) {
    switch (mode) {
    case WrapMode::Clamp:
        return gltf::WrapMode::ClampToEdge;
    case WrapMode::Mirror:
        return gltf::WrapMode::MirroredRepeat;
    case WrapMode::Repeat:
        break;
    }
    return gltf::WrapMode::Repeat;
}

/// A deterministic, URI-safe suggested filename for a texture reference. The
/// app driver rewrites it when it resolves and embeds real pixels; standalone
/// library output keeps it, and the file still validates.
std::string SuggestUri(const TextureRef& ref, u32 index) {
    std::string base = !ref.path.empty() ? ref.path : Describe(ref);
    const std::size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        base = base.substr(0, dot);
    }
    for (char& c : base) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            c = '_';
        }
    }
    if (base.empty()) {
        base = "texture_" + std::to_string(index);
    }
    return base + ".png";
}

/// Folds a factor-only input (no texture) into a colour factor.
void FoldConstant(Vector4f& factor, const TextureInput& input) {
    factor.x *= input.constant.x;
    factor.y *= input.constant.y;
    factor.z *= input.constant.z;
    factor.w *= input.constant.w * input.weight;
}

} // namespace

// ============================================================================
// GltfMaterialExporter
// ============================================================================

GltfMaterialExporter::GltfMaterialExporter(const Document& document, gltf::Asset& asset)
    : document_(document), asset_(asset) {
    imageForTexture_.assign(document.textures.size(), gltf::kNone);
}

void GltfMaterialExporter::noteExtension(const char* name) {
    for (const std::string& existing : asset_.extensionsUsed) {
        if (existing == name) {
            return;
        }
    }
    asset_.extensionsUsed.push_back(name);
}

bool GltfMaterialExporter::textureExportable(const TextureInput& input) const {
    return input.hasTexture() && input.texture < document_.textures.size() &&
           document_.textures[input.texture].replaceableId == 0 &&
           input.mapping == UVMappingMode::ExplicitUV;
}

u32 GltfMaterialExporter::textureIndexFor(const TextureInput& input, Diagnostics& diagnostics,
                                          const std::string& where) {
    const u32 index = input.texture;
    if (index >= document_.textures.size()) {
        diagnostics.warn(DiagCode::TextureUnresolved,
                         where + ": texture index " + std::to_string(index) +
                             " names nothing the document holds");
        return gltf::kNone;
    }
    const TextureRef& ref = document_.textures[index];
    if (ref.replaceableId != 0) {
        // The MDX-export precedent: a replaceable slot names no file; the
        // colour that fills it is the host's.
        diagnostics.info(DiagCode::TextureUnresolved,
                         where + ": replaceable texture id " +
                             std::to_string(ref.replaceableId) + " skipped",
                         ElementRef(ElementKind::Texture, index));
        return gltf::kNone;
    }

    if (imageForTexture_[index] == gltf::kNone) {
        gltf::Image image;
        image.uri = SuggestUri(ref, index);
        // The join key the app driver resolves pixels through.
        image.name = "wem:texture:" + std::to_string(index);
        asset_.images.push_back(std::move(image));
        imageForTexture_[index] = static_cast<u32>(asset_.images.size() - 1);
    }
    const u32 image = imageForTexture_[index];

    const gltf::WrapMode wrapS = WrapToGltf(input.wrapU);
    const gltf::WrapMode wrapT = WrapToGltf(input.wrapV);
    u32 sampler = gltf::kNone;
    for (const SamplerKey& key : samplers_) {
        if (WrapToGltf(key.wrapU) == wrapS && WrapToGltf(key.wrapV) == wrapT) {
            sampler = key.sampler;
            break;
        }
    }
    if (sampler == gltf::kNone) {
        gltf::Sampler entry;
        entry.wrapS = wrapS;
        entry.wrapT = wrapT;
        asset_.samplers.push_back(entry);
        sampler = static_cast<u32>(asset_.samplers.size() - 1);
        samplers_.push_back(SamplerKey{input.wrapU, input.wrapV, sampler});
    }

    for (const TextureKeyEntry& entry : textures_) {
        if (entry.image == image && entry.sampler == sampler) {
            return entry.texture;
        }
    }
    gltf::Texture texture;
    texture.sampler = sampler;
    texture.source = image;
    asset_.textures.push_back(texture);
    const u32 result = static_cast<u32>(asset_.textures.size() - 1);
    textures_.push_back(TextureKeyEntry{image, sampler, result});
    return result;
}

gltf::TextureInfo GltfMaterialExporter::textureInfoFor(const TextureInput& input,
                                                       Diagnostics& diagnostics,
                                                       const std::string& where) {
    gltf::TextureInfo info;
    if (!input.hasTexture()) {
        return info;
    }
    if (input.mapping != UVMappingMode::ExplicitUV) {
        diagnostics.warn(DiagCode::FeatureDropped,
                         where + ": " + std::string(ToString(input.mapping)) +
                             " mapping has no glTF form; the layer's texture is dropped");
        return info;
    }
    info.index = textureIndexFor(input, diagnostics, where);
    if (info.index == gltf::kNone) {
        info = gltf::TextureInfo{};
        return info;
    }
    info.texCoord = input.uvSet;

    if (!input.uvTransform.isIdentity()) {
        const auto& m = input.uvTransform.m;
        gltf::TextureTransform transform;
        transform.offset = {m[0][2], m[1][2]};
        const f32 scaleX = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
        const f32 scaleY = std::sqrt(m[0][1] * m[0][1] + m[1][1] * m[1][1]);
        transform.scale = {scaleX, scaleY};
        // KHR's rotation matrix is [[cos, sin], [-sin, cos]] over column UVs,
        // so the angle reads off the first column; a shear shows up as the two
        // columns disagreeing about it.
        transform.rotation = std::atan2(-m[1][0], m[0][0]);
        const f32 other = std::atan2(m[0][1], m[1][1]);
        if (std::fabs(transform.rotation - other) > 1e-3f && scaleX > 0.0f && scaleY > 0.0f) {
            diagnostics.warn(DiagCode::FeatureDropped,
                             where + ": sheared UV transform approximated as offset/rotation/"
                                     "scale");
        }
        info.transform = transform;
        noteExtension("KHR_texture_transform");
    }
    return info;
}

GltfExportedMaterial GltfMaterialExporter::exportMaterial(const Material& material,
                                                          const std::string& where,
                                                          Diagnostics& diagnostics) {
    for (const CachedMaterial& cached : cache_) {
        if (cached.source == &material) {
            return cached.exported;
        }
    }

    const CommonMaterial& common = material.Common();
    gltf::Material out;
    out.name = !material.name.empty() ? material.name : where;

    GltfExportedMaterial exported;
    exported.invisible = hasFlag(common.flags, MaterialFlags::Invisible);

    // --- kind-independent state ----------------------------------------------
    switch (common.blend) {
    case BlendMode::Opaque:
        out.alphaMode = gltf::AlphaMode::Opaque;
        break;
    case BlendMode::AlphaKey:
    case BlendMode::Transparent:
        out.alphaMode = gltf::AlphaMode::Mask;
        out.alphaCutoff = common.alphaTestThreshold > 0.0f ? common.alphaTestThreshold : 0.5f;
        break;
    case BlendMode::AlphaBlend:
        out.alphaMode = gltf::AlphaMode::Blend;
        break;
    default:
        out.alphaMode = gltf::AlphaMode::Blend;
        diagnostics.warn(DiagCode::LossyBlendMode,
                         where + ": " + std::string(ToString(common.blend)) +
                             " has no glTF equivalent; exported as BLEND");
        break;
    }
    if (common.cull == CullMode::None) {
        out.doubleSided = true;
    } else if (common.cull == CullMode::Front) {
        // No glTF spelling; the mesh exporter reverses those primitives'
        // winding instead, which draws the same faces.
        exported.reverseWinding = true;
    }
    if (hasFlag(common.flags, MaterialFlags::Unlit)) {
        out.unlit = true;
        noteExtension("KHR_materials_unlit");
    }

    // --- the body ------------------------------------------------------------
    const auto dropLayer = [&](const char* what) {
        diagnostics.warn(DiagCode::LayerDropped,
                         where + ": " + what + " does not cross to metallic-roughness");
    };
    const auto referencesReplaceable = [&](const TextureInput& input) {
        return input.hasTexture() && input.texture < document_.textures.size() &&
               document_.textures[input.texture].replaceableId != 0;
    };

    const auto exportLegacyBody = [&](const LegacyDeferredBody& body) {
        out.pbr.baseColorFactor = body.diffuseFactor;
        out.pbr.metallicFactor = 0.0f;
        out.pbr.roughnessFactor = RoughnessFromExponent(body.specularExponent);
        out.emissiveFactor =
            Vector3f{body.emissiveFactor.x, body.emissiveFactor.y, body.emissiveFactor.z};
        for (const auto& [slot, input] : body.slots) {
            switch (slot) {
            case LegacySlot::Diffuse:
                if (textureExportable(input)) {
                    out.pbr.baseColorTexture = textureInfoFor(input, diagnostics, where);
                } else {
                    if (referencesReplaceable(input)) {
                        exported.gameComposited = true;
                    }
                    FoldConstant(out.pbr.baseColorFactor, input);
                }
                break;
            case LegacySlot::Normal:
                out.normalTexture = textureInfoFor(input, diagnostics, where);
                break;
            case LegacySlot::Emissive:
                out.emissiveTexture = textureInfoFor(input, diagnostics, where);
                break;
            case LegacySlot::AmbientOcclusion:
                out.occlusionTexture = textureInfoFor(input, diagnostics, where);
                break;
            case LegacySlot::Specular:
                dropLayer("the specular map (the exponent lowers to a roughness constant)");
                break;
            case LegacySlot::Gloss:
                dropLayer("the gloss map (the app driver packs it into an ORM; the library "
                          "cannot)");
                break;
            case LegacySlot::Environment:
                dropLayer("the environment layer");
                break;
            case LegacySlot::Height:
                dropLayer("the height map");
                break;
            case LegacySlot::Lightmap:
                dropLayer("the lightmap");
                break;
            case LegacySlot::Detail:
                dropLayer("the detail layer");
                break;
            case LegacySlot::Count:
                break;
            }
        }
    };

    switch (common.kind()) {
    case MaterialKind::PBRDeferred: {
        const PbrDeferredBody& body = *common.pbr();
        out.pbr.baseColorFactor = body.baseColorFactor;
        out.pbr.metallicFactor = body.metallicFactor;
        out.pbr.roughnessFactor = body.roughnessFactor;
        out.emissiveFactor = body.emissiveFactor;
        for (const auto& [slot, input] : body.slots) {
            switch (slot) {
            case PbrSlot::BaseColor:
                if (input.hasTexture()) {
                    out.pbr.baseColorTexture = textureInfoFor(input, diagnostics, where);
                } else {
                    FoldConstant(out.pbr.baseColorFactor, input);
                }
                break;
            case PbrSlot::Normal:
                out.normalTexture = textureInfoFor(input, diagnostics, where);
                break;
            case PbrSlot::Orm:
                // glTF's G=roughness/B=metalness packing IS the ORM layout,
                // and its R is the occlusion the spec's occlusionTexture
                // reads — one image, both bindings, the well-formed case.
                out.pbr.metallicRoughnessTexture = textureInfoFor(input, diagnostics, where);
                out.occlusionTexture = out.pbr.metallicRoughnessTexture;
                break;
            case PbrSlot::AmbientOcclusion:
                out.occlusionTexture = textureInfoFor(input, diagnostics, where);
                break;
            case PbrSlot::Metallic:
                dropLayer("the unpacked metallic map (the app driver packs an ORM; the "
                          "library cannot)");
                break;
            case PbrSlot::Roughness:
                dropLayer("the unpacked roughness map (the app driver packs an ORM; the "
                          "library cannot)");
                break;
            case PbrSlot::Emissive:
                out.emissiveTexture = textureInfoFor(input, diagnostics, where);
                break;
            case PbrSlot::Environment:
                dropLayer("the environment layer");
                break;
            case PbrSlot::TeamColorMask:
                dropLayer("the team-colour mask");
                break;
            case PbrSlot::Count:
                break;
            }
        }
        break;
    }
    case MaterialKind::LegacyDeferred:
        exportLegacyBody(*common.legacy());
        break;
    case MaterialKind::Composite: {
        const CompositeBody& body = *common.composite();
        if (std::optional<LegacyDeferredBody> flattened = Flatten(body);
            flattened.has_value()) {
            exportLegacyBody(*flattened);
            break;
        }
        // Not a slot map: take each channel's first layer, drop the rest of
        // the stack with a count — honest about how much shading is missing.
        out.pbr.baseColorFactor = body.diffuseFactor;
        out.pbr.metallicFactor = 0.0f;
        out.pbr.roughnessFactor = RoughnessFromExponent(body.specularExponent);
        out.emissiveFactor =
            Vector3f{body.emissiveFactor.x, body.emissiveFactor.y, body.emissiveFactor.z};
        bool haveColor = false;
        bool haveNormal = false;
        bool haveEmissive = false;
        bool haveOcclusion = false;
        bool sawReplaceable = false;
        for (const CompositeLayer& layer : body.layers) {
            bool taken = false;
            sawReplaceable = sawReplaceable || referencesReplaceable(layer.input);
            switch (layer.target) {
            case SurfaceChannel::Color:
                if (!haveColor && textureExportable(layer.input)) {
                    out.pbr.baseColorTexture = textureInfoFor(layer.input, diagnostics, where);
                    haveColor = true;
                    taken = true;
                }
                break;
            case SurfaceChannel::Normal:
                if (!haveNormal) {
                    out.normalTexture = textureInfoFor(layer.input, diagnostics, where);
                    haveNormal = true;
                    taken = true;
                }
                break;
            case SurfaceChannel::Emissive:
                if (!haveEmissive) {
                    out.emissiveTexture = textureInfoFor(layer.input, diagnostics, where);
                    haveEmissive = true;
                    taken = true;
                }
                break;
            case SurfaceChannel::AmbientOcclusion:
                if (!haveOcclusion) {
                    out.occlusionTexture = textureInfoFor(layer.input, diagnostics, where);
                    haveOcclusion = true;
                    taken = true;
                }
                break;
            default:
                break;
            }
            if (!taken) {
                dropLayer("a stacked composite layer");
            }
        }
        if (!haveColor && sawReplaceable) {
            exported.gameComposited = true;
        }
        break;
    }
    case MaterialKind::Combiners: {
        const CombinersBody& body = *common.combiners();
        out.pbr.baseColorFactor = body.diffuseFactor;
        out.pbr.metallicFactor = 0.0f;
        out.pbr.roughnessFactor = 0.9f;
        out.emissiveFactor =
            Vector3f{body.emissiveFactor.x, body.emissiveFactor.y, body.emissiveFactor.z};
        // The base-colour stage is the first whose texture actually crosses.
        // WC3 body materials put the team-colour plate at stage 0 — no file
        // behind it — with the diffuse texture layered above; a positional
        // stage-0 claim exported those bodies flat white.
        std::size_t baseStage = body.stages.size();
        for (std::size_t stage = 0; stage < body.stages.size(); ++stage) {
            if (textureExportable(body.stages[stage].input)) {
                baseStage = stage;
                break;
            }
        }
        bool sawReplaceable = false;
        for (std::size_t stage = 0; stage < body.stages.size(); ++stage) {
            const CombinerStage& entry = body.stages[stage];
            sawReplaceable = sawReplaceable || referencesReplaceable(entry.input);
            if (stage == baseStage) {
                out.pbr.baseColorTexture = textureInfoFor(entry.input, diagnostics, where);
                continue;
            }
            // A texture-less stage under the base is a plate the base draws
            // over; above it, only a plain modulate by a constant folds — the
            // rest of a combiner chain is not expressible in
            // metallic-roughness and the count says how much is missing.
            if (!entry.input.hasTexture() &&
                (stage < baseStage || entry.rgb == CombinerOp::Mod ||
                 entry.rgb == CombinerOp::Mod2x)) {
                FoldConstant(out.pbr.baseColorFactor, entry.input);
                if (entry.rgb == CombinerOp::Mod2x) {
                    out.pbr.baseColorFactor.x *= 2.0f;
                    out.pbr.baseColorFactor.y *= 2.0f;
                    out.pbr.baseColorFactor.z *= 2.0f;
                }
                continue;
            }
            dropLayer("a combiner stage");
        }
        if (baseStage == body.stages.size() && sawReplaceable) {
            exported.gameComposited = true;
        }
        break;
    }
    case MaterialKind::Count:
        break;
    }

    // A composite's emissive factor, as the M3 import states it, is
    // `hdrEmissiveMultiplier`: a gain on the emissive layer, shipped at 1 on
    // materials that have no such layer. glTF reads a factor with no texture as
    // constant emission, so those surfaces exported glowing flat white — every
    // crate on SM_ArmorySpectreCrate. With no map to scale there is nothing to emit.
    if (common.kind() == MaterialKind::Composite && !out.emissiveTexture.present()) {
        out.emissiveFactor = Vector3f{0, 0, 0};
    }

    // A factor past 1 is HDR emission; the core factor clamps at 1, so the
    // overshoot becomes KHR_materials_emissive_strength. Not on an unlit
    // material: unlit consumers ignore emission, and the validator flags the
    // extension pair as incompatible.
    f32 peak = out.emissiveFactor.x;
    peak = out.emissiveFactor.y > peak ? out.emissiveFactor.y : peak;
    peak = out.emissiveFactor.z > peak ? out.emissiveFactor.z : peak;
    if (peak > 1.0f) {
        out.emissiveFactor = out.emissiveFactor * (1.0f / peak);
        if (!out.unlit) {
            out.emissiveStrength = peak;
            noteExtension("KHR_materials_emissive_strength");
        }
    }

    asset_.materials.push_back(std::move(out));
    exported.material = static_cast<u32>(asset_.materials.size() - 1);
    cache_.push_back(CachedMaterial{&material, exported});
    return exported;
}

// ============================================================================
// GltfMaterialImporter
// ============================================================================

namespace {

WrapMode WrapFromGltf(gltf::WrapMode mode) {
    switch (mode) {
    case gltf::WrapMode::ClampToEdge:
        return WrapMode::Clamp;
    case gltf::WrapMode::MirroredRepeat:
        return WrapMode::Mirror;
    case gltf::WrapMode::Repeat:
        break;
    }
    return WrapMode::Repeat;
}

/// KHR_texture_transform composed back into the affine `uv' = M * (u, v, 1)`:
/// `T(offset) * R(rotation) * S(scale)` with R = [[cos, sin], [-sin, cos]].
Matrix3x2f UvTransformFromGltf(const gltf::TextureTransform& transform) {
    const f32 c = std::cos(transform.rotation);
    const f32 s = std::sin(transform.rotation);
    Matrix3x2f out;
    out.m[0][0] = c * transform.scale.x;
    out.m[0][1] = s * transform.scale.y;
    out.m[0][2] = transform.offset.x;
    out.m[1][0] = -s * transform.scale.x;
    out.m[1][1] = c * transform.scale.y;
    out.m[1][2] = transform.offset.y;
    return out;
}

} // namespace

GltfMaterialImporter::GltfMaterialImporter(const gltf::Asset& asset, Document& document)
    : asset_(asset), document_(document) {
    refForTexture_.assign(asset.textures.size(), kInvalidIndex);
}

u32 GltfMaterialImporter::textureRefFor(u32 textureIndex) {
    if (textureIndex >= asset_.textures.size()) {
        return kInvalidIndex;
    }
    if (refForTexture_[textureIndex] != kInvalidIndex) {
        return refForTexture_[textureIndex];
    }
    const gltf::Texture& texture = asset_.textures[textureIndex];
    if (texture.source >= asset_.images.size()) {
        return kInvalidIndex;
    }
    const gltf::Image& image = asset_.images[texture.source];
    // The path is the URI when the file has one; an embedded image gets a
    // synthesized name the app driver writes its extracted bytes under.
    std::string path = image.uri;
    if (path.empty()) {
        path = !image.name.empty() ? image.name
                                   : ("image" + std::to_string(texture.source) + ".png");
    }
    // One document texture per distinct path — two glTF textures over one
    // image (different samplers) share the file.
    for (std::size_t i = 0; i < document_.textures.size(); ++i) {
        if (document_.textures[i].path == path) {
            refForTexture_[textureIndex] = static_cast<u32>(i);
            return refForTexture_[textureIndex];
        }
    }
    TextureRef ref;
    ref.key = TexturePath{path};
    ref.path = path;
    document_.textures.push_back(std::move(ref));
    refForTexture_[textureIndex] = static_cast<u32>(document_.textures.size() - 1);
    return refForTexture_[textureIndex];
}

TextureInput GltfMaterialImporter::inputFor(const gltf::TextureInfo& info, ColorSpace space,
                                            Diagnostics& diagnostics, const std::string& where) {
    TextureInput input;
    input.texture = textureRefFor(info.index);
    if (input.texture == kInvalidIndex) {
        diagnostics.warn(DiagCode::TextureUnresolved,
                         where + ": texture reference names nothing usable");
        return input;
    }
    input.uvSet = info.texCoord;
    input.colorSpace = space;
    const gltf::Texture& texture = asset_.textures[info.index];
    if (texture.sampler < asset_.samplers.size()) {
        const gltf::Sampler& sampler = asset_.samplers[texture.sampler];
        input.wrapU = WrapFromGltf(sampler.wrapS);
        input.wrapV = WrapFromGltf(sampler.wrapT);
    }
    if (info.transform.has_value()) {
        input.uvTransform = UvTransformFromGltf(*info.transform);
        if (info.transform->texCoord != gltf::kNone) {
            input.uvSet = info.transform->texCoord;
        }
    }
    return input;
}

Material GltfMaterialImporter::importMaterial(u32 materialIndex, Diagnostics& diagnostics) {
    static const gltf::Material kDefault{};
    const gltf::Material& source =
        materialIndex < asset_.materials.size() ? asset_.materials[materialIndex] : kDefault;
    const std::string where =
        !source.name.empty() ? source.name : ("material" + std::to_string(materialIndex));

    Material material;
    material.name = where;
    CommonMaterial& common = material.InitCommon();
    switch (source.alphaMode) {
    case gltf::AlphaMode::Opaque:
        common.blend = BlendMode::Opaque;
        break;
    case gltf::AlphaMode::Mask:
        common.blend = BlendMode::AlphaKey;
        common.alphaTestThreshold = source.alphaCutoff;
        break;
    case gltf::AlphaMode::Blend:
        common.blend = BlendMode::AlphaBlend;
        break;
    }
    common.cull = source.doubleSided ? CullMode::None : CullMode::Back;
    if (source.unlit) {
        common.flags |= MaterialFlags::Unlit;
    }

    common.setKind(MaterialKind::PBRDeferred);
    PbrDeferredBody& body = *common.pbr();
    body.baseColorFactor = source.pbr.baseColorFactor;
    body.metallicFactor = source.pbr.metallicFactor;
    body.roughnessFactor = source.pbr.roughnessFactor;
    body.emissiveFactor = source.emissiveFactor * source.emissiveStrength;

    if (source.pbr.baseColorTexture.present()) {
        body.set(PbrSlot::BaseColor,
                 inputFor(source.pbr.baseColorTexture, ColorSpace::Srgb, diagnostics, where));
    }
    if (source.normalTexture.present()) {
        if (source.normalTexture.scale != 1.0f) {
            diagnostics.info(DiagCode::FeatureDropped,
                             where + ": normal-map scale has no WEM field; dropped");
        }
        body.set(PbrSlot::Normal,
                 inputFor(source.normalTexture, ColorSpace::Linear, diagnostics, where));
    }
    if (source.emissiveTexture.present()) {
        body.set(PbrSlot::Emissive,
                 inputFor(source.emissiveTexture, ColorSpace::Srgb, diagnostics, where));
    }
    const bool haveMr = source.pbr.metallicRoughnessTexture.present();
    if (haveMr) {
        // glTF's G-roughness/B-metalness IS the ORM packing; when the
        // occlusion rides the same image the R channel completes it.
        body.set(PbrSlot::Orm, inputFor(source.pbr.metallicRoughnessTexture, ColorSpace::Linear,
                                        diagnostics, where));
    }
    if (source.occlusionTexture.present()) {
        if (source.occlusionTexture.strength != 1.0f) {
            diagnostics.info(DiagCode::FeatureDropped,
                             where + ": occlusion strength has no WEM field; dropped");
        }
        const bool sameImage =
            haveMr && source.occlusionTexture.index == source.pbr.metallicRoughnessTexture.index;
        if (sameImage) {
            // Already in the Orm slot.
        } else if (!haveMr) {
            body.set(PbrSlot::AmbientOcclusion,
                     inputFor(source.occlusionTexture, ColorSpace::Linear, diagnostics, where));
        } else {
            // `Orm` and unpacked AO are mutually exclusive by `Validate`; the
            // app driver bakes the separate image's R channel into the ORM at
            // import time — the library alone has no pixels to do it with.
            diagnostics.warn(DiagCode::LayerDropped,
                             where + ": separate occlusion image cannot join the packed ORM "
                                     "without pixels; dropped");
        }
    }
    return material;
}

} // namespace wem
} // namespace models
} // namespace whiteout
