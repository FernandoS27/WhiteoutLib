// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file wmo_converter.cpp
 * @brief `.wmo` -> `Document` (wmo_converter.h).
 *
 * ### The chain table is read off the uber shader, not the names
 *
 * `EvalMaterial` in the app's `wmo_uber.slang`, validated per shader against
 * 12.1's DXBC, is what each row transcribes. A WMO layer splits into what the
 * light multiplies (`base`) and what it adds after (`emis`); the combiner chain
 * has the same split, a seed with folds and an `AddAlpha` stage outside the
 * light. What no stage reads is said once per shader as `LossyKindConversion`:
 * a fold by the base map's alpha, an environment map added by it, and above all
 * a lerp by a vertex colour's alpha. That last one is decided per batch by the
 * mean over its vertices, so a wall that is mostly the second layer exports as
 * the second layer.
 */

#include "whiteout/models/wem/wmo_converter.h"

#include "whiteout/models/wem/geometry/builder.h"
#include "whiteout/models/wem/materials/features.h"
#include "whiteout/models/wow/wmo/runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {

namespace {

namespace wmo = wow::wmo;

constexpr f32 kByte = 1.0f / 255.0f;
constexpr u8 kNoMatrix = 0xFF;

/// Where a stage samples: a UV set, or the sphere map. `matrix` is the
/// material's texture matrix it reads through (MOUV layer 0 or 1), if any.
struct UvFrom {
    u8 set = 0;
    bool env = false;
    u8 matrix = kNoMatrix;
};

constexpr UvFrom kUv0{0, false, 0};
constexpr UvFrom kUv1{1, false, 1};
constexpr UvFrom kUv2{2, false, kNoMatrix};
constexpr UvFrom kEnv{0, true, kNoMatrix};

struct StageDesc {
    u8 slot = 0; ///< The MOMT texture slot.
    UvFrom uv = kUv0;
    CombinerOp rgb = CombinerOp::Pass;
    CombinerOp alpha = CombinerOp::Pass;
};

struct Chain {
    std::vector<StageDesc> stages;
    const char* lost = nullptr;
};

constexpr auto O = CombinerOp::Opaque;
constexpr auto P = CombinerOp::Pass;
constexpr auto M = CombinerOp::Mod;

/// One shader's chain. @p vertexA: the batch's vertices mostly carry MOCV set 1
/// alpha of 1 or more than half, which is the first layer's side of each lerp
/// by it. @p layer23: shader 23's heaviest MOC2 layer, 0..3.
Chain ChainFor(u32 shader, bool vertexA, u32 layer23) {
    const char* kVertexLerp = "a layer is lerped by MOCV set 1's alpha per vertex; the batch's mean "
                              "picked the side";
    const char* kEnvMask = "the environment map is added by the base map's alpha, which no stage reads";
    switch (shader) {
    case 0:
    case 16:
        return {{{0, kUv0, O, M}}};
    case 3:
    case 5:
        return {{{0, kUv0, O, P}, {1, kEnv, P, P}}, kEnvMask};
    case 6:
        return {{{0, kUv0, O, M}, {1, kUv1, vertexA ? P : CombinerOp::Fade, P}}, kVertexLerp};
    case 7:
        return {{{vertexA ? u8{0} : u8{1}, vertexA ? kUv0 : kUv1, O, P},
                 {vertexA ? u8{1} : u8{0}, vertexA ? kUv1 : kUv0, P, P},
                 {2, kEnv, P, P}},
                "two layers lerped by MOCV set 1's alpha per vertex, and an environment map "
                "added by the result's alpha"};
    case 8:
        // The second layer's UVs are the world's XY, which no input states.
        return {{{vertexA ? u8{0} : u8{1}, kUv0, O, vertexA ? M : P},
                 {vertexA ? u8{1} : u8{0}, kUv0, P, P}},
                "two layers lerped by MOCV set 1's alpha per vertex, the second planar-mapped"};
    case 9:
        return {{{0, kUv0, O, M}, {1, kUv1, vertexA ? CombinerOp::AddAlpha : P, P}},
                "the glow layer is scaled by MOCV set 1's alpha per vertex"};
    case 11:
        return {{{0, kUv0, O, P},
                 {1, kEnv, CombinerOp::MaskedMod2x, P},
                 {2, kUv1, vertexA ? CombinerOp::Fade : P, P}},
                "the third layer lerps in by its alpha times MOCV set 1's"};
    case 12:
        return {{{0, kUv0, O, P}, {1, kEnv, P, P}, {2, kUv1, vertexA ? CombinerOp::AddAlpha : P, P}},
                kEnvMask};
    case 13:
        return {{{vertexA ? u8{0} : u8{1}, vertexA ? kUv0 : kUv1, O, P},
                 {vertexA ? u8{1} : u8{0}, vertexA ? kUv1 : kUv0, P, P}},
                kVertexLerp};
    case 15:
        if (vertexA)
            return {{{0, kUv0, O, M}, {1, kUv1, P, P}}, kVertexLerp};
        return {{{1, kUv1, O, P}, {0, kUv0, P, M}}, kVertexLerp};
    case 17:
        return {{{0, kUv0, O, P},
                 {1, kEnv, CombinerOp::MaskedMod2x, P},
                 {2, kUv1, vertexA ? CombinerOp::AddAlpha : P, P}},
                "the third layer adds by its alpha times MOCV set 1's"};
    case 18:
        return {{{0, kUv0, O, M}, {1, kUv1, vertexA ? P : CombinerOp::Fade, P}, {2, kUv2, CombinerOp::Mod2x, P}},
                kVertexLerp};
    case 19:
        return {{{0, kUv0, O, M}, {1, kUv1, vertexA ? CombinerOp::Mod2x : P, P}}, kVertexLerp};
    case 20:
        return {{{0, kUv0, O, M}, {1, kUv1, CombinerOp::Fade, P}, {2, kUv2, CombinerOp::Mod2x, P}},
                "the third layer's alpha fades the second back out"};
    case 21:
        // The second layer is a light map in place of the vertex colour.
        return {{{0, kUv0, O, M}, {1, kUv0, CombinerOp::Mod2x, P}, {2, kUv0, CombinerOp::Add, P}}};
    case 22:
        if (vertexA)
            return {{{0, kUv0, O, P}, {2, kUv1, P, P}, {1, kEnv, P, P}},
                    "a parallax blend of four layers; the base layer stands in"};
        return {{{2, kUv1, O, P}, {0, kUv0, P, P}, {1, kEnv, P, P}},
                "a parallax blend of four layers; the third stands in"};
    case 23: {
        // Four layers, slots 1..4 on UV sets 0..3 as they stand, blended by
        // MOC2's weights and their height masks.
        Chain chain;
        chain.stages.push_back({static_cast<u8>(layer23 + 1), {static_cast<u8>(layer23), false, kNoMatrix}, O, P});
        for (u32 l = 0; l < 4; ++l) {
            if (l != layer23)
                chain.stages.push_back({static_cast<u8>(l + 1), {static_cast<u8>(l), false, kNoMatrix}, P, P});
        }
        chain.stages.push_back({0, kEnv, P, P});
        chain.lost = "four layers blended by MOC2's weights per vertex; the batch's heaviest stands in";
        return chain;
    }
    case 24:
        return {{{0, kUv0, O, P}, {1, kEnv, P, P}, {2, kUv0, P, M}}, kEnvMask};
    default:
        // 1 Specular, 2 Metal, 4 Opaque: 12.1 gives the first two no specular.
        return {{{0, kUv0, O, P}}};
    }
}

BlendMode BlendFor(u32 blendMode, bool& exact) {
    exact = true;
    switch (blendMode) {
    case 0:
        return BlendMode::Opaque;
    case 1:
        return BlendMode::AlphaKey;
    case 2:
        return BlendMode::AlphaBlend;
    case 3: // SRC_ALPHA, ONE
        return BlendMode::AdditiveAlpha;
    case 4:
        return BlendMode::Modulate;
    case 5:
        return BlendMode::Modulate2x;
    case 10: // ONE, ONE
    case 14:
        return BlendMode::Additive;
    case 13:
        return BlendMode::BlendAdd;
    case 6:  // DST_COLOR, ONE
    case 7:  // INV_SRC_ALPHA, ONE
    case 12: // Screen
        exact = false;
        return BlendMode::Additive;
    default:
        exact = false;
        return BlendMode::AlphaBlend;
    }
}

Vector3f Rgb(const wmo::Color& c) {
    return {static_cast<f32>(c.r) * kByte, static_cast<f32>(c.g) * kByte, static_cast<f32>(c.b) * kByte};
}

std::array<u8, 4> Rgba(const wmo::Color& c) {
    return {c.r, c.g, c.b, c.a};
}

i64 Packed(const wmo::Color& c) {
    return static_cast<i64>(static_cast<u32>(c.b) | (static_cast<u32>(c.g) << 8) |
                            (static_cast<u32>(c.r) << 16) | (static_cast<u32>(c.a) << 24));
}

/// The textures MOMT names, one entry per distinct file.
class TextureTable {
public:
    TextureTable(Document& document, const wmo::Root& root) : document_(document), root_(root) {}

