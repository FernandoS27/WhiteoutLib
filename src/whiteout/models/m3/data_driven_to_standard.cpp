// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Reverses the engine's StandardMaterial -> DataDrivenMaterial conversion. The
// forward direction lives in the Heroes client (sub_1027A2380) and is lossy, so
// everything here that cannot be recovered is reported rather than invented.

#include <whiteout/models/m3/structures.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace whiteout {
namespace m3 {
namespace {

using PropertyMap = std::map<std::string, const DataDrivenProperty*>;

struct LayerSpec {
    std::optional<TextureLayer> StandardMaterial::*slot;
    const char* prefix;         // property prefix, which is not always the group name
    std::vector<const char*> families;
};

// Fragment families that select a StandardMaterial layer slot. The prefix column
// is the irregular part: AlphaMask stores its properties under `Alpha`, the
// environment layer splits between `EnvironmentMap` and `Envio`, and HeightMap
// spells its own scalar `HeightMapScale` while its layer properties use
// `Heightmap`.
const std::vector<LayerSpec>& layerTable() {
    static const std::vector<LayerSpec> table = {
        {&StandardMaterial::diffuseLayer, "Diffuse",
         {"Diffuse", "DiffuseFast", "DiffuseTriPlanar", "DiffuseTeamColor", "DiffuseConstantTeam"}},
        {&StandardMaterial::decalLayer, "Decal", {"Decal"}},
        {&StandardMaterial::specularLayer, "Specular",
         {"Specular", "SpecularTriPlanar", "SpecularConstant"}},
        {&StandardMaterial::glossLayer, "Gloss", {"Gloss", "GlossConstant"}},
        {&StandardMaterial::emissiveLayer1, "Emissive",
         {"Emissive", "EmissiveFast", "EmissiveMasked", "EmissiveTriPlanar", "EmissiveTeam",
          "EmissiveConstant", "EmissiveConstantTeam"}},
        {&StandardMaterial::emissiveLayer2, "Emissive2",
         {"Emissive2", "Emissive2Fast", "Emissive2Team", "Emissive2Constant",
          "Emissive2ConstantTeam"}},
        {&StandardMaterial::environmentLayer, "EnvironmentMap",
         {"Envio", "EnvioBlurred", "EnvioNonReflective", "EnvioNonReflectiveBlurred", "SphereEnvio",
          "SphereEnvioBlurred", "SphereEnvioNonReflective", "SphereEnvioNonReflectiveBlurred",
          "EnvioConstant"}},
        {&StandardMaterial::environmentMaskLayer, "EnvioMask",
         {"EnvioMask", "SphereEnvioMask", "EnvioMaskConstant"}},
        {&StandardMaterial::alphaLayer1, "Alpha",
         {"AlphaMask", "AlphaMaskTriPlanar", "AlphaMaskConstant"}},
        {&StandardMaterial::alphaLayer2, "Alpha2", {"AlphaMask2", "AlphaMask2Constant"}},
        {&StandardMaterial::normalLayer, "Normal",
         {"Normal", "NormalWorld", "NormalTriPlanar", "NormalConstant"}},
        {&StandardMaterial::heightLayer, "Heightmap", {"HeightMap"}},
        {&StandardMaterial::lightMapLayer, "Lightmap", {"LightMap"}},
        {&StandardMaterial::ambientOcclusionLayer, "AOMask", {"AOMask"}},
    };
    return table;
}

// Fragments the forward converter emits from the model context (which emitter
// uses the material, the render pass) rather than from material state. They
// cannot be pushed back into a StandardMaterial.
bool isPipelineFragment(const std::string& n) {
    static const std::vector<std::string> pipeline = {
        "MaterialFinalize", "FogAmount",     "ReconstructBinormal", "SplatPS",
        "SplatMaterialAttenuation", "ECSpec", "FresnelSetup",       "FresnelSetupSimplified",
        "PremultiplyAlpha"};
    return std::find(pipeline.begin(), pipeline.end(), n) != pipeline.end();
}

// Fragments belonging to a different material type entirely.
const char* foreignMaterial(const std::string& n) {
    if (n.rfind("Displacement", 0) == 0)
        return "DisplacementMaterial";
    if (n.rfind("Reflection", 0) == 0)
        return "ReflectionMaterial";
    if (n == "Cloaking")
        return "cloaking material";
    return nullptr;
}

// Nodes of the shader-graph vocabulary, which the node editor emits instead of
// the fixed-function fragments. A material built from these was authored as a
// graph and never had a StandardMaterial form. Deliberately keyed on the
// vocabulary rather than on "the name did not resolve": the Rail* names are in
// the recovered table now, and resolving a name must not change what converts.
bool isShaderGraphFragment(const std::string& n) {
    return n.rfind("Rail", 0) == 0 || n.rfind("Surface", 0) == 0 || n == "GradientColorize";
}

enum class MaterialKind { FixedFunction, ShaderGraph, Foreign, UnknownFragment };

struct Classification {
    MaterialKind kind = MaterialKind::FixedFunction;
    std::string blocker;
};

Classification classify(const DataDrivenProperties& decoded) {
    Classification out;
    const DataDrivenGroup* unnamed = nullptr;
    const DataDrivenGroup* foreign = nullptr;
    const char* foreignKind = nullptr;

    for (const auto& group : decoded.groups) {
        if (isShaderGraphFragment(group.name)) {
            out.kind = MaterialKind::ShaderGraph;
            out.blocker = "shader-graph material: fragment " + group.name +
                          " is not in the fixed-function vocabulary";
            return out;
        }
        if (!foreign)
            if (const char* kind = foreignMaterial(group.name)) {
                foreign = &group;
                foreignKind = kind;
            }
        if (!unnamed && group.name.empty())
            unnamed = &group;
    }
    if (foreign) {
        out.kind = MaterialKind::Foreign;
        out.blocker = std::string("converted from a ") + foreignKind + ", not a StandardMaterial";
        return out;
    }
    if (unnamed) {
        char hex[11];
        std::snprintf(hex, sizeof hex, "0x%08x", unnamed->nameHash);
        out.kind = MaterialKind::UnknownFragment;
        out.blocker = std::string("fragment ") + hex + " is not in the recovered vocabulary";
        return out;
    }
    return out;
}

f32 readF32(const DataDrivenProperty* p, f32 fallback = 0.0f) {
    if (!p || p->data.size() < 4)
        return fallback;
    f32 v = 0.0f;
    std::memcpy(&v, p->data.data(), sizeof(v));
    return v;
}

u32 readU32(const DataDrivenProperty* p, u32 fallback = 0) {
    if (!p || p->data.size() < 4)
        return fallback;
    u32 v = 0;
    std::memcpy(&v, p->data.data(), sizeof(v));
    return v;
}

const DataDrivenProperty* find(const PropertyMap& props, const std::string& name) {
    auto it = props.find(name);
    return it == props.end() ? nullptr : it->second;
}

void applyTexture(const PropertyMap& props, const LayerSpec& spec, const DataDrivenMaterial& madd,
                  TextureLayer& layer, std::vector<std::string>& lossy) {
    const auto* tex = find(props, std::string("Tex") + spec.prefix);
    if (!tex || tex->data.size() < 8) {
        layer.flags |= TextureLayerFlag::Color; // a layer with no bitmap is a flat colour
        return;
    }
    u32 index = 0, source = 0;
    std::memcpy(&index, tex->data.data(), sizeof(index));
    std::memcpy(&source, tex->data.data() + 4, sizeof(source));
    if (index < madd.texturePaths.size())
        layer.texturePath = madd.texturePaths[index];
    else
        lossy.push_back(std::string("Tex") + spec.prefix + " indexes a missing texture path");
    layer.textureSource = source;
}

void applyChannelSelection(const PropertyMap& props, const LayerSpec& spec, TextureLayer& layer) {
    const auto* p = find(props, std::string(spec.prefix) + "ChannelSelection");
    if (!p || p->data.size() < 20)
        return;
    u32 selector = 0;
    f32 multiply = 1.0f, add = 0.0f;
    std::memcpy(&selector, p->data.data(), sizeof(selector));
    std::memcpy(&multiply, p->data.data() + 4, sizeof(multiply));
    std::memcpy(&add, p->data.data() + 8, sizeof(add));
    if (selector <= static_cast<u32>(ColorChannelSelect::Blue))
        layer.colorType = static_cast<ColorChannelSelect>(selector);
    layer.rgbMultiply.initValue = multiply;
    layer.rgbAdd.initValue = add;
    // Two booleans at byte 16/17; the two bytes after them are uninitialised.
    if (p->data[16])
        layer.flags |= TextureLayerFlag::ColorInvert;
    if (p->data[17])
        layer.flags |= TextureLayerFlag::ColorClamp;
}

void applyUV(const PropertyMap& props, const LayerSpec& spec, TextureLayer& layer) {
    layer.uvMapping =
        static_cast<UVMappingMode>(readU32(find(props, std::string(spec.prefix) + "UVSelection"), 0));

    const auto* p = find(props, std::string(spec.prefix) + "UVTransform");
    if (!p)
        p = find(props, std::string(spec.prefix) + "UVWTransform");
    if (!p || p->data.size() < 32)
        return;
    f32 w[7];
    std::memcpy(w, p->data.data(), sizeof(w));
    layer.uvOffset.initValue = {w[0], w[1]};
    layer.uvTiling.initValue = {w[2], w[3]};
    layer.uvAngle.initValue = {w[4], w[5], w[6]};
    if (p->data[30])
        layer.flags |= TextureLayerFlag::UVWrapX;
    if (p->data[31])
        layer.flags |= TextureLayerFlag::UVWrapY;
}

void applyFresnel(const PropertyMap& props, const LayerSpec& spec, TextureLayer& layer,
                  std::vector<std::string>& lossy) {
    const auto* p = find(props, std::string("FresnelParams") + spec.prefix);
    if (!p || p->data.size() < 48)
        return;
    u32 mode = 0;
    f32 f[11];
    std::memcpy(&mode, p->data.data(), sizeof(mode));
    std::memcpy(f, p->data.data() + 4, sizeof(f));

    layer.fresnelMode = static_cast<FresnelMode>(mode & 0x7);
    if (mode & ~0x7u)
        lossy.push_back(std::string("FresnelParams") + spec.prefix + " carries unmodelled flags 0x" +
                        [](u32 v) {
                            static const char* hex = "0123456789abcdef";
                            std::string s;
                            for (int shift = 28; shift >= 0; shift -= 4)
                                if (s.size() || ((v >> shift) & 0xF) || shift == 0)
                                    s += hex[(v >> shift) & 0xF];
                            return s;
                        }(mode & ~0x7u));
    layer.fresnelExponent = f[0];
    layer.fresnelMax = f[1];
    layer.fresnelMin = f[2];
    layer.fresnelTranslation = {f[3], f[4], f[5]};
    layer.fresnelRotation = {f[6], f[7]};
    layer.fresnelMask = {f[8], f[9], f[10]};
}

} // namespace

StandardMaterialConversion DataDrivenMaterial::toStandardMaterial() const {
    StandardMaterialConversion out;

    const DataDrivenProperties decoded = decodeProperties();
    if (!propertyBlob.empty() && decoded.groups.empty()) {
        out.blocker = "property blob failed to decode";
        return out;
    }

    PropertyMap props;
    std::vector<std::string> fragments;
    const Classification kind = classify(decoded);
    if (kind.kind != MaterialKind::FixedFunction) {
        out.blocker = kind.blocker;
        return out;
    }
    for (const auto& group : decoded.groups) {
        fragments.push_back(group.name);
        for (const auto& property : group.properties)
            if (!property.name.empty())
                props.emplace(property.name, &property);
    }

    auto has = [&fragments](const char* name) {
        return std::find(fragments.begin(), fragments.end(), name) != fragments.end();
    };

    StandardMaterial& mat = out.material;
    mat.name = materialName;

    if (has("TwoSided"))
        mat.flags |= MaterialFlag::TwoSided;
    if (!has("Fog"))
        mat.flags |= MaterialFlag::Unfogged;
    if (!has("LightingForward") && !has("LightingDeferred"))
        mat.flags |= MaterialFlag::Unshaded;
    if (has("ClampOutput"))
        mat.flags |= MaterialFlag::ClampOutput;
    if (has("DepthBlend"))
        mat.additionalFlags |= MaterialAdditionalFlag::DepthBlendFalloff;

    mat.specularExponent = readF32(find(props, "Specularity"), 20.0f);
    mat.depthBlendFalloff = readF32(find(props, "DepthBlendThreshhold"), 0.0f);
    // MADD stores the cut-off normalised; StandardMaterial stores it 0..255.
    mat.alphaTestThreshold = has("AlphaTest")
        ? static_cast<u32>(readF32(find(props, "AlphaTestThreshold"), 0.0f) * 255.0f + 0.5f)
        : 0;
    mat.parallaxHeight.initValue = readF32(find(props, "HeightMapScale"), 0.0f);

    for (const auto& spec : layerTable()) {
        const auto family =
            std::find_if(spec.families.begin(), spec.families.end(),
                         [&has](const char* name) { return has(name); });
        if (family == spec.families.end())
            continue;

        TextureLayer layer;
        applyTexture(props, spec, *this, layer, out.lossy);
        applyUV(props, spec, layer);
        applyChannelSelection(props, spec, layer);
        applyFresnel(props, spec, layer, out.lossy);

        if (const auto* c = find(props, std::string(spec.prefix) + "Constant")) {
            const u32 packed = readU32(c);
            layer.color.initValue = ColorBGRA{static_cast<u8>(packed & 0xFF),
                                              static_cast<u8>((packed >> 8) & 0xFF),
                                              static_cast<u8>((packed >> 16) & 0xFF),
                                              static_cast<u8>((packed >> 24) & 0xFF)};
        }
        if (const auto* ctrl = find(props, std::string(spec.prefix) + "Control")) {
            const u32 op = readU32(ctrl);
            if (spec.slot == &StandardMaterial::emissiveLayer1)
                mat.emissiveBlendMode1 = static_cast<LayerBlendOp>(op);
            else if (spec.slot == &StandardMaterial::emissiveLayer2)
                mat.emissiveBlendMode2 = static_cast<LayerBlendOp>(op);
        }
        if (const auto* alpha = find(props, std::string(spec.prefix) + "Alpha"))
            layer.mapAlpha.initValue = readF32(alpha, 1.0f);
        if (const auto* off = find(props, std::string(spec.prefix) + "TriplanarOffset");
            off && off->data.size() >= 12)
            std::memcpy(&layer.triplanarOffset.initValue, off->data.data(), 12);
        if (const auto* scale = find(props, std::string(spec.prefix) + "TriplanarScale");
            scale && scale->data.size() >= 12)
            std::memcpy(&layer.triplanarScale.initValue, scale->data.data(), 12);

        // The forward converter picks one of several fragments per slot; which one
        // encodes flags and a quality tier the standard form has no field for.
        if (std::string(*family) != spec.families.front())
            out.lossy.push_back(std::string("layer variant '") + *family + "' collapsed onto " +
                                spec.families.front());

        mat.*(spec.slot) = std::move(layer);
    }

    if (has("DiffuseTeamColor") || has("DiffuseConstantTeam") || has("EmissiveTeam") ||
        has("EmissiveConstantTeam") || has("Emissive2Team") || has("Emissive2ConstantTeam"))
        out.lossy.emplace_back("team-colour fragments have no direct StandardMaterial field");

    for (const auto& fragment : fragments)
        if (isPipelineFragment(fragment))
            out.lossy.push_back("pipeline fragment '" + fragment +
                                "' is derived from the model, not the material");

    out.lossy.emplace_back("animation links are not stored in the blob; every AnimRef is constant");
    out.converted = true;
    return out;
}

namespace {

// Texture roles the approximation can place. Ordered to match nothing in
// particular; the mapping onto layer slots is in approximationSlots().
enum class GraphRole { Diffuse, Normal, Specular, Emissive, Emissive2, Environment };

std::optional<TextureLayer> StandardMaterial::*graphSlot(GraphRole r) {
    switch (r) {
    case GraphRole::Diffuse: return &StandardMaterial::diffuseLayer;
    case GraphRole::Normal: return &StandardMaterial::normalLayer;
    case GraphRole::Specular: return &StandardMaterial::specularLayer;
    case GraphRole::Emissive: return &StandardMaterial::emissiveLayer1;
    case GraphRole::Emissive2: return &StandardMaterial::emissiveLayer2;
    case GraphRole::Environment: return &StandardMaterial::environmentLayer;
    }
    return &StandardMaterial::diffuseLayer;
}

const char* graphRoleName(GraphRole r) {
    switch (r) {
    case GraphRole::Diffuse: return "diffuse";
    case GraphRole::Normal: return "normal";
    case GraphRole::Specular: return "specular";
    case GraphRole::Emissive: return "emissive";
    case GraphRole::Emissive2: return "emissive2";
    case GraphRole::Environment: return "environment";
    }
    return "?";
}

// Signal 1: the node type settles the role on its own.
std::optional<GraphRole> roleFromNode(u32 fragmentHash, const std::string& property) {
    if (property == "TexNorm" || fragmentHash == 0x1d52e95eu)  // RailNormalTex
        return GraphRole::Normal;
    if (fragmentHash == 0x046d0f06u)                           // RailCubemapBlurred
        return GraphRole::Environment;
    return std::nullopt;
}

// Signal 2: the name the artist gave the node, held in extraHashes[i]. Only the
// five labels the corpus attests in bulk are listed; the long tail of labels
// seen four or five times is overfitted to one corpus and left out. Held-out
// 5-fold validation of the full learned table scored 545/547.
std::optional<GraphRole> roleFromNodeLabel(u32 label) {
    switch (label) {
    case 0xddea3ad8u: return GraphRole::Diffuse;      // 104 observations, one role
    case 0xf3fcee22u: return GraphRole::Normal;       // 97
    case 0xa9f7c812u: return GraphRole::Environment;  // 105
    case 0xa546033cu: return GraphRole::Specular;     // 104
    case 0x6d0d1d79u: return GraphRole::Emissive;     // 80
    default: return std::nullopt;
    }
}

// Signal 3: the texture filename. A convention, not a guarantee -- which is why
// it is the last resort and why the result is labelled an approximation.
std::optional<GraphRole> roleFromTexturePath(const std::string& raw) {
    std::string p;
    p.reserve(raw.size());
    for (char c : raw)
        p += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&p](const char* t) { return p.find(t) != std::string::npos; };
    if (has("_emis2") || has("_emissive2")) return GraphRole::Emissive2;
    if (has("_diff") || has("diffuse")) return GraphRole::Diffuse;
    if (has("_norm") || has("normal")) return GraphRole::Normal;
    if (has("_spec") || has("specular")) return GraphRole::Specular;
    if (has("_emis") || has("emissive")) return GraphRole::Emissive;
    if (has("reflection") || has("envio") || has("cubemap") || has("_env"))
        return GraphRole::Environment;
    return std::nullopt;
}

} // namespace

