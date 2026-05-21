// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "mdl_converter.h"
#include "mdl_parser.h"

#include <whiteout/models/mdx/structures.h>
#include <whiteout/vector_types.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace whiteout {
namespace mdx {

namespace {

// ============================================================================
// Utility helpers
// ============================================================================

// Case-insensitive string comparison
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Get a child property by name from a node's children list.
// Returns nullptr if not found.
const MdlProperty* findProp(const MdlNode& node, std::string_view name) {
    for (auto& child : node.children) {
        if (auto* p = std::get_if<MdlProperty>(&child)) {
            if (p->name == name) return p;
        }
    }
    return nullptr;
}

// Get a child sub-block by name from a node's children list.
const MdlNode* findBlock(const MdlNode& node, std::string_view name) {
    for (auto& child : node.children) {
        if (auto* n = std::get_if<MdlNode>(&child)) {
            if (n->name == name) return n;
        }
    }
    return nullptr;
}

// Get a child anim track by name from a node's children list.
const MdlAnimTrack* findTrack(const MdlNode& node, std::string_view name) {
    for (auto& child : node.children) {
        if (auto* t = std::get_if<MdlAnimTrack>(&child)) {
            if (t->name == name) return t;
        }
    }
    return nullptr;
}

// Extract a float from a property's first value (or from static value)
f32 floatProp(const MdlNode& node, std::string_view name, f32 def = 0.0f) {
    if (auto* p = findProp(node, name)) {
        if (!p->values.empty() && p->values[0].isNumber())
            return static_cast<f32>(p->values[0].asNumber());
    }
    return def;
}

// Extract a u32 from a property's first value
u32 u32Prop(const MdlNode& node, std::string_view name, u32 def = 0) {
    if (auto* p = findProp(node, name)) {
        if (!p->values.empty() && p->values[0].isNumber())
            return static_cast<u32>(p->values[0].asNumber());
    }
    return def;
}

// Extract a string from a property's first value
std::string stringProp(const MdlNode& node, std::string_view name,
                       const std::string& def = "") {
    if (auto* p = findProp(node, name)) {
        if (!p->values.empty() && p->values[0].isString()) return p->values[0].asString();
    }
    return def;
}

// Extract a Vector3f from a property's first value (which should be an array of 3)
Vector3f vec3Prop(const MdlNode& node, std::string_view name,
                  Vector3f def = Vector3f(0, 0, 0)) {
    if (auto* p = findProp(node, name)) {
        if (!p->values.empty() && p->values[0].isArray()) {
            auto& arr = p->values[0].asArray();
            if (arr.size() >= 3 && arr[0].isNumber() && arr[1].isNumber() &&
                arr[2].isNumber()) {
                return Vector3f(static_cast<f32>(arr[0].asNumber()),
                                static_cast<f32>(arr[1].asNumber()),
                                static_cast<f32>(arr[2].asNumber()));
            }
        }
    }
    return def;
}

// Check if a bare flag property exists
bool hasFlag(const MdlNode& node, std::string_view name) {
    return findProp(node, name) != nullptr;
}

// Case-insensitive flag check. MDL exporters disagree on casing for
// acronym-bearing flags — e.g. Blizzard's exporter writes `EmitterUsesMDL`
// while some tools emit `EmitterUsesMdl`.
bool hasFlagCI(const MdlNode& node, std::string_view name) {
    for (auto& child : node.children) {
        if (auto* p = std::get_if<MdlProperty>(&child)) {
            if (iequals(p->name, name)) return true;
        }
    }
    return false;
}

// Parse an Extent from a node's MinimumExtent/MaximumExtent/BoundsRadius properties
Extent parseExtent(const MdlNode& node) {
    Extent e;
    e.minimum = vec3Prop(node, "MinimumExtent");
    e.maximum = vec3Prop(node, "MaximumExtent");
    e.boundsRadius = floatProp(node, "BoundsRadius");
    return e;
}

// Convert interpolation string to enum
InterpolationType parseInterpolation(const std::string& s) {
    if (s == "Linear") return InterpolationType::Linear;
    if (s == "Hermite") return InterpolationType::Hermite;
    if (s == "Bezier") return InterpolationType::Bezier;
    return InterpolationType::None; // "DontInterp" or empty
}

// Extract a float value from an MdlValue
f32 valueToFloat(const MdlValue& v) {
    if (v.isNumber()) return static_cast<f32>(v.asNumber());
    return 0.0f;
}

// Extract a u32 value from an MdlValue
u32 valueToU32(const MdlValue& v) {
    if (v.isNumber()) return static_cast<u32>(v.asNumber());
    return 0;
}

// Extract a Vector3f from an MdlValue (expects array of 3)
Vector3f valueToVec3(const MdlValue& v) {
    if (v.isArray()) {
        auto& arr = v.asArray();
        if (arr.size() >= 3)
            return Vector3f(static_cast<f32>(arr[0].asNumber()),
                            static_cast<f32>(arr[1].asNumber()),
                            static_cast<f32>(arr[2].asNumber()));
    }
    return Vector3f(0, 0, 0);
}

// Extract a Quaternion (XYZW) from an MdlValue (expects array of 4)
Quaternion valueToQuat(const MdlValue& v) {
    if (v.isArray()) {
        auto& arr = v.asArray();
        if (arr.size() >= 4)
            return Quaternion(static_cast<f32>(arr[0].asNumber()),
                              static_cast<f32>(arr[1].asNumber()),
                              static_cast<f32>(arr[2].asNumber()),
                              static_cast<f32>(arr[3].asNumber()));
    }
    return Quaternion(0, 0, 0, 1);
}

// Extract a Vector4f from an MdlValue (expects array of 4)
Vector4f valueToVec4(const MdlValue& v) {
    if (v.isArray()) {
        auto& arr = v.asArray();
        if (arr.size() >= 4)
            return Vector4f(static_cast<f32>(arr[0].asNumber()),
                            static_cast<f32>(arr[1].asNumber()),
                            static_cast<f32>(arr[2].asNumber()),
                            static_cast<f32>(arr[3].asNumber()));
    }
    return Vector4f(0, 0, 0, 0);
}

// ============================================================================
// Track conversion: MdlAnimTrack → Track<T>
// ============================================================================

// Convert an MdlValue to a typed value T.  Specializations below.
template <typename T>
T convertValue(const MdlValue& v);

template <>
f32 convertValue<f32>(const MdlValue& v) {
    return valueToFloat(v);
}

template <>
u32 convertValue<u32>(const MdlValue& v) {
    return valueToU32(v);
}

template <>
Vector3f convertValue<Vector3f>(const MdlValue& v) {
    return valueToVec3(v);
}

template <>
Quaternion convertValue<Quaternion>(const MdlValue& v) {
    return valueToQuat(v);
}

template <>
[[maybe_unused]] Vector4f convertValue<Vector4f>(const MdlValue& v) {
    return valueToVec4(v);
}

// Build a Track<T> from an MdlAnimTrack. Also handles "static PROP value" properties
// that become a single-key track with no interpolation.
//
// In-memory layout (post split-timestamp refactor):
//   timestamps : one u32 per keyframe
//   keys_data  : keyCount values for non-smooth, keyCount * 3 for smooth
//                (laid out as [value, inTan, outTan, value, ...])
template <typename T>
Track<T> buildTrack(const MdlAnimTrack& atrack) {
    Track<T> track;
    track.isUsed = true;
    track.interpolationType = parseInterpolation(atrack.interpolation);
    track.globalSequenceId = atrack.globalSequenceId;
    track.keyCount = atrack.keyframes.size();

    const bool smooth = isSmoothInterpolation(track.interpolationType);

    track.timestamps.reserve(track.keyCount);
    track.keys_data.reserve(track.keyCount * (smooth ? 3 : 1));

    for (auto& kf : atrack.keyframes) {
        track.timestamps.push_back(static_cast<u32>(kf.time));
        track.keys_data.push_back(convertValue<T>(kf.value));
        if (smooth) {
            // Smooth tracks always carry 3 components per key on the in-memory
            // side; default missing tangents to a zero-initialized T.
            track.keys_data.push_back(kf.hasTangents ? convertValue<T>(kf.inTan) : T{});
            track.keys_data.push_back(kf.hasTangents ? convertValue<T>(kf.outTan) : T{});
        }
    }

    return track;
}

// Convert a static property value into a Track with a single keyframe at frame 0.
template <typename T>
Track<T> buildStaticTrack(const T& value) {
    Track<T> track;
    track.isUsed = true;
    track.interpolationType = InterpolationType::None;
    track.globalSequenceId = 0xFFFFFFFF;
    track.keyCount = 1;
    track.timestamps.push_back(0);
    track.keys_data.push_back(value);
    return track;
}

// Get a Track<T> from a node. Only an animated `Name { N { ... } }` block
// yields a track; a `static Name value` property does NOT — it carries the
// scalar field, which every caller reads separately via getFloatOrStatic /
// floatProp / vec3Prop. Promoting `static` to a one-key track here would make
// the MDX writer emit a spurious K*** animation chunk that the source model
// never had, breaking the binary round-trip.
template <typename T>
Track<T> getTrack(const MdlNode& node, std::string_view name) {
    if (auto* t = findTrack(node, name)) {
        return buildTrack<T>(*t);
    }
    return Track<T>{};
}

// Overload: get a float value (either animated track or static property value)
f32 getFloatOrStatic(const MdlNode& node, std::string_view name, f32 def = 0.0f) {
    if (auto* p = findProp(node, name)) {
        if (!p->values.empty() && p->values[0].isNumber())
            return static_cast<f32>(p->values[0].asNumber());
    }
    return def;
}

// ============================================================================
// Node parsing (common for Bone, Helper, Light, Attachment, etc.)
// ============================================================================

Node parseNodeFields(const MdlNode& block, Node::NodeType type) {
    Node node;
    node.type = type;

    // Name from block header param
    if (!block.headerParams.empty() && block.headerParams[0].isString()) {
        node.name = block.headerParams[0].asString();
    }

    node.objectId = u32Prop(block, "ObjectId");

    // Parent: can be "Parent N, // comment"
    if (auto* p = findProp(block, "Parent")) {
        if (!p->values.empty() && p->values[0].isNumber())
            node.parentId = static_cast<u32>(p->values[0].asNumber());
    } else {
        node.parentId = Node::NO_PARENT;
    }

    // Flags
    node.flags = Node::NodeFlag::None;

    // Set type flags
    switch (type) {
        case Node::NodeType::Bone:
            node.flags = node.flags | Node::NodeFlag::Bone;
            break;
        case Node::NodeType::Light:
            node.flags = node.flags | Node::NodeFlag::Light;
            break;
        case Node::NodeType::EventObject:
            node.flags = node.flags | Node::NodeFlag::EventObject;
            break;
        case Node::NodeType::Attachment:
            node.flags = node.flags | Node::NodeFlag::Attachment;
            break;
        case Node::NodeType::ParticleEmitter:
            node.flags = node.flags | Node::NodeFlag::ParticleEmitter;
            break;
        case Node::NodeType::ParticleEmitter2:
            node.flags = node.flags | Node::NodeFlag::ParticleEmitter;
            break;
        case Node::NodeType::CollisionShape:
            node.flags = node.flags | Node::NodeFlag::CollisionShape;
            break;
        case Node::NodeType::RibbonEmitter:
            node.flags = node.flags | Node::NodeFlag::RibbonEmitter;
            break;
        case Node::NodeType::CornEmitter:
            node.flags = node.flags | Node::NodeFlag::ParticleEmitter;
            break;
        default:
            break;
    }

    // Parse flag identifiers
    if (hasFlag(block, "DontInheritTranslation"))
        node.flags = node.flags | Node::NodeFlag::DontInheritTranslation;
    if (hasFlag(block, "DontInheritRotation"))
        node.flags = node.flags | Node::NodeFlag::DontInheritRotation;
    if (hasFlag(block, "DontInheritScaling"))
        node.flags = node.flags | Node::NodeFlag::DontInheritScaling;
    if (hasFlag(block, "Billboarded"))
        node.flags = node.flags | Node::NodeFlag::Billboarded;
    if (hasFlag(block, "BillboardedLockX"))
        node.flags = node.flags | Node::NodeFlag::BillboardedLockX;
    if (hasFlag(block, "BillboardedLockY"))
        node.flags = node.flags | Node::NodeFlag::BillboardedLockY;
    if (hasFlag(block, "BillboardedLockZ"))
        node.flags = node.flags | Node::NodeFlag::BillboardedLockZ;
    if (hasFlag(block, "CameraAnchored"))
        node.flags = node.flags | Node::NodeFlag::CameraAnchored;
    if (hasFlag(block, "Unshaded"))
        node.flags = node.flags | Node::NodeFlag::Unshaded;
    if (hasFlag(block, "SortPrimsFarZ"))
        node.flags = node.flags | Node::NodeFlag::SortPrimitives;
    // Bits 0x20000 / 0x40000 carry different meanings for Popcorn emitters
    // than for the other emitter types, so we branch on the node kind.
    if (type == Node::NodeType::CornEmitter) {
        if (hasFlag(block, "Unfogged"))
            node.flags = node.flags | Node::NodeFlag::PopcornUnfogged;
        if (hasFlag(block, "PopcornScaling"))
            node.flags = node.flags | Node::NodeFlag::PopcornScaling;
    } else {
        if (hasFlag(block, "LineEmitter"))
            node.flags = node.flags | Node::NodeFlag::LineEmitter;     // 0x20000
        if (hasFlag(block, "Unfogged"))
            node.flags = node.flags | Node::NodeFlag::Unfogged;        // 0x40000
    }
    if (hasFlag(block, "ModelSpace"))
        node.flags = node.flags | Node::NodeFlag::ModelSpace;
    if (hasFlag(block, "XYQuad"))
        node.flags = node.flags | Node::NodeFlag::XYQuad;

    // Transform tracks
    node.translationTracks = getTrack<Vector3f>(block, "Translation");
    node.rotationTracks = getTrack<Quaternion>(block, "Rotation");
    node.scalingTracks = getTrack<Vector3f>(block, "Scaling");

    return node;
}

// ============================================================================
// Block converters
// ============================================================================

void convertVersion(const MdlNode& block, Model& model) {
    model.version = u32Prop(block, "FormatVersion", 800);
}

void convertModel(const MdlNode& block, Model& model) {
    if (!block.headerParams.empty() && block.headerParams[0].isString()) {
        model.modelName = block.headerParams[0].asString();
    }
    model.animationFileName = stringProp(block, "AnimationFileName");
    model.blendTime = u32Prop(block, "BlendTime");

    model.modelExtent = parseExtent(block);
}

void convertSequences(const MdlNode& block, Model& model) {
    for (auto& child : block.children) {
        if (auto* anim = std::get_if<MdlNode>(&child)) {
            if (anim->name != "Anim") continue;
            Sequence seq;
            if (!anim->headerParams.empty() && anim->headerParams[0].isString()) {
                seq.name = anim->headerParams[0].asString();
            }
            // Interval { start, end }
            if (auto* p = findProp(*anim, "Interval")) {
                if (!p->values.empty() && p->values[0].isArray()) {
                    auto& arr = p->values[0].asArray();
                    if (arr.size() >= 2) {
                        seq.intervalStart = static_cast<u32>(arr[0].asNumber());
                        seq.intervalEnd = static_cast<u32>(arr[1].asNumber());
                    }
                }
            }
            seq.moveSpeed = floatProp(*anim, "MoveSpeed");
            seq.rarity = floatProp(*anim, "Rarity");
            if (hasFlag(*anim, "NonLooping")) seq.flags |= Sequence::Flag::NonLooping;
            seq.extent = parseExtent(*anim);
            model.sequences.push_back(std::move(seq));
        }
    }
}

void convertGlobalSequences(const MdlNode& block, Model& model) {
    // GlobalSequences N { val, val, ... }
    // Children are properties with numeric values and no name (parsed as bare values).
    // Actually, in MDL: GlobalSequences N { Duration1, Duration2, ... }
    // The parser sees these as properties (bare numbers with comma).
    // But they might also be parsed as a flat list. Let's handle both.
    for (auto& child : block.children) {
        if (auto* p = std::get_if<MdlProperty>(&child)) {
            if (p->name == "Duration" && !p->values.empty() && p->values[0].isNumber()) {
                model.globalSequences.push_back(static_cast<u32>(p->values[0].asNumber()));
            } else if (p->name.empty() && !p->values.empty() && p->values[0].isNumber()) {
                // Anonymous bare number value
                model.globalSequences.push_back(static_cast<u32>(p->values[0].asNumber()));
            }
        }
    }
}

void convertTextures(const MdlNode& block, Model& model) {
    for (auto& child : block.children) {
        if (auto* bitmap = std::get_if<MdlNode>(&child)) {
            if (bitmap->name != "Bitmap") continue;
            Texture tex;
            tex.fileName = stringProp(*bitmap, "Image");
            tex.replaceableId = u32Prop(*bitmap, "ReplaceableId");
            if (hasFlag(*bitmap, "WrapWidth")) tex.flags |= Texture::Flag::WrapWidth;
            if (hasFlag(*bitmap, "WrapHeight")) tex.flags |= Texture::Flag::WrapHeight;
            model.textures.push_back(std::move(tex));
        }
    }
}

void convertMaterials(const MdlNode& block, Model& model) {
    for (auto& child : block.children) {
        if (auto* matNode = std::get_if<MdlNode>(&child)) {
            if (matNode->name != "Material") continue;
            Material mat;
            mat.priorityPlane = u32Prop(*matNode, "PriorityPlane");
            mat.flags = static_cast<Material::Flag>(u32Prop(*matNode, "Flags"));
            mat.shader = stringProp(*matNode, "Shader");

            // Flags -- accept both HiveWorkshop and Warcraft III dialect names
            if (hasFlag(*matNode, "ConstantColor"))   mat.flags |= Material::Flag::ConstantColor;
            if (hasFlag(*matNode, "TwoSided"))        mat.flags |= Material::Flag::TwoSided;
            if (hasFlag(*matNode, "Unfogged"))        mat.flags |= Material::Flag::Unfogged;
            if (hasFlag(*matNode, "SortPrimsNearZ"))  mat.flags |= Material::Flag::SortPrimsNearZ;
            if (hasFlag(*matNode, "SortPrimsFarZ"))   mat.flags |= Material::Flag::SortPrimsFarZ;
            if (hasFlag(*matNode, "SortPrimitives"))  mat.flags |= Material::Flag::SortPrimsFarZ;
            if (hasFlag(*matNode, "FullResolution"))  mat.flags |= Material::Flag::FullResolution;

            // Parse layers
            for (auto& mc : matNode->children) {
                if (auto* layerNode = std::get_if<MdlNode>(&mc)) {
                    if (layerNode->name != "Layer") continue;
                    Layer layer;

                    // FilterMode
                    if (auto* p = findProp(*layerNode, "FilterMode")) {
                        if (!p->values.empty() && p->values[0].isString()) {
                            auto& fm = p->values[0].asString();
                            if (fm == "None")
                                layer.filterMode = Layer::FilterMode::None;
                            else if (fm == "Transparent")
                                layer.filterMode = Layer::FilterMode::Transparent;
                            else if (fm == "Blend")
                                layer.filterMode = Layer::FilterMode::Blend;
                            else if (fm == "Additive")
                                layer.filterMode = Layer::FilterMode::Additive;
                            else if (fm == "AddAlpha")
                                layer.filterMode = Layer::FilterMode::AddAlpha;
                            else if (fm == "Modulate")
                                layer.filterMode = Layer::FilterMode::Modulate;
                            else if (fm == "Modulate2x")
                                layer.filterMode = Layer::FilterMode::Modulate2x;
                        }
                    }

                    // Shading flags -- accept both HiveWorkshop and Warcraft III dialect names
                    layer.shadingFlags = Layer::ShadingFlag::None;
                    if (hasFlag(*layerNode, "Unshaded"))
                        layer.shadingFlags |= Layer::ShadingFlag::Unshaded;
                    if (hasFlag(*layerNode, "SphereEnvMap"))
                        layer.shadingFlags |= Layer::ShadingFlag::SphereEnvMap;
                    if (hasFlag(*layerNode, "WrapWidth"))
                        layer.shadingFlags |= Layer::ShadingFlag::WrapWidth;
                    if (hasFlag(*layerNode, "WrapHeight"))
                        layer.shadingFlags |= Layer::ShadingFlag::WrapHeight;
                    if (hasFlag(*layerNode, "TwoSided"))
                        layer.shadingFlags |= Layer::ShadingFlag::TwoSided;
                    if (hasFlag(*layerNode, "Unfogged"))
                        layer.shadingFlags |= Layer::ShadingFlag::Unfogged;
                    if (hasFlag(*layerNode, "NoDepthTest"))
                        layer.shadingFlags |= Layer::ShadingFlag::NoDepthTest;
                    if (hasFlag(*layerNode, "NoDepthSet"))
                        layer.shadingFlags |= Layer::ShadingFlag::NoDepthSet;
                    if (hasFlag(*layerNode, "Unlit"))
                        layer.shadingFlags |= Layer::ShadingFlag::Unlit;

                    // Per-layer `Shader "name",` directive maps to ShaderType.
                    if (auto* p = findProp(*layerNode, "Shader")) {
                        if (!p->values.empty() && p->values[0].isString()) {
                            const auto& name = p->values[0].asString();
                            if (name == "Shader_SD_Legacy")
                                layer.shader = Layer::ShaderType::SD;
                            else if (name == "Shader_HD_DefaultUnit")
                                layer.shader = Layer::ShaderType::HD;
                            else if (name == "Shader_SD_FixedFunction")
                                layer.shader = Layer::ShaderType::SDOnHD;
                            else if (name == "Shader_HD_Crystal")
                                layer.shader = Layer::ShaderType::Crystal;
                            // Unknown shader names: leave as default
                        }
                    }
                    // HiveWorkshop fallback: `ShaderTypeId 1,` for HD detection
                    if (auto* p = findProp(*layerNode, "ShaderTypeId")) {
                        if (!p->values.empty() && p->values[0].isNumber()) {
                            layer.shader = static_cast<Layer::ShaderType>(p->values[0].asNumber());
                        }
                    }
                    layer.is_hd = (layer.shader == Layer::ShaderType::HD ||
                                   layer.shader == Layer::ShaderType::Crystal);

                    // Texture parsing -- accept both engine and HiveWorkshop styles:
                    //   Engine:       `static TextureID 5 <= 1,`  (slot designator)
                    //   HiveWorkshop: `NormalTextureID 5,` / `ORMTextureID 5,` ...
                    // Both produce entries in layer.subTextures. Plain `TextureID 5,`
                    // without a slot suffix falls back to legacy layer.textureId.
                    auto slotForName = [](const std::string& n) -> Layer::SlotType {
                        if (n == "NormalTextureID")        return Layer::SlotType::NormalMap;
                        if (n == "ORMTextureID")           return Layer::SlotType::ORMMap;
                        if (n == "EmissiveTextureID")      return Layer::SlotType::EmissiveMap;
                        if (n == "TeamColorTextureID")     return Layer::SlotType::TeamColor;
                        if (n == "ReflectionsTextureID")   return Layer::SlotType::EnvironmentMap;
                        if (n == "TextureID")              return Layer::SlotType::DiffuseMap;
                        return Layer::SlotType::Unknown;
                    };


                    auto texSlotForProp = [&](const MdlProperty& p) -> Layer::SlotType {
                        if (p.name == "TextureID")
                            return p.slot.has_value()
                                       ? static_cast<Layer::SlotType>(p.slot.value())
                                       : Layer::SlotType::DiffuseMap;
                        return slotForName(p.name);
                    };

                    for (auto& mc2 : layerNode->children) {
                        auto* prop = std::get_if<MdlProperty>(&mc2);
                        if (!prop) continue;

                        if (model.version < 1100) {
                            if (prop->name == "TextureID") {
                                if (!prop->values.empty() && prop->values[0].isNumber()) {
                                    layer.textureId = static_cast<u32>(prop->values[0].asNumber());
                                }
                                continue;
                            }
                        }

                        // Warcraft III slot form: `static TextureID N <= S,`
                        if (prop->name == "TextureID" && prop->slot.has_value()) {
                            Layer::SubTexture sub;
                            sub.slot = slot;
                            if (!prop->values.empty() && prop->values[0].isNumber())
                                sub.textureId =
                                    static_cast<u32>(prop->values[0].asNumber());
                            layer.subTextures.push_back(std::move(sub));
                            continue;
                        }
                        // HiveWorkshop named-slot form (handles both static and animated)
                        Layer::SlotType namedSlot = slotForName(prop->name);
                        if (namedSlot != Layer::SlotType::Unknown) {
                            Layer::SubTexture sub;
                            sub.slot = slot;
                            sub.tracks = buildTrack<u32>(*track);
                            layer.subTextures.push_back(std::move(sub));
                        }
                    }
                    // Animated HiveWorkshop named slots without a matching static prop
                    // (e.g. `NormalTextureID 5 { Linear, ... },` with no separate
                    // `NormalTextureID 5,`) are parsed as MdlAnimTrack children, so
                    // also walk those:
                    for (auto& mc2 : layerNode->children) {
                        auto* track = std::get_if<MdlAnimTrack>(&mc2);
                        if (!track) continue;

                        if (model.version < 1100) {
                            if (track->name == "TextureID") {
                                layer.textureIdTracks = buildTrack<u32>(*track);
                                continue;
                            }
                        }

                        Layer::SlotType namedSlot = slotForName(track->name);
                        if (namedSlot == Layer::SlotType::Unknown) continue;
                        // Skip if a static prop with the same name was already added above.
                        bool already = false;
                        for (auto& s : layer.subTextures) {
                            if (s.slot == namedSlot && s.tracks.isUsed) { already = true; break; }
                        }
                        if (already) continue;

                        Layer::SubTexture sub;
                        sub.slot = namedSlot;
                        sub.tracks = buildTrack<u32>(*track);
                        layer.subTextures.push_back(std::move(sub));
                    }

                    layer.alpha = floatProp(*layerNode, "Alpha", 1.0f);
                    layer.textureAnimationId =
                        u32Prop(*layerNode, "TVertexAnimId", 0xFFFFFFFF);
                    layer.coordId = u32Prop(*layerNode, "CoordId");

                    // Animation tracks
                    layer.alphaTracks = getTrack<f32>(*layerNode, "Alpha");

                    // Reforged PBR. emissiveGain's default value is 1.0, so a
                    // layer with no `static EmissiveGain` line (absent, or
                    // animated by a track that shadows the base scalar) reads
                    // back as 1.0.
                    layer.emissiveGain = floatProp(*layerNode, "EmissiveGain", 1.0f);
                    layer.fresnelColor =
                        vec3Prop(*layerNode, "FresnelColor", Vector3f(1, 1, 1));
                    layer.fresnelOpacity = floatProp(*layerNode, "FresnelOpacity");
                    layer.fresnelTeamColor =
                        floatProp(*layerNode, "FresnelTeamColor");

                    layer.emissiveGainTracks =
                        getTrack<f32>(*layerNode, "EmissiveGain");
                    layer.fresnelColorTracks =
                        getTrack<Vector3f>(*layerNode, "FresnelColor");
                    layer.fresnelAlphaTracks =
                        getTrack<f32>(*layerNode, "FresnelOpacity");
                    layer.fresnelTeamColorTracks =
                        getTrack<f32>(*layerNode, "FresnelTeamColor");

                    mat.layers.push_back(std::move(layer));
                }
            }
            model.materials.push_back(std::move(mat));
        }
    }
}

void convertTextureAnims(const MdlNode& block, Model& model) {
    for (auto& child : block.children) {
        if (auto* taNode = std::get_if<MdlNode>(&child)) {
            if (taNode->name != "TVertexAnim") continue;
            TextureAnimation ta;
            ta.translationTracks = getTrack<Vector3f>(*taNode, "Translation");
            ta.rotationTracks = getTrack<Quaternion>(*taNode, "Rotation");
            ta.scalingTracks = getTrack<Vector3f>(*taNode, "Scaling");
            model.textureAnimations.push_back(std::move(ta));
        }
    }
}

void convertGeoset(const MdlNode& block, Model& model) {
    Geoset geo;

    for (auto& child : block.children) {
        if (auto* sub = std::get_if<MdlNode>(&child)) {
            if (sub->name == "Vertices") {
                for (auto& vc : sub->children) {
                    if (auto* vp = std::get_if<MdlProperty>(&vc)) {
                        if (!vp->values.empty() && vp->values[0].isArray()) {
                            geo.vertexPositions.push_back(
                                valueToVec3(vp->values[0]));
                        }
                    }
                }
            } else if (sub->name == "Normals") {
                for (auto& vc : sub->children) {
                    if (auto* vp = std::get_if<MdlProperty>(&vc)) {
                        if (!vp->values.empty() && vp->values[0].isArray()) {
                            geo.vertexNormals.push_back(
                                valueToVec3(vp->values[0]));
                        }
                    }
                }
            } else if (sub->name == "TVertices") {
                std::vector<Vector2f> uvs;
                for (auto& vc : sub->children) {
                    if (auto* vp = std::get_if<MdlProperty>(&vc)) {
                        if (!vp->values.empty() && vp->values[0].isArray()) {
                            auto& arr = vp->values[0].asArray();
                            if (arr.size() >= 2) {
                                uvs.push_back(Vector2f(
                                    static_cast<f32>(arr[0].asNumber()),
                                    static_cast<f32>(arr[1].asNumber())));
                            }
                        }
                    }
                }
                geo.textureCoordinateSets.push_back(std::move(uvs));
            } else if (sub->name == "VertexGroup") {
                // Fallback: VertexGroup parsed as MdlNode (with header param count)
                for (auto& vc : sub->children) {
                    if (auto* vp = std::get_if<MdlProperty>(&vc)) {
                        if (!vp->values.empty() && vp->values[0].isNumber()) {
                            geo.vertexGroups.push_back(static_cast<u8>(
                                vp->values[0].asNumber()));
                        }
                    }
                }
            } else if (sub->name == "Faces") {
                // Faces N M { Triangles { { idx, idx, ... }, } }
                for (auto& fc : sub->children) {
                    if (auto* triBlock = std::get_if<MdlNode>(&fc)) {
                        if (triBlock->name == "Triangles") {
                            for (auto& tc : triBlock->children) {
                                if (auto* tp = std::get_if<MdlProperty>(&tc)) {
                                    if (!tp->values.empty() &&
                                        tp->values[0].isArray()) {
                                        for (auto& idx :
                                             tp->values[0].asArray()) {
                                            if (idx.isNumber())
                                                geo.faces.push_back(
                                                    static_cast<u16>(
                                                        idx.asNumber()));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                // Set face type/groups
                if (!geo.faces.empty()) {
                    geo.faceTypeGroups.push_back(4); // triangles
                    geo.faceGroups.push_back(
                        static_cast<u32>(geo.faces.size()));
                }
            } else if (sub->name == "Groups") {
                // Groups N M { Matrices { idx, idx, ... }, ... }
                // Matrices is parsed as a property (IDENT { Number })
                for (auto& gc : sub->children) {
                    if (auto* mp = std::get_if<MdlProperty>(&gc)) {
                        if (mp->name == "Matrices" && !mp->values.empty() &&
                            mp->values[0].isArray()) {
                            auto& arr = mp->values[0].asArray();
                            for (auto& v : arr) {
                                if (v.isNumber())
                                    geo.matrixIndices.push_back(
                                        static_cast<u32>(v.asNumber()));
                            }
                            geo.matrixGroups.push_back(
                                static_cast<u32>(arr.size()));
                        }
                    }
                }
            } else if (sub->name == "Tangents") {
                for (auto& vc : sub->children) {
                    if (auto* vp = std::get_if<MdlProperty>(&vc)) {
                        if (!vp->values.empty() && vp->values[0].isArray()) {
                            geo.tangents.push_back(
                                valueToVec4(vp->values[0]));
                        }
                    }
                }
            } else if (sub->name == "SkinWeights") {
                // SkinWeights N { { b0 b1 b2 b3 w0 w1 w2 w3 }, ... }
                for (auto& vc : sub->children) {
                    if (auto* vp = std::get_if<MdlProperty>(&vc)) {
                        if (!vp->values.empty() && vp->values[0].isArray()) {
                            auto& arr = vp->values[0].asArray();
                            for (auto& elem : arr) {
                                if (elem.isNumber())
                                    geo.skinData.push_back(static_cast<u8>(
                                        elem.asNumber()));
                            }
                        }
                    }
                }
            } else if (sub->name == "Anim") {
                // Sequence extent
                geo.sequenceExtents.push_back(parseExtent(*sub));
            }
        } else if (auto* prop = std::get_if<MdlProperty>(&child)) {
            if (prop->name == "VertexGroup") {
                // VertexGroup { 0, 0, ... } → parsed as property with array value
                if (!prop->values.empty() && prop->values[0].isArray()) {
                    for (auto& v : prop->values[0].asArray()) {
                        if (v.isNumber())
                            geo.vertexGroups.push_back(
                                static_cast<u8>(v.asNumber()));
                    }
                }
            } else if (prop->name == "MaterialID" && !prop->values.empty())
                geo.materialId = static_cast<u32>(prop->values[0].asNumber());
            else if (prop->name == "SelectionGroup" && !prop->values.empty())
                geo.selectionGroup =
                    static_cast<u32>(prop->values[0].asNumber());
            else if (prop->name == "SelectionFlags" && !prop->values.empty())
                geo.selectionFlags =
                    static_cast<u32>(prop->values[0].asNumber());
            else if (prop->name == "Unselectable")
                geo.selectionFlags = 4;
            else if (prop->name == "LevelOfDetail" && !prop->values.empty())
                geo.lod = static_cast<u32>(prop->values[0].asNumber());
            else if (prop->name == "LevelOfDetailName" && !prop->values.empty())
                geo.lodName = prop->values[0].asString();
            else if (prop->name == "MinimumExtent" || prop->name == "MaximumExtent" ||
                     prop->name == "BoundsRadius") {
                // Handled by parseExtent below
            }
        }
    }

    geo.extent = parseExtent(block);
    model.geosets.push_back(std::move(geo));
}

void convertGeosetAnim(const MdlNode& block, Model& model) {
    GeosetAnimation ga;
    ga.alpha = floatProp(block, "Alpha", 1.0f);
    ga.geosetId = u32Prop(block, "GeosetId");
    ga.flags = static_cast<GeosetAnimation::Flag>(u32Prop(block, "Flags"));

    // DropShadow flag
    if (hasFlag(block, "DropShadow")) ga.flags |= GeosetAnimation::Flag::DropShadow;

    // Color
    if (auto* p = findProp(block, "Color")) {
        if (p->isStatic && !p->values.empty() && p->values[0].isArray()) {
            ga.color = valueToVec3(p->values[0]);
            ga.flags |= GeosetAnimation::Flag::Color;
        }
    }

    ga.alphaTracks = getTrack<f32>(block, "Alpha");
    ga.colorTracks = getTrack<Vector3f>(block, "Color");
    if (ga.colorTracks.isUsed) ga.flags |= GeosetAnimation::Flag::Color;

    model.geosetAnimations.push_back(std::move(ga));
}

void convertBone(const MdlNode& block, Model& model) {
    Bone bone;
    bone.node = parseNodeFields(block, Node::NodeType::Bone);

    // GeosetId: can be "GeosetId Multiple" or "GeosetId N"
    if (auto* p = findProp(block, "GeosetId")) {
        if (!p->values.empty()) {
            if (p->values[0].isString() && p->values[0].asString() == "Multiple") {
                bone.geosetId = Bone::MULTIPLE_GEOSETS;
            } else if (p->values[0].isNumber()) {
                bone.geosetId = static_cast<u32>(p->values[0].asNumber());
            }
        }
    }

    // GeosetAnimId: can be "GeosetAnimId None" or "GeosetAnimId N"
    if (auto* p = findProp(block, "GeosetAnimId")) {
        if (!p->values.empty()) {
            if (p->values[0].isString() && p->values[0].asString() == "None") {
                bone.geosetAnimationId = 0xFFFFFFFF;
            } else if (p->values[0].isNumber()) {
                bone.geosetAnimationId =
                    static_cast<u32>(p->values[0].asNumber());
            }
        }
    }

    model.bones.push_back(std::move(bone));
}

void convertHelper(const MdlNode& block, Model& model) {
    Helper helper;
    helper.node = parseNodeFields(block, Node::NodeType::Helper);
    model.helpers.push_back(std::move(helper));
}

void convertLight(const MdlNode& block, Model& model) {
    Light light;
    light.node = parseNodeFields(block, Node::NodeType::Light);

    // Light type
    if (hasFlag(block, "Omnidirectional"))
        light.type = Light::LightType::Omni;
    else if (hasFlag(block, "Directional"))
        light.type = Light::LightType::Directional;
    else if (hasFlag(block, "Ambient"))
        light.type = Light::LightType::Ambient;

    light.attenuationStart = getFloatOrStatic(block, "AttenuationStart");
    light.attenuationEnd = getFloatOrStatic(block, "AttenuationEnd");
    light.intensity = getFloatOrStatic(block, "Intensity");
    light.ambientIntensity = getFloatOrStatic(block, "AmbIntensity");

    // Both colour fields default to white (1,1,1) — the value the war3.w3mod
    // game assets carry when the colour is shadowed by an animation track.
    light.color = vec3Prop(block, "Color", Vector3f(1, 1, 1));
    light.ambientColor = vec3Prop(block, "AmbColor", Vector3f(1, 1, 1));

    light.attenuationStartTracks = getTrack<f32>(block, "AttenuationStart");
    light.attenuationEndTracks = getTrack<f32>(block, "AttenuationEnd");
    light.colorTracks = getTrack<Vector3f>(block, "Color");
    light.intensityTracks = getTrack<f32>(block, "Intensity");
    light.ambientIntensityTracks = getTrack<f32>(block, "AmbIntensity");
    light.ambientColorTracks = getTrack<Vector3f>(block, "AmbColor");
    light.visibilityTracks = getTrack<f32>(block, "Visibility");

    model.lights.push_back(std::move(light));
}

void convertAttachment(const MdlNode& block, Model& model) {
    Attachment att;
    att.node = parseNodeFields(block, Node::NodeType::Attachment);
    att.attachmentId = u32Prop(block, "AttachmentID");
    att.path = stringProp(block, "Path");
    att.visibilityTracks = getTrack<f32>(block, "Visibility");
    model.attachments.push_back(std::move(att));
}

void convertPivotPoints(const MdlNode& block, Model& model) {
    for (auto& child : block.children) {
        if (auto* p = std::get_if<MdlProperty>(&child)) {
            if (!p->values.empty() && p->values[0].isArray()) {
                model.pivotPoints.push_back(valueToVec3(p->values[0]));
            }
        }
    }
}

void convertParticleEmitter(const MdlNode& block, Model& model) {
    ParticleEmitter pe;
    pe.node = parseNodeFields(block, Node::NodeType::ParticleEmitter);

    // MDL nests the per-particle fields (LifeSpan / InitVelocity / Path and
    // their tracks) inside a `Particle { ... }` sub-block; binary MDX stores
    // them inline on the emitter. Read those from the sub-block when present,
    // falling back to the emitter block so a flattened MDL still works.
    const MdlNode* particle = findBlock(block, "Particle");
    const MdlNode& pblock = particle ? *particle : block;

    pe.emissionRate = getFloatOrStatic(block, "EmissionRate");
    pe.gravity = getFloatOrStatic(block, "Gravity");
    pe.longitude = getFloatOrStatic(block, "Longitude");
    pe.latitude = getFloatOrStatic(block, "Latitude");
    pe.lifespan = getFloatOrStatic(pblock, "LifeSpan");
    pe.initialVelocity = getFloatOrStatic(pblock, "InitVelocity");
    pe.spawnModelFileName = stringProp(pblock, "Path");

    // Check EmitterUsesMdl / EmitterUsesTga flags (case-insensitive — the
    // canonical MDL spelling is the all-caps acronym EmitterUsesMDL/TGA).
    if (hasFlagCI(block, "EmitterUsesMdl"))
        pe.node.flags = pe.node.flags | Node::NodeFlag::EmitterUsesMdl;
    if (hasFlagCI(block, "EmitterUsesTga"))
        pe.node.flags = pe.node.flags | Node::NodeFlag::EmitterUsesTga;

    pe.emissionRateTracks = getTrack<f32>(block, "EmissionRate");
    pe.gravityTracks = getTrack<f32>(block, "Gravity");
    pe.longitudeTracks = getTrack<f32>(block, "Longitude");
    pe.latitudeTracks = getTrack<f32>(block, "Latitude");
    pe.lifespanTracks = getTrack<f32>(pblock, "LifeSpan");
    pe.speedTracks = getTrack<f32>(pblock, "InitVelocity");
    pe.visibilityTracks = getTrack<f32>(block, "Visibility");

    model.particleEmitters.push_back(std::move(pe));
}

void convertParticleEmitter2(const MdlNode& block, Model& model) {
    ParticleEmitter2 pe2;
    pe2.node = parseNodeFields(block, Node::NodeType::ParticleEmitter2);

    pe2.speed = getFloatOrStatic(block, "Speed");
    pe2.variation = getFloatOrStatic(block, "Variation");
    pe2.latitude = getFloatOrStatic(block, "Latitude");
    pe2.gravity = getFloatOrStatic(block, "Gravity");
    pe2.lifespan = getFloatOrStatic(block, "LifeSpan");
    pe2.emissionRate = getFloatOrStatic(block, "EmissionRate");
    pe2.length = getFloatOrStatic(block, "Length");
    pe2.width = getFloatOrStatic(block, "Width");

    // FilterMode as bare flag: "Blend,", "Additive,", etc.
    if (hasFlag(block, "Blend"))
        pe2.filterMode = 0;
    else if (hasFlag(block, "Additive"))
        pe2.filterMode = 1;
    else if (hasFlag(block, "Modulate"))
        pe2.filterMode = 2;
    else if (hasFlag(block, "Modulate2x"))
        pe2.filterMode = 3;
    else if (hasFlag(block, "AlphaKey"))
        pe2.filterMode = 4;

    pe2.rows = u32Prop(block, "Rows", 1);
    pe2.columns = u32Prop(block, "Columns", 1);

    // Head/Tail/Both flags
    if (hasFlag(block, "Head"))
        pe2.headOrTail = 0;
    else if (hasFlag(block, "Tail"))
        pe2.headOrTail = 1;
    else if (hasFlag(block, "Both"))
        pe2.headOrTail = 2;

    pe2.tailLength = floatProp(block, "TailLength", 1.0f);
    pe2.time = floatProp(block, "Time");

    pe2.textureId = u32Prop(block, "TextureID");
    pe2.squirt = u32Prop(block, "Squirt");
    pe2.priorityPlane = u32Prop(block, "PriorityPlane");
    pe2.replaceableId = u32Prop(block, "ReplaceableId");

    // ParticleEmitter2::segmentColor has no in-class initializer, so default
    // to neutral white — a model that omits SegmentColor entirely then reads
    // as full-bright instead of uninitialised garbage (which clamps to black).
    pe2.segmentColor = {Vector3f(1, 1, 1), Vector3f(1, 1, 1), Vector3f(1, 1, 1)};

    // SegmentColor { Color { r, g, b }, Color { r, g, b }, Color { r, g, b }, }
    // Each `Color { ... }` is an IDENT followed by a brace list of bare
    // numbers, which the parser classifies as an array-valued MdlProperty
    // (same shape as Alpha / ParticleScaling below) — NOT a sub-block. Read
    // them that way; the old MdlNode/headerParams path never matched, leaving
    // segmentColor uninitialised so every PE2 particle rendered black.
    if (auto* seg = findBlock(block, "SegmentColor")) {
        int colorIdx = 0;
        for (auto& sc : seg->children) {
            if (colorIdx >= 3)
                break;
            auto* cp = std::get_if<MdlProperty>(&sc);
            if (!cp || cp->name != "Color")
                continue;
            if (!cp->values.empty() && cp->values[0].isArray()) {
                auto& arr = cp->values[0].asArray();
                if (arr.size() >= 3) {
                    pe2.segmentColor[colorIdx] =
                        Vector3f(static_cast<f32>(arr[0].asNumber()),
                                 static_cast<f32>(arr[1].asNumber()),
                                 static_cast<f32>(arr[2].asNumber()));
                }
            }
            colorIdx++;
        }
    }

    // Alpha { a1, a2, a3 } - stored as a property with array value
    if (auto* p = findProp(block, "Alpha")) {
        if (!p->values.empty() && p->values[0].isArray()) {
            auto& arr = p->values[0].asArray();
            for (size_t i = 0; i < std::min(arr.size(), size_t(3)); ++i) {
                pe2.segmentAlpha[i] = static_cast<u8>(arr[i].asNumber());
            }
        }
    }

    // ParticleScaling { s1, s2, s3 }
    if (auto* p = findProp(block, "ParticleScaling")) {
        if (!p->values.empty() && p->values[0].isArray()) {
            auto& arr = p->values[0].asArray();
            for (size_t i = 0; i < std::min(arr.size(), size_t(3)); ++i) {
                pe2.segmentScaling[i] = static_cast<f32>(arr[i].asNumber());
            }
        }
    }

    // LifeSpanUVAnim { start, end, repeat }
    auto readInterval = [&](const char* name, std::array<u32, 3>& out) {
        if (auto* p = findProp(block, name)) {
            if (!p->values.empty() && p->values[0].isArray()) {
                auto& arr = p->values[0].asArray();
                for (size_t i = 0; i < std::min(arr.size(), size_t(3)); ++i) {
                    out[i] = static_cast<u32>(arr[i].asNumber());
                }
            }
        }
    };
    readInterval("LifeSpanUVAnim", pe2.headInterval);
    readInterval("DecayUVAnim", pe2.headDecayInterval);
    readInterval("TailUVAnim", pe2.tailInterval);
    readInterval("TailDecayUVAnim", pe2.tailDecayInterval);

    // Animation tracks
    pe2.speedTracks = getTrack<f32>(block, "Speed");
    pe2.variationTracks = getTrack<f32>(block, "Variation");
    pe2.latitudeTracks = getTrack<f32>(block, "Latitude");
    pe2.gravityTracks = getTrack<f32>(block, "Gravity");
    pe2.emissionRateTracks = getTrack<f32>(block, "EmissionRate");
    pe2.lengthTracks = getTrack<f32>(block, "Length");
    pe2.widthTracks = getTrack<f32>(block, "Width");
    pe2.visibilityTracks = getTrack<f32>(block, "Visibility");

    model.particleEmitters2.push_back(std::move(pe2));
}

void convertRibbonEmitter(const MdlNode& block, Model& model) {
    RibbonEmitter re;
    re.node = parseNodeFields(block, Node::NodeType::RibbonEmitter);

    re.heightAbove = getFloatOrStatic(block, "HeightAbove");
    re.heightBelow = getFloatOrStatic(block, "HeightBelow");
    re.alpha = getFloatOrStatic(block, "Alpha", 1.0f);
    re.lifespan = floatProp(block, "LifeSpan");
    re.textureSlot = u32Prop(block, "TextureSlot");
    re.emissionRate = u32Prop(block, "EmissionRate");
    re.rows = u32Prop(block, "Rows", 1);
    re.columns = u32Prop(block, "Columns", 1);
    re.materialId = u32Prop(block, "MaterialID");
    re.gravity = floatProp(block, "Gravity");

    // Color: "static Color { r, g, b },"
    if (auto* p = findProp(block, "Color")) {
        if (!p->values.empty() && p->values[0].isArray()) {
            re.color = valueToVec3(p->values[0]);
        }
    }

    re.heightAboveTracks = getTrack<f32>(block, "HeightAbove");
    re.heightBelowTracks = getTrack<f32>(block, "HeightBelow");
    re.alphaTracks = getTrack<f32>(block, "Alpha");
    re.colorTracks = getTrack<Vector3f>(block, "Color");
    re.textureSlotTracks = getTrack<u32>(block, "TextureSlot");
    re.visibilityTracks = getTrack<f32>(block, "Visibility");

    model.ribbonEmitters.push_back(std::move(re));
}

void convertEventObject(const MdlNode& block, Model& model) {
    EventObject ev;
    ev.node = parseNodeFields(block, Node::NodeType::EventObject);

    // EventTrack N { time, time, ... }
    if (auto* et = findTrack(block, "EventTrack")) {
        for (auto& kf : et->keyframes) {
            ev.eventTrackTimes.push_back(static_cast<u32>(kf.time));
        }
    }

    model.eventObjects.push_back(std::move(ev));
}

void convertCamera(const MdlNode& block, Model& model) {
    Camera cam;
    if (!block.headerParams.empty() && block.headerParams[0].isString()) {
        cam.name = block.headerParams[0].asString();
    }

    cam.position = vec3Prop(block, "Position");
    cam.fieldOfView = floatProp(block, "FieldOfView");
    cam.farClippingPlane = floatProp(block, "FarClip", 100.0f);
    cam.nearClippingPlane = floatProp(block, "NearClip", 0.1f);

    cam.positionTracks = getTrack<Vector3f>(block, "Translation");
    cam.targetRotationTracks = getTrack<f32>(block, "Rotation");

    // Target sub-block
    if (auto* target = findBlock(block, "Target")) {
        cam.targetPosition = vec3Prop(*target, "Position");
        cam.targetPositionTracks = getTrack<Vector3f>(*target, "Translation");
    }

    model.cameras.push_back(std::move(cam));
}

void convertCollisionShape(const MdlNode& block, Model& model) {
    CollisionShape cs;
    cs.node = parseNodeFields(block, Node::NodeType::CollisionShape);

    // Shape type as bare flag
    if (hasFlag(block, "Box"))
        cs.type = CollisionShape::ShapeType::Box;
    else if (hasFlag(block, "Sphere"))
        cs.type = CollisionShape::ShapeType::Sphere;
    else if (hasFlag(block, "Plane"))
        cs.type = CollisionShape::ShapeType::Plane;
    else if (hasFlag(block, "Cylinder"))
        cs.type = CollisionShape::ShapeType::Cylinder;

    cs.radius = floatProp(block, "BoundsRadius");

    // Vertices sub-block
    if (auto* verts = findBlock(block, "Vertices")) {
        for (auto& vc : verts->children) {
            if (auto* vp = std::get_if<MdlProperty>(&vc)) {
                if (!vp->values.empty() && vp->values[0].isArray()) {
                    cs.vertices.push_back(valueToVec3(vp->values[0]));
                }
            }
        }
    }

    model.collisionShapes.push_back(std::move(cs));
}

void convertFaceEffect(const MdlNode& block, Model& model) {
    FaceEffect fe;
    // FaceFX "Name" { Path "...", }
    if (!block.headerParams.empty() && block.headerParams[0].isString()) {
        fe.name = block.headerParams[0].asString();
    }
    fe.path = stringProp(block, "Path");
    model.faceEffects.push_back(std::move(fe));
}

void convertCornEmitter(const MdlNode& block, Model& model) {
    CornEmitter ce;
    ce.node = parseNodeFields(block, Node::NodeType::CornEmitter);

    // Static base value of each field. When an animated track shadows the
    // field, MDL carries only the track and the base value is absent — the
    // war3.w3mod PopcornFX game assets keep that shadowed base at 1.0 (not the
    // struct's 0.0), so fall back to 1.0 to keep the MDX round-trip exact.
    ce.lifeSpan = getFloatOrStatic(block, "LifeSpan", 1.0f);
    ce.emissionRate = getFloatOrStatic(block, "EmissionRate", 1.0f);
    ce.speed = getFloatOrStatic(block, "Speed", 1.0f);
    ce.alpha = getFloatOrStatic(block, "Alpha", 1.0f);
    ce.color = vec3Prop(block, "Color", Vector3f(1, 1, 1));
    ce.replaceableId = u32Prop(block, "ReplaceableId");
    ce.path = stringProp(block, "Path");
    ce.animVisibilityGuide = stringProp(block, "AnimVisibilityGuide");

    // Animation tracks. Color and Alpha are separate sections in MDL.
    ce.lifeSpanTracks     = getTrack<f32>(block, "LifeSpan");
    ce.emissionRateTracks = getTrack<f32>(block, "EmissionRate");
    ce.speedTracks        = getTrack<f32>(block, "Speed");
    ce.colorTracks        = getTrack<Vector3f>(block, "Color");
    ce.alphaTracks        = getTrack<f32>(block, "Alpha");
    ce.visibilityTracks   = getTrack<f32>(block, "Visibility");

    model.cornEmitters.push_back(std::move(ce));
}

void convertBindPose(const MdlNode& block, Model& model) {
    // BindPose { Matrices N { { 12 floats }, ... } }
    if (auto* matrices = findBlock(block, "Matrices")) {
        for (auto& mc : matrices->children) {
            if (auto* mp = std::get_if<MdlProperty>(&mc)) {
                if (!mp->values.empty() && mp->values[0].isArray()) {
                    auto& arr = mp->values[0].asArray();
                    if (arr.size() >= 12) {
                        std::array<f32, 12> mat{};
                        for (size_t i = 0; i < 12; ++i) {
                            mat[i] = static_cast<f32>(arr[i].asNumber());
                        }
                        model.bindPoses.push_back(mat);
                    }
                }
            }
        }
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

Model convertMdlToModel(std::string_view source, std::vector<std::string>& issues) {
    MdlDocument const doc = MdlParser::parse(source);

    // Forward parse errors
    for (auto& err : doc.errors) {
        issues.push_back("MDL parse error at line " + std::to_string(err.line) + ":" +
                         std::to_string(err.column) + ": " + err.message);
    }

    Model model;

    for (auto& root : doc.roots) {
        if (root.name == "Version") {
            convertVersion(root, model);
        } else if (root.name == "Model") {
            convertModel(root, model);
        } else if (root.name == "Sequences") {
            convertSequences(root, model);
        } else if (root.name == "GlobalSequences") {
            convertGlobalSequences(root, model);
        } else if (root.name == "Textures") {
            convertTextures(root, model);
        } else if (root.name == "Materials") {
            convertMaterials(root, model);
        } else if (root.name == "TextureAnims") {
            convertTextureAnims(root, model);
        } else if (root.name == "Geoset") {
            convertGeoset(root, model);
        } else if (root.name == "GeosetAnim") {
            convertGeosetAnim(root, model);
        } else if (root.name == "Bone") {
            convertBone(root, model);
        } else if (root.name == "Helper") {
            convertHelper(root, model);
        } else if (root.name == "Light") {
            convertLight(root, model);
        } else if (root.name == "Attachment") {
            convertAttachment(root, model);
        } else if (root.name == "PivotPoints") {
            convertPivotPoints(root, model);
        } else if (root.name == "ParticleEmitter") {
            convertParticleEmitter(root, model);
        } else if (root.name == "ParticleEmitter2") {
            convertParticleEmitter2(root, model);
        } else if (root.name == "ParticleEmitterPopcorn") {
            convertCornEmitter(root, model);
        } else if (root.name == "RibbonEmitter") {
            convertRibbonEmitter(root, model);
        } else if (root.name == "EventObject") {
            convertEventObject(root, model);
        } else if (root.name == "Camera") {
            convertCamera(root, model);
        } else if (root.name == "CollisionShape") {
            convertCollisionShape(root, model);
        } else if (root.name == "FaceFX") {
            convertFaceEffect(root, model);
        } else if (root.name == "BindPose") {
            convertBindPose(root, model);
        } else {
            issues.push_back("Unknown top-level block: " + root.name);
        }
    }

    return model;
}

} // namespace mdx
} // namespace whiteout