    /// @p material's slot @p slot, or `kInvalidIndex` when it names nothing.
    u32 at(const wmo::Material& material, u32 slot) {
        if (!wmo::hasTexture(root_, material, slot))
            return kInvalidIndex;
        const u32 value = material.texture(slot);
        const auto key = std::make_pair(root_.textureNames.has_value(), value);
        if (const auto it = byValue_.find(key); it != byValue_.end())
            return it->second;
        TextureRef ref;
        if (root_.textureNames) {
            ref.path = std::string(root_.textureName(value));
            ref.key = TexturePath{ref.path};
        } else {
            ref.key = TextureFileDataId{value};
        }
        // Wrap both ways unless the first material to name it clamps (the
        // `.m2` and `.mdx` bits: 1 wraps S, 2 wraps T).
        ref.flags = (wmo::hasFlag(material.flags, wmo::MaterialFlag::ClampS) ? 0u : 1u) |
                    (wmo::hasFlag(material.flags, wmo::MaterialFlag::ClampT) ? 0u : 2u);
        const u32 index = static_cast<u32>(document_.textures.size());
        document_.textures.push_back(std::move(ref));
        byValue_.emplace(key, index);
        return index;
    }

private:
    Document& document_;
    const wmo::Root& root_;
    std::map<std::pair<bool, u32>, u32> byValue_;
};

/// The light MOCV gives a vertex of a vertex-lit batch: the interior ambient
/// plus twice the uploaded colour set 0 (`psWmo`'s light-mode-2 add, whose
/// load-time fix-up halved it and took the root's ambient out), lerped in by
/// the interior weight on a transition batch.
std::array<u8, 4> VertexLight(u32 uploaded, const wmo::Color& ambient, bool transition) {
    const auto channel = [&](u32 shift, u8 amb) {
        const f32 lit = std::min(2.0f * static_cast<f32>((uploaded >> shift) & 0xFFu) * kByte +
                                     static_cast<f32>(amb) * kByte,
                                 1.0f);
        const f32 w = transition ? 1.0f - static_cast<f32>(uploaded >> 24) * kByte : 1.0f;
        return static_cast<u8>(std::lround((1.0f - w + w * lit) * 255.0f));
    };
    return {channel(0, ambient.r), channel(8, ambient.g), channel(16, ambient.b), 255};
}

Node MakeNode(std::string name, NodeKind kind, const Transform& local) {
    Node node;
    node.name = std::move(name);
    node.kind = kind;
    node.resetPayloadForKind();
    node.parent = 0;
    node.pivot = local.translation;
    node.local = local;
    node.poses.push_back(node.local);
    return node;
}

void ImportLights(const wmo::Model& source, std::span<const u16> sets, NodeTree& tree) {
    std::set<u32> seenNew;
    u32 count = 0;
    for (u32 g = 0; g < source.groups.size(); ++g) {
        if (!source.groups[g])
            continue;
        for (const wmo::GatheredLight& light : wmo::gatherLights(source, g, sets)) {
            // MNLR entries of several groups can name one MNLD record.
            if (light.source == wmo::GatheredLight::Source::Mnld && !seenNew.insert(light.id).second)
                continue;
            // A spot's axis is 12.1's local +Z (R = Rz·Ry·Rx): the node turns +Z
            // onto it, row vectors like every WEM matrix.
            Matrix44f frame = Matrix44f::translation(light.position);
            if (light.spot) {
                const Vector3f d = wmo::lightDirection(light.rotation);
                const Vector3f up = std::abs(d.z) < 0.99f ? Vector3f{0, 0, 1} : Vector3f{1, 0, 0};
                const Vector3f x = cross(up, d).normalized();
                const Vector3f y = cross(d, x);
                for (u32 k = 0; k < 3; ++k) {
                    frame.data[0][k] = x.data[k];
                    frame.data[1][k] = y.data[k];
                    frame.data[2][k] = d.data[k];
                }
            }
            Node node = MakeNode("light_" + std::to_string(count++), NodeKind::Light, FromMatrix(frame));
            auto& payload = std::get<LightPayload>(node.payload);
            payload.kind = light.spot ? LightKind::Spot : LightKind::Omni;
            payload.color = Rgb(light.colorA);
            payload.intensity = light.intensity;
            payload.attenuationStart = light.attenuationStart;
            payload.attenuationEnd = light.attenuationEnd;
            payload.hotSpot = light.innerAngle;
            payload.falloff = light.outerAngle;
            node.native.set("wmoLightSource", static_cast<i64>(light.source));
            node.native.set("wmoLightId", static_cast<i64>(light.id));
            node.native.set("wmoLightFlags", static_cast<i64>(light.flags));
            tree.add(std::move(node));
        }
    }
}

void ImportDoodads(const wmo::Model& source, std::span<const u16> sets, NodeTree& tree) {
    const wmo::Root& root = source.root;
    for (const wmo::PlacedDoodad& placed : wmo::placedDoodads(source, sets)) {
        const wmo::DoodadDef& def = root.doodadDefs[placed.index];
        // The renderer's placement (`WmoDoodadTransform`): C44Matrix(C4Quaternion)
        // is the transpose of `Matrix44f::rotation`, row vectors, S·R·T.
        const Matrix44f placement = Matrix44f::scaling({def.scale, def.scale, def.scale}) *
                                    Matrix44f::rotation(def.rotation).transpose() *
                                    Matrix44f::translation(def.position);
        Node node = MakeNode("doodad_" + std::to_string(placed.index), NodeKind::Attachment,
                             FromMatrix(placement));
        const wmo::DoodadModel model = wmo::doodadModel(root, def);
        auto& payload = std::get<AttachmentPayload>(node.payload);
        payload.asset.id = model.fileId != 0 ? model.fileId : AssetKey::kNoId;
        payload.asset.path = std::string(model.path);
        node.native.set("wmoDoodad", static_cast<i64>(placed.index));
        node.native.set("wmoDoodadSet", static_cast<i64>(placed.set));
        node.native.set("wmoDoodadFlags", static_cast<i64>(def.flags()));
        node.native.set("wmoDoodadColor", Packed(def.color));
        tree.add(std::move(node));
    }
}

} // namespace