StandardMaterialConversion DataDrivenMaterial::approximateStandardMaterial() const {
    const DataDrivenProperties decoded = decodeProperties();

    if (classify(decoded).kind != MaterialKind::ShaderGraph)
        return toStandardMaterial();

    StandardMaterialConversion out;
    out.material.name = materialName;

    // extraHashes runs parallel to the node list, holding the name the artist
    // gave each node (0 for pipeline fragments, which are not nodes).
    const bool haveLabels = decoded.groups.size() == extraHashes.size();

    std::map<GraphRole, std::string> assigned;
    std::vector<std::string> dropped;
    size_t unassignedTextures = 0;
    bool lit = false, fogged = false, alphaTest = false;
    f32 specularity = -1.0f, alphaCutoff = -1.0f;

    for (size_t i = 0; i < decoded.groups.size(); ++i) {
        const DataDrivenGroup& group = decoded.groups[i];
        const std::string& type = group.name;

        if (type == "Fog") fogged = true;
        else if (type == "RailAlphaTest") alphaTest = true;
        else if (type == "RailLighting" || type == "RailToonLighting") lit = true;
        else if (type.rfind("RailRGB", 0) == 0 || type.rfind("RailScalar", 0) == 0 ||
                 type.rfind("RailUV", 0) == 0 || type == "RailNormalLerp" ||
                 type == "RailCombineNormals" || type == "RailConstantColor" ||
                 type == "RailFloat" || type == "RailFloat4")
            dropped.push_back(type.empty() ? "<unnamed node>" : type);

        for (const DataDrivenProperty& property : group.properties) {
            if (property.name == "Specularity")
                specularity = readF32(&property, specularity);
            if (property.name == "AlphaTestThreshold")
                alphaCutoff = readF32(&property, alphaCutoff);
            if ((property.name != "Tex" && property.name != "TexNorm") ||
                property.data.size() < 8)
                continue;

            u32 index = 0;
            std::memcpy(&index, property.data.data(), sizeof(index));
            if (index >= texturePaths.size() || texturePaths[index].empty())
                continue;
            const std::string& path = texturePaths[index];

            const std::optional<GraphRole> fromPath = roleFromTexturePath(path);
            std::optional<GraphRole> role = roleFromNode(group.nameHash, property.name);
            if (!role && haveLabels)
                role = roleFromNodeLabel(extraHashes[i]);
            if (!role)
                role = fromPath;
            if (!role) {
                unassignedTextures++;
                continue;
            }
            // Node labels are generic where filenames are not -- one "Emissive"
            // label covers both emissive slots. On a clash the filename wins,
            // because it is the signal that distinguishes them.
            if (assigned.count(*role) && fromPath && !assigned.count(*fromPath))
                role = fromPath;

            auto it = assigned.find(*role);
            if (it == assigned.end())
                assigned.emplace(*role, path);
            else if (it->second != path)
                out.lossy.push_back(std::string("two textures claim the ") + graphRoleName(*role) +
                                    " role; kept " + it->second);
        }
    }

    if (assigned.empty()) {
        out.blocker = "shader-graph material: no sampled texture could be assigned a role";
        return out;
    }
    if (!assigned.count(GraphRole::Diffuse) && !assigned.count(GraphRole::Emissive)) {
        out.blocker = "shader-graph material: neither a diffuse nor an emissive texture";
        return out;
    }

    StandardMaterial& mat = out.material;
    for (const auto& [role, path] : assigned) {
        TextureLayer layer;
        layer.texturePath = path;
        mat.*(graphSlot(role)) = std::move(layer);
    }
    mat.specularExponent = specularity >= 0.0f ? specularity : 20.0f;
    if (!fogged)
        mat.flags |= MaterialFlag::Unfogged;
    if (!lit)
        mat.flags |= MaterialFlag::Unshaded;
    if (alphaTest)
        mat.alphaTestThreshold =
            static_cast<u32>((alphaCutoff >= 0.0f ? alphaCutoff : 0.0f) * 255.0f + 0.5f);

    out.lossy.emplace_back(
        "approximated from a shader graph: the blob stores nodes but not the edges between "
        "them, so the graph topology is gone and the layers below are inferred");
    if (!dropped.empty()) {
        std::string list;
        for (const auto& d : dropped)
            list += (list.empty() ? "" : ", ") + d;
        out.lossy.push_back(std::to_string(dropped.size()) +
                            " arithmetic/constant/UV nodes were dropped: " + list);
    }
    if (unassignedTextures)
        out.lossy.push_back(std::to_string(unassignedTextures) +
                            " sampled textures had no role signal and were dropped");
    out.lossy.emplace_back("animation links are not stored in the blob; every AnimRef is constant");
    out.converted = true;
    return out;
}

} // namespace m3
} // namespace whiteout
