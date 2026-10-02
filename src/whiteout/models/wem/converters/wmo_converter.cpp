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
 * a blend per vertex. Which side of a lerp by MOCV set 1's alpha, or which of
 * shader 23's layers, is heavier is linear across a triangle, so the triangle
 * is cut where it changes and each piece takes its own. Deciding it per batch
 * drew a log's bark with its end-grain texture and a floor whose painted grime
 * outweighed the planks as all grime; per whole triangle, it lost the stone
 * nosings Zul'Aman's stairs paint into MOC2 row by row.
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
#include <tuple>
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

/// One shader's chain. @p vertexA: the piece's MOCV set 1 alpha is a half or
/// more, the first layer's side of each lerp by it. @p layer23: the shader-23
/// layer heavier over the piece, 0..3.
Chain ChainFor(u32 shader, bool vertexA, u32 layer23) {
    const char* kVertexLerp = "a layer is lerped by MOCV set 1's alpha per vertex; triangles are cut "
                              "where it crosses a half";
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
    case 23:
        // Slots 1..4 on UV sets 0..3 untransformed; the triangle's layer has
        // its set moved to 0 (`fromWmo`).
        return {{{static_cast<u8>(layer23 + 1), {0, false, kNoMatrix}, O, P}, {0, kEnv, P, P}},
                "four layers blended per pixel by MOC2's weights and their height masks; triangles are "
                "cut where the heaviest changes, each height taken as its mask's mean"};
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

/// The texture a MOMT slot value names: a MOTX name, or a FileDataID.
TextureRef TextureOf(const wmo::Root& root, u32 value) {
    TextureRef ref;
    if (root.textureNames) {
        ref.path = std::string(root.textureName(value));
        ref.key = TexturePath{ref.path};
    } else {
        ref.key = TextureFileDataId{value};
    }
    return ref;
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
        TextureRef ref = TextureOf(root_, value);
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

/// The mean heights shader 23 weighs its layers by, each texture read once. The
/// height maps are not document textures: no stage samples them.
class MeanHeights {
public:
    MeanHeights(const wmo::Root& root, const WmoImportOptions& options) : root_(root), read_(options.meanAlpha) {}

    /// @p material's slot @p slot, or 1: no reader, no texture, or unreadable.
    /// At least 0.004, the floor the shader gives a sampled height.
    f32 at(const wmo::Material& material, u32 slot) {
        if (!read_ || !wmo::hasTexture(root_, material, slot))
            return 1.0f;
        const u32 value = material.texture(slot);
        auto it = means_.find(value);
        if (it == means_.end())
            it = means_.emplace(value, read_(TextureOf(root_, value))).first;
        return it->second ? std::clamp(*it->second, 0.004f, 1.0f) : 1.0f;
    }

private:
    const wmo::Root& root_;
    const std::function<std::optional<f32>(const TextureRef&)>& read_;
    std::map<u32, std::optional<f32>> means_;
};

/// A point of a triangle being cut: its barycentrics, and for a point on an
/// edge (bit e: corners e and e + 1) the cut that made it, by the edge's file
/// vertices and the two variants it divides, so a neighbour cuts it the same.
struct CutPoint {
    std::array<f32, 3> at{};
    u8 edges = 0;
    std::array<u32, 2> edge{};
    u8 between = 0; ///< 1 + lower variant · 4 + higher; 0 for no edge cut.
};

std::vector<CutPoint> WholeTriangle() {
    return {{{1, 0, 0}, 0b101}, {{0, 1, 0}, 0b011}, {{0, 0, 1}, 0b110}};
}

/// The part of convex @p polygon, a piece of a triangle with corner scores @p s,
/// where variant @p keep scores at least as high as @p other; a tie goes to the
/// lower variant. A cut on an edge is taken from the edge's own corners, lower
/// file vertex first, so the triangle across it cuts at the same point.
std::vector<CutPoint> CutAway(const std::vector<CutPoint>& polygon, const std::array<u32, 3>& corners,
                              const std::array<std::array<f32, 4>, 3>& s, u8 keep, u8 other) {
    const auto d = [&](const CutPoint& p) {
        f32 sum = 0.0f;
        for (u32 c = 0; c < 3; ++c)
            sum += p.at[c] * (s[c][keep] - s[c][other]);
        return sum;
    };
    const auto inside = [&](f32 x) { return keep < other ? x >= 0.0f : x > 0.0f; };
    const u8 lo = std::min(keep, other);
    const u8 hi = std::max(keep, other);
    std::vector<CutPoint> out;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const CutPoint& p = polygon[i];
        const CutPoint& q = polygon[(i + 1) % polygon.size()];
        const f32 dp = d(p);
        const f32 dq = d(q);
        if (inside(dp))
            out.push_back(p);
        if (inside(dp) == inside(dq))
            continue;
        CutPoint cut;
        cut.edges = p.edges & q.edges;
        if (cut.edges != 0) {
            u32 a = (cut.edges & 1u) ? 0u : (cut.edges & 2u) ? 1u : 2u;
            u32 b = (a + 1) % 3;
            if (corners[b] < corners[a])
                std::swap(a, b);
            const f32 da = s[a][lo] - s[a][hi];
            const f32 db = s[b][lo] - s[b][hi];
            if (da != db) {
                const f32 t = std::clamp(da / (da - db), 0.0f, 1.0f);
                cut.at[a] = 1.0f - t;
                cut.at[b] = t;
                cut.edge = {corners[a], corners[b]};
                cut.between = static_cast<u8>(1 + lo * 4 + hi);
                out.push_back(cut);
                continue;
            }
        }
        const f32 t = dp / (dp - dq);
        for (u32 c = 0; c < 3; ++c)
            cut.at[c] = p.at[c] + t * (q.at[c] - p.at[c]);
        out.push_back(cut);
    }
    // A cut through a corner repeats the corner.
    std::vector<CutPoint> unique;
    for (const CutPoint& p : out) {
        if (unique.empty() || p.at != unique.back().at)
            unique.push_back(p);
    }
    if (unique.size() > 1 && unique.front().at == unique.back().at)
        unique.pop_back();
    return unique;
}

/// The shaders whose chain takes a side of a lerp by MOCV set 1's alpha.
bool LerpsByVertexAlpha(u32 shader) {
    switch (shader) {
    case 6:
    case 7:
    case 8:
    case 9:
    case 11:
    case 12:
    case 13:
    case 15:
    case 17:
    case 18:
    case 19:
    case 22:
        return true;
    default:
        return false;
    }
}

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
    MeanHeights heights(root, options);
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
        // The vertices cuts made: by batch, edge and the variants divided, and
        // where each lies between its triangle's corners.
        std::map<std::tuple<u32, u32, u32, u8>, u32> cutVertex;
        struct Cut {
            u32 id = 0;
            std::array<u32, 3> corners{};
            std::array<f32, 3> at{};
        };
        std::vector<Cut> cuts;
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

            // Which layer (shader 23) or side (a vertex-alpha lerp, 0 the first
            // layer's) is heavier, per vertex: shader 23 weighs MOC2's weights
            // by its layers' mean heights. It is linear across a triangle, so a
            // triangle where it changes is cut along that line and each piece
            // takes its own. The batch draws a section per one taken.
            const bool clampS = wmo::hasFlag(record.flags, wmo::MaterialFlag::ClampS);
            const bool clampT = wmo::hasFlag(record.flags, wmo::MaterialFlag::ClampT);
            const u8 candidates = shader == 23 ? 4 : LerpsByVertexAlpha(shader) ? 2 : 1;
            std::array<f32, 4> height{1.0f, 1.0f, 1.0f, 1.0f};
            std::array<bool, 4> drawn{true, true, true, true};
            if (shader == 23) {
                for (u32 l = 0; l < 4; ++l) {
                    height[l] = heights.at(record, 5 + l);
                    drawn[l] = wmo::hasTexture(root, record, 1 + l);
                }
            }
            const auto scoresOf = [&](u32 v) {
                std::array<f32, 4> score{-1.0f, -1.0f, -1.0f, -1.0f};
                if (shader == 23) {
                    const u32 w = uploaded.color2[v];
                    std::array<f32, 4> weight{};
                    for (u32 k = 0; k < 3; ++k)
                        weight[k] = static_cast<f32>((w >> (8 * k)) & 0xFFu) * kByte;
                    weight[3] = 1.0f - std::min(weight[0] + weight[1] + weight[2], 1.0f);
                    for (u32 l = 0; l < 4; ++l) {
                        if (drawn[l])
                            score[l] = weight[l] * height[l];
                    }
                } else if (candidates == 2) {
                    score[0] = static_cast<f32>(uploaded.color1[v] >> 24) * kByte;
                    score[1] = 1.0f - score[0];
                } else {
                    score[0] = 0.0f;
                }
                return score;
            };
            struct Piece {
                u8 variant = 0;
                std::array<u32, 3> corners{};
                std::vector<CutPoint> polygon;
            };
            std::vector<Piece> pieces;
            std::array<bool, 4> used{};
            for (u64 i = batch.startIndex; i + 2 < end; i += 3) {
                const std::array<u32, 3> corners{group.indices[i], group.indices[i + 1], group.indices[i + 2]};
                if (corners[0] >= vertexCount || corners[1] >= vertexCount || corners[2] >= vertexCount) {
                    diagnostics.warn(DiagCode::IndexOutOfRange, "a batch index is past the group's vertices",
                                     ElementRef(ElementKind::Mesh, static_cast<u32>(model.meshes.size())),
                                     ProfileId::Wow);
                    continue;
                }
                const std::array<std::array<f32, 4>, 3> score{scoresOf(corners[0]), scoresOf(corners[1]),
                                                             scoresOf(corners[2])};
                const auto heaviest = [&](u32 c) {
                    u8 best = 0;
                    for (u8 k = 1; k < candidates; ++k) {
                        if (score[c][k] > score[c][best])
                            best = k;
                    }
                    return best;
                };
                const u8 first = heaviest(0);
                if (heaviest(1) == first && heaviest(2) == first) {
                    pieces.push_back({first, corners, WholeTriangle()});
                    used[first] = true;
                    continue;
                }
                for (u8 variant = 0; variant < candidates; ++variant) {
                    std::vector<CutPoint> polygon = WholeTriangle();
                    for (u8 other = 0; other < candidates && polygon.size() >= 3; ++other) {
                        if (other != variant)
                            polygon = CutAway(polygon, corners, score, variant, other);
                    }
                    if (polygon.size() >= 3) {
                        pieces.push_back({variant, corners, std::move(polygon)});
                        used[variant] = true;
                    }
                }
            }
            const bool split = std::count(used.begin(), used.end(), true) > 1;

            for (u8 variant = 0; variant < 4; ++variant) {
                if (!used[variant])
                    continue;
                // Named for what split it, where something did.
                const std::string suffix =
                    !split ? std::string() : (shader == 23 ? "_layer" : "_side") + std::to_string(variant);

                // --- the material -----------------------------------------
                const std::string name = "group_" + std::to_string(g) + "_batch_" + std::to_string(b) + suffix;
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

                const Chain chain = ChainFor(shader, variant == 0, variant);
                if (chain.lost && reported.insert(shader).second) {
                    diagnostics.info(DiagCode::LossyKindConversion,
                                     "shader " + std::to_string(shader) + ": " + chain.lost,
                                     ElementRef(ElementKind::Slot, slot), ProfileId::Wow);
                }
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

                // --- the section and its triangles -----------------------
                MeshSection section;
                section.name = "batch_" + std::to_string(b) + suffix;
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

                // The corners carry the sets the chain reads. Shader 23's layer
                // reads its own, which crosses as set 0.
                u32 uvCount = 0;
                for (const StageDesc& desc : chain.stages)
                    uvCount = desc.uv.env ? uvCount : std::max<u32>(uvCount, desc.uv.set + 1u);
                uvCount = std::min(uvCount, uvSets);
                const u32 uvFirst = shader == 23 ? std::min<u32>(variant, uvSets ? uvSets - 1 : 0) : 0;

                // A piece's points: its triangle's own corners, or a vertex cut
                // there, one per edge cut so the triangle across shares it.
                const auto pointVertex = [&](const Piece& piece, const CutPoint& p) {
                    for (u32 c = 0; c < 3; ++c) {
                        if (p.at[c] == 1.0f)
                            return vertex(piece.corners[c]);
                    }
                    const auto key = std::make_tuple(b, p.edge[0], p.edge[1], p.between);
                    if (p.between != 0) {
                        if (const auto it = cutVertex.find(key); it != cutVertex.end())
                            return geom::VertexId(it->second);
                    }
                    Vector3f position{0, 0, 0};
                    for (u32 c = 0; c < 3; ++c) {
                        for (u32 k = 0; k < 3; ++k)
                            position.data[k] += p.at[c] * group.positions[piece.corners[c]].data[k];
                    }
                    const u32 id = builder.addVertex(position).value();
                    cuts.push_back({id, piece.corners, p.at});
                    if (p.between != 0)
                        cutVertex.emplace(key, id);
                    return geom::VertexId(id);
                };
                const auto setCorner = [&](geom::FaceId face, u32 c, const Piece& piece, const CutPoint& p) {
                    bool corner = false;
                    Vector3f normal{0, 0, 0};
                    std::array<Vector2f, 4> uv{};
                    std::array<f32, 4> light{};
                    for (u32 k = 0; k < 3; ++k) {
                        if (p.at[k] == 0.0f)
                            continue;
                        corner = corner || p.at[k] == 1.0f;
                        const u32 v = piece.corners[k];
                        const Vector3f n = v < group.normals.size() ? group.normals[v] : Vector3f{0, 0, 1};
                        for (u32 a = 0; a < 3; ++a)
                            normal.data[a] += p.at[k] * n.data[a];
                        for (u32 s = 0; s < uvCount; ++s) {
                            if (v < group.uvSets[uvFirst + s].size()) {
                                uv[s].x += p.at[k] * group.uvSets[uvFirst + s][v].x;
                                uv[s].y += p.at[k] * group.uvSets[uvFirst + s][v].y;
                            }
                        }
                        if (groupLit) {
                            const std::array<u8, 4> lit = vertexLit
                                                              ? VertexLight(uploaded.color0[v], ambient, transition)
                                                              : std::array<u8, 4>{255, 255, 255, 255};
                            for (u32 a = 0; a < 4; ++a)
                                light[a] += p.at[k] * static_cast<f32>(lit[a]);
                        }
                    }
                    if (!corner) {
                        const f32 length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                        if (length > 0.0f) {
                            for (u32 a = 0; a < 3; ++a)
                                normal.data[a] /= length;
                        }
                    }
                    builder.setCornerAttr(face, c, geom::names::kNormal, normal);
                    for (u32 s = 0; s < uvCount; ++s)
                        builder.setCornerAttr(face, c, geom::names::uv(s), uv[s]);
                    if (groupLit) {
                        std::array<u8, 4> rounded{};
                        for (u32 a = 0; a < 4; ++a)
                            rounded[a] = static_cast<u8>(std::lround(std::clamp(light[a], 0.0f, 255.0f)));
                        builder.setCornerAttr(face, c, geom::names::color(0), rounded);
                    }
                };
                for (const Piece& piece : pieces) {
                    if (piece.variant != variant)
                        continue;
                    // A fan: the pieces are convex.
                    for (std::size_t k = 1; k + 1 < piece.polygon.size(); ++k) {
                        const std::array<const CutPoint*, 3> at{&piece.polygon[0], &piece.polygon[k],
                                                                &piece.polygon[k + 1]};
                        const std::array<geom::VertexId, 3> ids{pointVertex(piece, *at[0]), pointVertex(piece, *at[1]),
                                                                pointVertex(piece, *at[2])};
                        if (ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2])
                            continue;
                        const geom::FaceId face = builder.addTriangle(ids[0], ids[1], ids[2], sectionIndex);
                        for (u32 c = 0; c < 3; ++c)
                            setCorner(face, c, piece, *at[c]);
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
        for (const Cut& cut : cuts) {
            const auto mix = [&](const std::vector<wmo::Color>& colors) {
                std::array<f32, 4> sum{};
                for (u32 c = 0; c < 3; ++c) {
                    if (cut.corners[c] < colors.size()) {
                        const std::array<u8, 4> rgba = Rgba(colors[cut.corners[c]]);
                        for (u32 a = 0; a < 4; ++a)
                            sum[a] += cut.at[c] * static_cast<f32>(rgba[a]);
                    }
                }
                std::array<u8, 4> out{};
                for (u32 a = 0; a < 4; ++a)
                    out[a] = static_cast<u8>(std::lround(std::clamp(sum[a], 0.0f, 255.0f)));
                return out;
            };
            const geom::VertexId id(cut.id);
            if (set0)
                builder.setVertexAttr(id, "wmo.color0", mix(*set0));
            if (set1)
                builder.setVertexAttr(id, "wmo.color1", mix(*set1));
            if (group.vertexColors2)
                builder.setVertexAttr(id, "wmo.color2", mix(*group.vertexColors2));
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