Result<Document> WmoConverter::fromWmo(const wow::wmo::Model& source, const WmoImportOptions& options) const {
    Result<Document> result;
    Diagnostics& diagnostics = result.diagnostics;
    const wmo::Root& root = source.root;
    const std::span<const u16> sets(options.doodadSets);

    Document document;
    document.space = CoordSpace::Blizzard;
    document.declare(ProfileId::Wow);
    document.defaultProfile = ProfileId::Wow;

    Extent bounds;
    bounds.minimum = root.header.bounds.minimum;
    bounds.maximum = root.header.bounds.maximum;
    const Vector3f half{(bounds.maximum.x - bounds.minimum.x) * 0.5f, (bounds.maximum.y - bounds.minimum.y) * 0.5f,
                        (bounds.maximum.z - bounds.minimum.z) * 0.5f};
    bounds.sphereRadius = std::sqrt(half.x * half.x + half.y * half.y + half.z * half.z);
    document.bounds = bounds;

    Model model;
    model.bounds = bounds;
    model.nodes.poseSchema.push_back(PoseSchema{});
    model.nodes.authoritativePose = 0;
    model.nodes.rig = RigConvention::PivotRelative;
    {
        Node rootNode = MakeNode("wmo_root", NodeKind::Bone, Transform::identity());
        rootNode.parent = kInvalidNode;
        model.nodes.add(std::move(rootNode));
    }

    ProfileMaterialSet set;
    set.profile = ProfileId::Wow;
    set.looks.looks.push_back(Look{});
    set.native.set("wmoRootFlags", static_cast<i64>(root.header.flags));
    set.native.set("wmoRootId", static_cast<i64>(root.header.wmoId));

    TextureTable textures(document, root);
    const wmo::Color ambient = wmo::ambientColors(root, sets)[0];
    std::set<u32> reported;

    for (u32 g = 0; g < source.groups.size(); ++g) {
        if (!source.groups[g])
            continue;
        const wmo::Group& group = *source.groups[g];
        if (!wmo::groupDraws(root, group) || group.positions.empty())
            continue;
        const std::size_t vertexCount = group.positions.size();
        const wmo::VertexColors uploaded = wmo::vertexColors(root, group);
        const std::vector<wmo::Color>* set0 = group.colorSet0();
        const std::vector<wmo::Color>* set1 = group.colorSet1();
        const bool interior = (group.header.flags & 0x48u) == 0;
        // Vertex-lit batches: every one of an interior group, and the
        // transition batches of any. The others in the mesh take white.
        const bool groupLit = set0 && (interior || group.header.transBatchCount != 0);

        geom::MeshBuilder builder;
        std::vector<u32> local(vertexCount, kInvalidIndex);
        const auto vertex = [&](u32 v) {
            if (local[v] == kInvalidIndex)
                local[v] = builder.addVertex(group.positions[v]).value();
            return geom::VertexId(local[v]);
        };
        const u32 uvSets = static_cast<u32>(std::min<std::size_t>(group.uvSets.size(), 4));
        if (set0)
            builder.declareAttr(geom::Domain::Vertex, "wmo.color0", geom::AttrType::U8x4);
        if (set1)
            builder.declareAttr(geom::Domain::Vertex, "wmo.color1", geom::AttrType::U8x4);
        if (group.vertexColors2)
            builder.declareAttr(geom::Domain::Vertex, "wmo.color2", geom::AttrType::U8x4);

        for (u32 b = 0; b < group.batches.size(); ++b) {
            const wmo::Batch& batch = group.batches[b];
            const u64 end = static_cast<u64>(batch.startIndex) + batch.indexCount;
            if (!wmo::batchDraws(root, batch) || batch.indexCount < 3 || end > group.indices.size())
                continue;
            const wmo::Material& record = root.materials[batch.material()];
            const u32 shader = wmo::effectiveShader(root, record);
            const bool transition = b < group.header.transBatchCount;
            const bool vertexLit = set0 && (interior || transition);

            // The vertex-alpha sides: MOCV set 1's mean alpha, and MOC2's
            // heaviest layer for shader 23.
            f64 alphaSum = 0.0;
            std::array<f64, 4> layerSum{};
            u32 counted = 0;
            for (u64 i = batch.startIndex; i < end; ++i) {
                const u32 v = group.indices[i];
                if (v >= vertexCount)
                    continue;
                alphaSum += static_cast<f64>(uploaded.color1[v] >> 24) * kByte;
                const u32 w = uploaded.color2[v];
                const f64 x = (w & 0xFFu) * kByte, y = ((w >> 8) & 0xFFu) * kByte,
                          z = ((w >> 16) & 0xFFu) * kByte;
                layerSum[0] += x;
                layerSum[1] += y;
                layerSum[2] += z;
                layerSum[3] += 1.0 - std::min(x + y + z, 1.0);
                ++counted;
            }
            const bool vertexA = counted == 0 || alphaSum / counted >= 0.5;
            const u32 layer23 = static_cast<u32>(std::max_element(layerSum.begin(), layerSum.end()) -
                                                 layerSum.begin());

            // --- the material ---------------------------------------------
            const std::string name = "group_" + std::to_string(g) + "_batch_" + std::to_string(b);
            const u32 slot = model.addSlot(name);
            Material material;
            material.name = name;
            CommonMaterial& common = material.InitCommon();
            bool exactBlend = true;
            common.blend = BlendFor(record.blendMode, exactBlend);
            if (!exactBlend && reported.insert(0x10000u | record.blendMode).second) {
                diagnostics.info(DiagCode::LossyBlendMode,
                                 "blend mode " + std::to_string(record.blendMode) + " has no WEM twin; " +
                                     ToString(common.blend) + " stands in",
                                 ElementRef(ElementKind::Slot, slot), ProfileId::Wow);
            }
            if (common.blend == BlendMode::AlphaKey)
                common.alphaTestThreshold = 128.0f / 255.0f;
            common.depth.write = record.blendMode <= 1;
            if (wmo::hasFlag(record.flags, wmo::MaterialFlag::TwoSided))
                common.cull = CullMode::None;
            if (wmo::hasFlag(record.flags, wmo::MaterialFlag::Unlit))
                common.flags |= MaterialFlags::Unlit;
            if (wmo::hasFlag(record.flags, wmo::MaterialFlag::Unfogged))
                common.flags |= MaterialFlags::Unfogged;

            const Chain chain = ChainFor(shader, vertexA, layer23);
            if (chain.lost && reported.insert(shader).second) {
                diagnostics.info(DiagCode::LossyKindConversion,
                                 "shader " + std::to_string(shader) + ": " + chain.lost,
                                 ElementRef(ElementKind::Slot, slot), ProfileId::Wow);
            }
            const bool clampS = wmo::hasFlag(record.flags, wmo::MaterialFlag::ClampS);
            const bool clampT = wmo::hasFlag(record.flags, wmo::MaterialFlag::ClampT);
            // MOUV replaces both texture matrices while it moves; shader 11's
            // second scrolls a tile every ten seconds without it.
            std::array<Vector2f, 2> scroll{Vector2f{0, 0}, Vector2f{0, 0}};
            if (batch.material() < root.uvAnimations.size()) {
                scroll[0] = root.uvAnimations[batch.material()].speed0;
                scroll[1] = root.uvAnimations[batch.material()].speed1;
            }
            if (shader == 11 && scroll[0].x == 0 && scroll[0].y == 0 && scroll[1].x == 0 && scroll[1].y == 0)
                scroll[1] = Vector2f{0.1f, 0.0f};

            CombinersBody body;
            for (const StageDesc& desc : chain.stages) {
                CombinerStage stage;
                stage.input.texture = textures.at(record, desc.slot);
                if (stage.input.texture == kInvalidIndex && !body.stages.empty())
                    continue; // a folded layer the material does not have
                stage.input.uvSet = std::min<u32>(desc.uv.set, uvSets ? uvSets - 1 : 0);
                stage.input.mapping = desc.uv.env ? UVMappingMode::EnvSphere : UVMappingMode::ExplicitUV;
                stage.input.wrapU = clampS ? WrapMode::Clamp : WrapMode::Repeat;
                stage.input.wrapV = clampT ? WrapMode::Clamp : WrapMode::Repeat;
                stage.rgb = desc.rgb;
                stage.alpha = desc.alpha;
                if (desc.uv.matrix != kNoMatrix) {
                    const Vector2f rate = scroll[desc.uv.matrix];
                    if (rate.x != 0 || rate.y != 0) {
                        MaterialFeature feature;
                        feature.id = static_cast<u32>(common.features.size());
                        feature.layer = static_cast<u32>(body.stages.size());
                        UvAnimationFeature uv;
                        uv.scrollRate = rate;
                        feature.payload = uv;
                        common.features.push_back(feature);
                    }
                }
                body.stages.push_back(std::move(stage));
            }
            common.body = std::move(body);
            set.resizeBindings(model.materialSlots.size());
            set.slotBindings[slot].byLook[0] = static_cast<u32>(set.materials.size());
            set.materials.push_back(std::move(material));

            // --- the section and its triangles ---------------------------
            MeshSection section;
            section.name = "batch_" + std::to_string(b);
            section.materialSlot = slot;
            section.rigidNode = 0;
            section.selectionGroup = static_cast<u16>(b);
            section.native.set("wmoGroup", static_cast<i64>(g));
            section.native.set("wmoBatch", static_cast<i64>(b));
            section.native.set("wmoMaterial", static_cast<i64>(batch.material()));
            section.native.set("wmoShader", static_cast<i64>(record.shader));
            section.native.set("wmoBlend", static_cast<i64>(record.blendMode));
            section.native.set("wmoMaterialFlags", static_cast<i64>(record.flags));
            section.native.set("wmoGroupFlags", static_cast<i64>(group.header.flags));
            if (transition)
                section.native.set("wmoTransition", 1);
            const u32 sectionIndex = builder.addSection(std::move(section));

            for (u64 i = batch.startIndex; i + 2 < end; i += 3) {
                const std::array<u32, 3> corners{group.indices[i], group.indices[i + 1], group.indices[i + 2]};
                if (corners[0] >= vertexCount || corners[1] >= vertexCount || corners[2] >= vertexCount) {
                    diagnostics.warn(DiagCode::IndexOutOfRange, "a batch index is past the group's vertices",
                                     ElementRef(ElementKind::Mesh, static_cast<u32>(model.meshes.size())),
                                     ProfileId::Wow);
                    continue;
                }
                const geom::FaceId face =
                    builder.addTriangle(vertex(corners[0]), vertex(corners[1]), vertex(corners[2]), sectionIndex);
                for (u32 c = 0; c < 3; ++c) {
                    const u32 v = corners[c];
                    builder.setCornerAttr(face, c, geom::names::kNormal,
                                          v < group.normals.size() ? group.normals[v] : Vector3f{0, 0, 1});
                    for (u32 s = 0; s < uvSets; ++s) {
                        if (v < group.uvSets[s].size())
                            builder.setCornerAttr(face, c, geom::names::uv(s), group.uvSets[s][v]);
                    }
                    if (groupLit) {
                        builder.setCornerAttr(face, c, geom::names::color(0),
                                              vertexLit ? VertexLight(uploaded.color0[v], ambient, transition)
                                                        : std::array<u8, 4>{255, 255, 255, 255});
                    }
                }
            }
        }
        if (builder.faceCount() == 0)
            continue;

        // The file's colour sets, raw, per vertex the mesh kept.
        for (u32 v = 0; v < vertexCount; ++v) {
            if (local[v] == kInvalidIndex)
                continue;
            const geom::VertexId id(local[v]);
            if (set0 && v < set0->size())
                builder.setVertexAttr(id, "wmo.color0", Rgba((*set0)[v]));
            if (set1 && v < set1->size())
                builder.setVertexAttr(id, "wmo.color1", Rgba((*set1)[v]));
            if (group.vertexColors2 && v < group.vertexColors2->size())
                builder.setVertexAttr(id, "wmo.color2", Rgba((*group.vertexColors2)[v]));
        }

        geom::MeshBuilder::BuildOutcome outcome = builder.build();
        const std::string groupName(root.groupName(g));
        outcome.mesh.name = groupName.empty() ? "group_" + std::to_string(g) : groupName;
        outcome.mesh.recomputeBounds();
        model.meshes.push_back(std::move(outcome.mesh));
    }
    set.resizeBindings(model.materialSlots.size());
    model.profileSets.push_back(std::move(set));

    ImportLights(source, sets, model.nodes);
    ImportDoodads(source, sets, model.nodes);

    // What 12.1 draws besides the batches, said once each.
    u32 liquids = 0;
    for (const auto& group : source.groups)
        liquids += group && wmo::hasLiquid(*group) ? 1u : 0u;
    const auto notCarried = [&](bool present, const std::string& what) {
        if (present)
            diagnostics.info(DiagCode::FeatureDropped, what + " not carried", ElementRef(), ProfileId::Wow);
    };
    notCarried(liquids != 0, std::to_string(liquids) + " liquid surface(s)");
    notCarried(!root.detailDoodads.empty(), "detail doodads (MDDL)");
    notCarried(!source.lodGroups.empty(), "LOD group files");
    notCarried(!root.portals.empty(), "portals");
    notCarried(!root.fogs.empty(), "fog (MFOG)");
    notCarried(!root.ambientVolumes.empty() || !root.globalAmbients.empty(), "ambient volumes");
    notCarried((root.skyboxFileId && *root.skyboxFileId != 0) || (root.skyboxName && !root.skyboxName->empty()),
               "the skybox");

    document.models.push_back(std::move(model));
    result.value = std::move(document);
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
