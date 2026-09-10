// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file m3_converter.cpp
 * @brief `.m3` <-> `Document` (design §14, §10.7, §6.2).
 *
 * ### The profile is the version
 *
 * `MODL` v30 and up is Heroes of the Storm, where the loader turns every
 * material into a `DataDrivenMaterial` and `MADD` is the load-time truth; below
 * that is StarCraft II. Both are the same *format*, which is why one converter
 * serves two profiles and the caller may override the guess — content moved
 * between the two games exists, and a version number is a strong signal, not a
 * proof.
 *
 * ### The rebase is exact
 *
 * SC2 authors in Max's basis — `-Y` forward, `+X` left — and WEM's canonical
 * space is Blizzard's `+X` forward, `+Y` left. The change of basis is
 * `(x, y, z) -> (-y, x, z)`: an axis permutation with two sign flips,
 * determinant +1. That matters twice — it is bit-exact in floating point, so a
 * round trip loses nothing, and it is a rotation rather than a mirror, so
 * winding is preserved and no index reversal is needed.
 *
 * ### Two indirections, both easy to get backwards
 *
 * A region's face values are **region-local** — indices into that region's own
 * vertex slice, not into the division's buffer. And a vertex's bone index is an
 * offset into its region's `boneLookup` window, not a bone id. Reading either as
 * global produces a model that parses, builds, and is wrong.
 */

#include "whiteout/models/m3/engine_compat.h"
#include "whiteout/models/m3/parser.h"
#include "whiteout/models/m3/writer.h"
#include "whiteout/models/wem/converters.h"
#include "whiteout/models/wem/geometry/builder.h"
#include "whiteout/models/wem/geometry/render_view.h"

#include "../materials/m3_core.h"
#include "m3_anim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace whiteout {
namespace models {
namespace wem {

namespace {

constexpr ProfileId kM3Profiles[] = {ProfileId::Sc2, ProfileId::Heroes};

/// The first `MODL` version whose loader builds `MADD` (§7.2.6).
constexpr u32 kHeroesModlVersion = 30;

/// The animId every shipped model's `MSEC` bounds channel carries.
constexpr u32 kModelBoundsAnimId = 0x001F9BD2;

/// What MODL's attachment-point and camera addon arrays hold: 5,998 of 5,998
/// shipped attachment addons and every camera one measured is 0xFFFF.
constexpr u16 kNoAddon = 0xFFFFu;

/// A shipped `.m3` string carries its terminator inside the `std::string` — the
/// `Reference` count includes it — so anything comparing or storing one has to
/// drop it first.
std::string TrimNuls(std::string value) {
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

/// SC2's basis into WEM's canonical one. See the file comment.
Vector3f Rebase(const Vector3f& v) {
    return Vector3f{-v.y, v.x, v.z};
}

Vector3f Unrebase(const Vector3f& v) {
    return Vector3f{v.y, -v.x, v.z};
}

/// The same change of basis as a matrix, so a whole transform can be conjugated
/// by it rather than taken apart first: `v * kRebaseBasis == Rebase(v)`.
Matrix44f RebaseBasis() {
    Matrix44f r = Matrix44f::identity();
    r.data[0][0] = 0.0f;
    r.data[0][1] = 1.0f;
    r.data[1][0] = -1.0f;
    r.data[1][1] = 0.0f;
    return r;
}

/// A model-space matrix from SC2's basis into WEM's. A change of basis is a
/// conjugation, and this is the direction that agrees with `Rebase` on the
/// translation, on the rotation, **and** on the scale — which the per-component
/// route does not, since a 90 degree turn about Z swaps a non-uniform x and y.
Matrix44f RebaseMatrix(const Matrix44f& m) {
    const Matrix44f r = RebaseBasis();
    return r.transpose() * m * r;
}

Matrix44f UnrebaseMatrix(const Matrix44f& m) {
    const Matrix44f r = RebaseBasis();
    return r * m * r.transpose();
}

Extent ToExtent(const m3::Extent& source) {
    Extent out;
    const Vector3f a = Rebase(source.min);
    const Vector3f b = Rebase(source.max);
    // The rebase is a rotation, so a corner can swap sides; min/max are
    // recomputed rather than mapped.
    out.minimum = Vector3f{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    out.maximum = Vector3f{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    out.sphereRadius = source.radius;
    return out;
}

m3::Extent FromExtent(const Extent& source) {
    m3::Extent out;
    const Vector3f a = Unrebase(source.minimum);
    const Vector3f b = Unrebase(source.maximum);
    out.min = Vector3f{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    out.max = Vector3f{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    out.radius = source.sphereRadius;
    if (out.radius <= 0.0f) {
        // A source that states no radius still needs one here: StarCraft II's
        // is the sphere around the box, not around the origin. Measured on 377
        // shipped bounds, every one is |(max - min) / 2| to within a thousandth
        // (the 18 that read zero have a zero box).
        const Vector3f half{(out.max.x - out.min.x) * 0.5f, (out.max.y - out.min.y) * 0.5f,
                            (out.max.z - out.min.z) * 0.5f};
        out.radius = std::sqrt(half.x * half.x + half.y * half.y + half.z * half.z);
    }
    return out;
}

/// `BONE.flags` carries nothing `NodeFlags` can say.
///
/// It looks as though it does: `InheritTranslation`, `InheritScale` and
/// `InheritRotation` read as the positive spelling of MDX's three opt-outs. But
/// StarCraft II never asks. The evaluator composes every chain unconditionally
/// and the only bits it reads out of this word are the two billboard ones,
/// which are themselves dead (`m3_model_adapter.cpp`; billboards come from
/// `BBSC`). And the three are set on **no bone at all** — 0 of 50,771 across
/// 4,000 corpus `.m3` files — so mapping their absence hands every bone of
/// every `.m3` in existence all three suppressions, in a document a
/// pivot-relative target *does* honour. That detached every bone from its
/// parent the moment a `.m3` was opened as Warcraft III.
///
/// So the word stays native: `m3FlagBits` round-trips it verbatim and no
/// `NodeFlags` bit is derived from it.
NodeFlags ToNodeFlags(m3::BoneFlag) {
    return NodeFlags::None;
}

m3::BoneFlag FromNodeFlags(NodeFlags, u32 rawFallback) {
    return static_cast<m3::BoneFlag>(rawFallback);
}

std::string SlotName(std::size_t materialMapIndex) {
    return "material_" + std::to_string(materialMapIndex);
}

/// One vertex of the `.m3` blob, as the parser reads it back.
///
/// The layout is not declared anywhere in the file — it is derived from
/// `MODL.vertexFlags` and hard-coded in `m3::VertexBuffer::initialize`, so this
/// is that function read backwards. Position f32x3 at 0, weights and indices as
/// raw bytes at 12 and 16, the normal as UNORM bytes at 20 with the tangent's
/// handedness in its fourth, then one i16 pair per UV layer at 1/2048 units, and
/// the tangent last in the same UNORM encoding.
struct M3VertexEncoder {
    static constexpr std::size_t kBaseSize = 24;
    static constexpr f32 kUvScale = 2048.0f;
    static constexpr std::size_t kMaxUvSets = 5;

    std::vector<u8> data;
    std::size_t stride = 0;
    std::size_t uvCount = 1;
    bool hasColor = false;

    M3VertexEncoder(std::size_t uvSets, bool colored) : uvCount(uvSets), hasColor(colored) {
        stride = kBaseSize + (hasColor ? 4 : 0) + uvCount * 4 + 4;
    }

    static u8 EncodeUnorm(f32 value) {
        const f32 mapped = (std::clamp(value, -1.0f, 1.0f) * 0.5f + 0.5f) * 255.0f;
        return static_cast<u8>(mapped + 0.5f);
    }

    void push(const Vector3f& position, const Vector3f& normal, const Vector4f& tangent,
              const std::array<Vector2f, kMaxUvSets>& uvs, const std::array<u8, 4>& color,
              const std::array<u8, 4>& boneIndices, const std::array<u8, 4>& boneWeights) {
        const std::size_t base = data.size();
        data.resize(base + stride, 0);
        u8* out = data.data() + base;
        std::memcpy(out + 0, &position, sizeof(Vector3f));
        std::memcpy(out + 12, boneWeights.data(), 4);
        std::memcpy(out + 16, boneIndices.data(), 4);
        out[20] = EncodeUnorm(normal.x);
        out[21] = EncodeUnorm(normal.y);
        out[22] = EncodeUnorm(normal.z);
        // The bitangent's handedness rides the normal's fourth byte, not the
        // tangent's -- the tangent's own is unused and shipped as 255.
        out[23] = tangent.w < 0.0f ? 0 : 255;
        std::size_t at = kBaseSize;
        if (hasColor) {
            out[at + 0] = color[2]; // The blob is BGRA; the layer is RGBA.
            out[at + 1] = color[1];
            out[at + 2] = color[0];
            out[at + 3] = color[3];
            at += 4;
        }
        for (std::size_t set = 0; set < uvCount; ++set) {
            const i16 u =
                static_cast<i16>(std::clamp(uvs[set].x * kUvScale, -32768.0f, 32767.0f));
            const i16 v =
                static_cast<i16>(std::clamp(uvs[set].y * kUvScale, -32768.0f, 32767.0f));
            std::memcpy(out + at, &u, 2);
            std::memcpy(out + at + 2, &v, 2);
            at += 4;
        }
        out[stride - 4] = EncodeUnorm(tangent.x);
        out[stride - 3] = EncodeUnorm(tangent.y);
        out[stride - 2] = EncodeUnorm(tangent.z);
        out[stride - 1] = 255;
    }
};

/// The multiply/add that turns a raw i16 UV unit into a texture coordinate.
///
/// A REGN v5 states the pair itself, in SNORM space -- so the stock 16.0 is
/// `16 / 32767`, which is the same `1 / 2048` every older region implies and
/// what makes the flat divide look right on most models. It is not right on all
/// of them: shipped Heroes regions carry scales from 0.26 to 17, and reading one
/// at 1/2048 puts a body authored for [0,1] anywhere in [-16, 16]. The renderer
/// has always done this (`m3_surface_table.cpp`); the converter had not, and the
/// export only looked correct because a clamped sampler hid it.
Vector2f UvDecodeFor(const m3::Region& region) {
    if (region.getVersion() >= 5 && region.uvScale > 0.0f) {
        return Vector2f{region.uvScale / 32767.0f, region.uvOffset};
    }
    return Vector2f{1.0f / 2048.0f, 0.0f};
}

/// What a bone's visibility rests at, which is the only thing that decides
/// whether a gated batch draws in a model with nothing keying it.
///
/// A bone's position, rotation and scale rest in `Node::local`, so the export
/// reads them from there. Visibility has no such home -- it is an M3 property
/// with no node field of its own, and the import parks its rest in the channel
/// it declares for it. A written `.m3` therefore left `visibility.initValue` at
/// the struct's zero, which reads as "invisible": Alexstrasza, whose four
/// batches all gate on `Vis_Alexstrasza` and whose `.m3` has no sequence at all,
/// round-tripped into a model that draws nothing.
///
/// Visible is the answer when the document says nothing. Every source but M3
/// gates no batch, so the value is never read there; within M3 the file's own
/// AnimRef default is the rest, and `restValue` put it in the channel.
f32 VisibilityRest(const Model& model, u32 node) {
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind != TrackTarget::Kind::Node || channel.target.node != node ||
            channel.target.channel != Channel::Visibility || !channel.hasInitValue()) {
            continue;
        }
        f32 rest = 1.0f;
        std::memcpy(&rest, channel.initValue.data(), sizeof(f32));
        return rest;
    }
    return 1.0f;
}

NodeTree ImportNodes(const m3::Model& source) {
    NodeTree tree;

    // `IREF` is the one pose M3 carries: the inverse model-space bind pose, one
    // matrix per bone. Stored AS a matrix, because 23 of 250 corpus models hold
    // shear in it that no TRS reproduces -- see `PoseStorage`.
    PoseSchema schema;
    schema.name = "iref";
    schema.space = PoseSpace::Model;
    schema.inverse = true;
    schema.storage = PoseStorage::Matrix;
    tree.poseSchema.push_back(schema);
    tree.authoritativePose = 0;
    tree.rig = RigConvention::ExplicitBind;

    for (std::size_t b = 0; b < source.bones.size(); ++b) {
        const m3::Bone& bone = source.bones[b];
        Node node;
        node.name = TrimNuls(bone.name);
        node.kind = NodeKind::Bone;
        node.resetPayloadForKind();
        node.flags = ToNodeFlags(bone.flags);
        node.parent = bone.parentIndex == 0xFFFFu ? kInvalidNode : bone.parentIndex;
        node.native.set("m3FlagBits", static_cast<i64>(static_cast<u32>(bone.flags)));

        // The bone's own rest transform, in the source's own parent-relative
        // terms -- rebased, because a translation is a vector in the basis
        // being changed.
        node.local.translation = Rebase(bone.position.initValue);
        const Quaternion& rotation = bone.rotation.initValue;
        // The same change of basis applied to a quaternion's vector part; the
        // scalar is invariant under a rotation of the frame.
        node.local.rotation = Quaternion{-rotation.y, rotation.x, rotation.z, rotation.w};
        node.local.scale = bone.scale.initValue;

        // The pose is IREF itself, not the rest transform: the rest chain does
        // not compose to the bind pose (up to 2.2 units apart on `Marine.m3`),
        // so it cannot stand in for one. A bone past the end of IREF gets
        // identity, which is what the renderer would have bound anyway.
        const Matrix44f iref = b < source.initialReference.size()
                                   ? RebaseMatrix(source.initialReference[b].matrix)
                                   : Matrix44f::identity();
        node.poseMatrices.push_back(iref);
        // Derived, and never read back: it is what a TRS-only consumer gets.
        node.poses.push_back(FromMatrix(iref));
        tree.add(std::move(node));
    }

    const u32 boneCount = tree.size();
    for (const m3::AttachmentPoint& point : source.attachmentPoints) {
        Node node;
        node.name = TrimNuls(point.name);
        node.kind = NodeKind::Attachment;
        node.resetPayloadForKind();
        node.parent = point.boneIndex < boneCount ? point.boneIndex : kInvalidNode;
        tree.add(std::move(node));
    }

    for (std::size_t l = 0; l < source.lights.size(); ++l) {
        const m3::Light& light = source.lights[l];
        Node node;
        node.name = "light_" + std::to_string(l);
        node.kind = NodeKind::Light;
        node.resetPayloadForKind();
        node.parent = light.boneIndex < boneCount ? light.boneIndex : kInvalidNode;
        auto& payload = std::get<LightPayload>(node.payload);
        switch (light.lightType) {
        case m3::LightType::Spot:
            payload.kind = LightKind::Spot;
            break;
        case m3::LightType::Directional:
            payload.kind = LightKind::Directional;
            break;
        default:
            payload.kind = LightKind::Omni;
            break;
        }
        payload.color = light.diffuseColor.initValue;
        payload.intensity = light.intensityMultiplier.initValue;
        payload.attenuationStart = light.attenuationStart.initValue;
        payload.attenuationEnd = light.attenuationEnd;
        payload.hotSpot = light.hotSpot.initValue;
        payload.falloff = light.falloff.initValue;
        node.native.set("m3LightFlags", static_cast<i64>(static_cast<u32>(light.flags)));
        tree.add(std::move(node));
    }

    for (const m3::Camera& camera : source.cameras) {
        Node node;
        node.name = TrimNuls(camera.name);
        node.kind = NodeKind::Camera;
        node.resetPayloadForKind();
        node.parent = camera.boneIndex < boneCount ? camera.boneIndex : kInvalidNode;
        auto& payload = std::get<CameraPayload>(node.payload);
        payload.fov = camera.fieldOfView.initValue;
        payload.nearClip = camera.nearClip.initValue;
        payload.farClip = camera.farClip.initValue;
        tree.add(std::move(node));
    }

    for (std::size_t p = 0; p < source.particleEmitters.size(); ++p) {
        Node node;
        node.name = "particle_" + std::to_string(p);
        node.kind = NodeKind::ParticleEmitter;
        node.resetPayloadForKind();
        std::get<ParticlePayload>(node.payload).system.id = static_cast<u32>(p);
        tree.add(std::move(node));
    }

    for (std::size_t r = 0; r < source.ribbonEmitters.size(); ++r) {
        Node node;
        node.name = "ribbon_" + std::to_string(r);
        node.kind = NodeKind::RibbonEmitter;
        node.resetPayloadForKind();
        std::get<RibbonPayload>(node.payload).system.id = static_cast<u32>(r);
        tree.add(std::move(node));
    }

    return tree;
}

} // namespace

ProfileId M3Converter::ProfileForVersion(u32 modelVersion) {
    return modelVersion >= kHeroesModlVersion ? ProfileId::Heroes : ProfileId::Sc2;
}

// ============================================================================
// fromM3
// ============================================================================

Result<Document> M3Converter::fromM3(const m3::Model& source, ProfileId profileOverride) const {
    Result<Document> result;
    Diagnostics& diagnostics = result.diagnostics;

    const u32 modelVersion =
        source.getVersion() < 0 ? kHeroesModlVersion : static_cast<u32>(source.getVersion());
    const ProfileId profile =
        profileOverride == ProfileId::Count ? ProfileForVersion(modelVersion) : profileOverride;

    Document document;
    document.name = TrimNuls(source.name);
    document.bounds = ToExtent(source.bounds);
    document.space = CoordSpace::Blizzard;
    document.declare(profile);
    document.defaultProfile = profile;

    Model model;
    model.name = document.name;
    model.bounds = document.bounds;
    model.nodes = ImportNodes(source);

    // --- materials ----------------------------------------------------------
    //
    // A region names a `MaterialMap` entry, so the slot list is index-aligned
    // with `materialMaps` and a batch's `materialIndex` is the slot directly.
    m3_core::Context context;
    context.modelVersion = modelVersion;
    context.internUnknownPaths = true;

    ProfileMaterialSet set;
    set.profile = profile;
    set.looks.looks.push_back(Look{});
    // The `MODL` tail is profile-scoped (§6.3) -- it describes how this game
    // reads the model, not the geometry both games would share.
    set.native.set("modelVersion", static_cast<i64>(modelVersion));
    set.native.set("modelFlags", static_cast<i64>(static_cast<u32>(source.flags)));
    // The vertex declaration, kept whole. Only its UV and colour bits describe
    // the layout this reader understands; the rest tell the engine how to bind
    // the buffer, and an export that rebuilt the word from what it could name
    // would quietly re-declare every model as the one shape it knows.
    set.native.set("vertexFlags", static_cast<i64>(static_cast<u32>(source.vertices.flags)));
    for (std::size_t m = 0; m < source.materialMaps.size(); ++m) {
        model.materialSlots.push_back(SlotName(m));
    }
    set.resizeBindings(model.materialSlots.size());
    std::vector<std::vector<u32>> layerOrdinals(source.materialMaps.size());
    for (std::size_t m = 0; m < source.materialMaps.size(); ++m) {
        Material material = m3_core::ImportMaterial(source, source.materialMaps[m], profile,
                                                    context, diagnostics, &layerOrdinals[m]);
        material.name = TrimNuls(material.name);
        if (material.name.empty()) {
            material.name = SlotName(m);
        }
        set.slotBindings[m].byLook[0] = static_cast<u32>(set.materials.size());
        set.materials.push_back(std::move(material));
    }

    // The texture table is whatever the materials interned, in first-use order.
    for (const auto& [path, index] : context.texturesByPath) {
        if (index != document.textures.size()) {
            diagnostics.warn(DiagCode::TextureUnresolved,
                             "texture table went out of order interning " + path);
        }
        TextureRef ref;
        ref.path = path;
        ref.key = TexturePath{path};
        document.textures.push_back(std::move(ref));
    }

    // --- geometry -----------------------------------------------------------
    const std::vector<Vector3f> positions = source.vertices.getPositions();
    const std::vector<Vector3f> normals = source.vertices.getNormals();
    // The `.m3` tangent is three UNORM bytes at the end of the vertex; its `w`
    // is the bitangent handedness, which lives in the normal's fourth byte.
    // Nothing recomputes it as well as the file states it -- a rebuilt tangent
    // frame disagrees with the one the artist's normal map was baked against.
    const std::vector<Vector4f> tangents = source.vertices.getTangents();
    const std::vector<m3::ColorBGRA> colors = source.vertices.getColors();
    const std::size_t uvCount = source.vertices.UVsNum();
    // RAW i16 units, because the coordinate is not a property of the blob: a
    // REGN v5 states its own `uvScale`/`uvOffset` and only a stock one carries
    // 16 and 0 -- the pair that reproduces the flat `raw / 2048` older regions
    // imply. Alexstrasza's four regions carry 0.92/1.07, 0.26/0.26, 0.94/0.94
    // and 0.32/0.34, so decoding her at 1/2048 spreads a body meant for [0,1]
    // across the whole +-16 range. `UvDecodeFor` does the per-region half.
    std::vector<std::vector<Vector2f>> uvSets;
    for (std::size_t u = 0; u < uvCount; ++u) {
        uvSets.push_back(source.vertices.getUVs(u, 1.0f, 0.0f));
    }
    // Both come back as raw bytes: an index is region-local and a weight is
    // 0..255, so the divide is the converter's to do.
    const std::vector<std::array<u8, 4>> boneIndices = source.vertices.getBoneIndices();
    const std::vector<std::array<u8, 4>> boneWeights = source.vertices.getBoneWeights();

    for (std::size_t d = 0; d < source.divisions.size(); ++d) {
        const m3::MeshDivision& division = source.divisions[d];

        // The batch that draws each region, and therefore the material its
        // section binds. A region no batch names is imported and marked
        // undrawn rather than dropped.
        std::vector<u32> batchOfRegion(division.regions.size(), kInvalidIndex);
        for (std::size_t b = 0; b < division.batches.size(); ++b) {
            const m3::Batch& batch = division.batches[b];
            if (batch.regionIndex < batchOfRegion.size() &&
                batchOfRegion[batch.regionIndex] == kInvalidIndex) {
                batchOfRegion[batch.regionIndex] = static_cast<u32>(b);
            }
        }

        geom::MeshBuilder builder;
        // A division's regions select ranges of the global vertex buffer;
        // rebasing to the lowest one makes the mesh self-contained.
        u32 lowest = 0xFFFFFFFFu;
        u32 highest = 0;
        for (const m3::Region& region : division.regions) {
            lowest = std::min(lowest, region.firstVertex);
            highest = std::max(highest, region.firstVertex + region.vertexCount);
        }
        if (lowest > highest) {
            lowest = 0;
            highest = 0;
        }

        for (u32 v = lowest; v < highest; ++v) {
            builder.addVertex(v < positions.size() ? Rebase(positions[v]) : Vector3f{0, 0, 0});
        }

        std::vector<u32> sectionOfRegion(division.regions.size(), 0);
        for (std::size_t r = 0; r < division.regions.size(); ++r) {
            const m3::Region& region = division.regions[r];
            MeshSection section;
            section.name = "region_" + std::to_string(r);
            section.native.set("rootBone", static_cast<i64>(region.rootBone));
            section.native.set("regionFlags", static_cast<i64>(static_cast<u32>(region.flags)));
            // `Hidden` never appears alone. Measured over 111,743 shipped
            // regions — 36,712 Heroes and 75,031 StarCraft II — it is 189 + 189
            // and 27 + 27, in pairs: one `Hidden|ClothSimulated` beside one
            // `Hidden|Placeholder|ClothInfluenced`. The first is the simulation
            // cage and is not drawn; the second IS the cape, and reading its
            // `Hidden` bit as "do not draw" exported Alexstrasza's cloak and
            // Artanis's cloth as nothing at all.
            if (hasFlag(region.flags, m3::RegionFlag::ClothSimulated)) {
                section.flags |= SectionFlags::ClothSimulated;
            }
            if (hasFlag(region.flags, m3::RegionFlag::ClothInfluenced)) {
                section.flags |= SectionFlags::ClothInfluenced;
            }
            if (hasFlag(region.flags, m3::RegionFlag::Hidden) &&
                !hasFlag(region.flags, m3::RegionFlag::ClothInfluenced)) {
                section.flags |= SectionFlags::Hidden;
            }
            const u32 batch = batchOfRegion[r];
            if (batch == kInvalidIndex) {
                section.profiles = kNoProfiles;
                diagnostics.info(DiagCode::SectionUndrawn,
                                 "region " + std::to_string(r) + " has no batch",
                                 ElementRef(ElementKind::Section, r), profile);
            } else {
                const m3::Batch& record = division.batches[batch];
                if (record.materialIndex < model.materialSlots.size()) {
                    section.materialSlot = record.materialIndex;
                } else {
                    diagnostics.warn(DiagCode::IndexOutOfRange,
                                     "batch names material map entry " +
                                         std::to_string(record.materialIndex) + ", past the end",
                                     ElementRef(ElementKind::Section, r), profile);
                }
                // §16's measured finding: `boneCount` is a bone *index* whose
                // animated visibility gates the batch, and 0xFFFF means always
                // drawn. It is section state, not a count.
                section.native.set(kSectionVisibilityNode, static_cast<i64>(record.boneCount));
            }
            sectionOfRegion[r] = builder.addSection(std::move(section));
        }

        for (std::size_t r = 0; r < division.regions.size(); ++r) {
            const m3::Region& region = division.regions[r];
            const Vector2f uvDecode = UvDecodeFor(region);
            const u32 base = region.firstVertex - lowest;
            const std::size_t first = region.firstIndex;
            const std::size_t last = first + region.indexCount;
            for (std::size_t i = first; i + 2 < last && i + 2 < division.faces.size(); i += 3) {
                const std::array<u32, 3> corners = {static_cast<u32>(division.faces[i + 0]) + base,
                                                    static_cast<u32>(division.faces[i + 1]) + base,
                                                    static_cast<u32>(division.faces[i + 2]) + base};
                if (corners[0] >= builder.vertexCount() || corners[1] >= builder.vertexCount() ||
                    corners[2] >= builder.vertexCount()) {
                    diagnostics.warn(DiagCode::IndexOutOfRange,
                                     "region face corner past the division's vertex slice",
                                     ElementRef(ElementKind::Mesh, d));
                    continue;
                }
                const geom::FaceId face =
                    builder.addTriangle(geom::VertexId(corners[0]), geom::VertexId(corners[1]),
                                        geom::VertexId(corners[2]), sectionOfRegion[r]);
                for (u32 c = 0; c < 3; ++c) {
                    const u32 global = corners[c] + lowest;
                    if (global < normals.size()) {
                        builder.setCornerAttr(face, c, geom::names::kNormal,
                                              Rebase(normals[global]));
                    }
                    if (global < colors.size()) {
                        // Stored RGBA, which is what a `U8x4` colour layer means
                        // everywhere else; the `.m3` blob is BGRA.
                        const m3::ColorBGRA& color = colors[global];
                        const std::array<u8, 4> rgba{color.r, color.g, color.b, color.a};
                        builder.setCornerAttr(face, c, geom::names::color(0), rgba);
                    }
                    if (global < tangents.size()) {
                        const Vector4f& tangent = tangents[global];
                        const Vector3f axis =
                            Rebase(Vector3f{tangent.x, tangent.y, tangent.z});
                        builder.setCornerAttr(face, c, geom::names::kTangent,
                                              Vector4f{axis.x, axis.y, axis.z, tangent.w});
                    }
                    for (std::size_t u = 0; u < uvSets.size(); ++u) {
                        if (global < uvSets[u].size()) {
                            const Vector2f& raw = uvSets[u][global];
                            builder.setCornerAttr(
                                face, c, geom::names::uv(static_cast<u32>(u)),
                                Vector2f{raw.x * uvDecode.x + uvDecode.y,
                                         raw.y * uvDecode.x + uvDecode.y});
                        }
                    }
                }
            }

            // Skinning, through the region's own bone-lookup window.
            for (u32 v = 0; v < region.vertexCount; ++v) {
                const u32 global = region.firstVertex + v;
                if (global >= boneIndices.size() || global >= boneWeights.size()) {
                    break;
                }
                for (std::size_t k = 0; k < 4; ++k) {
                    const f32 weight = static_cast<f32>(boneWeights[global][k]) / 255.0f;
                    if (weight <= 0.0f) {
                        continue;
                    }
                    const std::size_t slot = region.firstBoneLookup + boneIndices[global][k];
                    const u32 bone =
                        slot < source.boneLookup.size() ? source.boneLookup[slot] : region.rootBone;
                    if (bone >= model.nodes.size()) {
                        diagnostics.warn(DiagCode::DanglingNodeReference,
                                         "bone lookup names bone " + std::to_string(bone),
                                         ElementRef(ElementKind::Mesh, d));
                        continue;
                    }
                    builder.addInfluence(geom::VertexId(global - lowest), bone, weight);
                }
            }
        }

        geom::MeshBuilder::BuildOutcome outcome = builder.build();
        outcome.mesh.name = "division_" + std::to_string(d);
        outcome.mesh.lodLevel = static_cast<u32>(d);
        outcome.mesh.recomputeBounds();
        model.meshes.push_back(std::move(outcome.mesh));
    }

    model.profileSets.push_back(std::move(set));
    m3_anim::Context animContext;
    animContext.profile = profile;
    animContext.bases = m3_anim::NodeBases::Of(source);
    animContext.layerOrdinals = std::move(layerOrdinals);

    const u32 modelIndex = static_cast<u32>(document.models.size());
    document.models.push_back(std::move(model));
    m3_anim::Import(source, animContext, document, modelIndex, diagnostics);

    result.value = std::move(document);
    return result;
}

// ============================================================================
// toM3
// ============================================================================

Result<m3::Model> M3Converter::toM3(const Document& document, ProfileId profile,
                                    u32 targetVersion, const M3ExportSettings& settings) const {
    Result<m3::Model> result;
    if (!checkExportProfile(document, profile, result.diagnostics)) {
        return result;
    }
    checkRigConvention(document, profile, result.diagnostics);

    Diagnostics& diagnostics = result.diagnostics;
    m3::Model out;
    // 0 means "whatever this profile's own game reads". The MODL version is the
    // one chunk version the caller has always chosen, because it is also how
    // `ProfileForVersion` decides which game a file came from.
    out.setVersion(static_cast<i32>(targetVersion != 0 ? targetVersion
                                    : profile == ProfileId::Heroes
                                        ? kHeroesModlVersion
                                        : static_cast<u32>(m3::SC2_MAX_MODEL_VERSION)));
    if (document.models.empty()) {
        result.value = std::move(out);
        return result;
    }

    const Model& model = document.models.front();
    out.name = document.name.empty() ? model.name : document.name;
    out.bounds = FromExtent(model.bounds);
    out.collisionBounds = out.bounds;
    const ProfileMaterialSet* set = model.setFor(profile);
    if (set != nullptr) {
        out.flags = static_cast<m3::ModelFlag>(static_cast<u32>(set->native.value("modelFlags")));
    }

    // Where each node lands, for the animation export below: an `.m3` keeps a
    // property's AnimRef ON the record that owns the property.
    m3_anim::ExportContext animContext;
    animContext.profile = profile;
    animContext.nodeSlots.assign(model.nodes.size(), m3_anim::ExportContext::NodeSlot{});

    // --- bones and the records that hang off them ---------------------------
    //
    // Which nodes become `BONE` records. An `.m3` has one node vocabulary --
    // the bone -- so every transform-bearing node crosses as one: a HELPER is
    // an articulation joint that merely does not skin (Warcraft III rigs
    // interleave them as parents of half the skeleton, and Blizzard's own
    // Warcraft III conversions write them all as bones -- the Footman's 47),
    // and an attachment, light or camera that carries a rest offset needs a
    // bone to keep it on. The identity-transform carrier nodes the m3 import
    // itself creates for ATT_/LITE/CAM_ stay boneless, so an m3 round trip
    // does not grow a bone per pass.
    std::vector<u32> boneOf(model.nodes.size(), 0xFFFFu);
    const auto identityLocal = [](const Node& node) {
        const Vector3f& t = node.local.translation;
        const Quaternion& r = node.local.rotation;
        const Vector3f& sc = node.local.scale;
        constexpr f32 e = 1e-6f;
        return std::fabs(t.x) < e && std::fabs(t.y) < e && std::fabs(t.z) < e &&
               std::fabs(r.x) < e && std::fabs(r.y) < e && std::fabs(r.z) < e &&
               std::fabs(std::fabs(r.w) - 1.0f) < e && std::fabs(sc.x - 1.0f) < e &&
               std::fabs(sc.y - 1.0f) < e && std::fabs(sc.z - 1.0f) < e;
    };
    // Parents before children -- the format's contract, not a preference: the
    // engine (and this build's renderer, built from its decompile) resolves
    // the hierarchy in ONE linear pass and reads a forward parent reference
    // as a root, so a child stored before its parent re-roots at the origin
    // with its whole subtree. The document keeps its source order and MDX
    // promises nothing (`SEAltarOfStars` parents node 5 to node 35), so the
    // bone indices are assigned in breadth-first order instead; siblings keep
    // their source order.
    std::vector<u32> topological;
    topological.reserve(model.nodes.size());
    // Only when the source order actually violates the contract: a shipped
    // `.m3` is already parents-first and keeps its exact order (a batch's
    // visibility-gate bone is an index into that order), so reordering one
    // would be churn with a hostage.
    bool ordered = true;
    for (u32 n = 0; n < model.nodes.size() && ordered; ++n) {
        const u32 parent = model.nodes.nodes[n].parent;
        ordered = parent == kInvalidNode || parent < n;
    }
    if (ordered) {
        for (u32 n = 0; n < model.nodes.size(); ++n) {
            topological.push_back(n);
        }
    } else {
        std::vector<std::vector<u32>> children(model.nodes.size());
        for (u32 n = 0; n < model.nodes.size(); ++n) {
            const u32 parent = model.nodes.nodes[n].parent;
            if (parent == kInvalidNode || parent >= model.nodes.size()) {
                topological.push_back(n);
            } else {
                children[parent].push_back(n);
            }
        }
        for (std::size_t head = 0; head < topological.size(); ++head) {
            for (u32 child : children[topological[head]]) {
                topological.push_back(child);
            }
        }
        // A parent cycle is a damaged document; the stragglers still get
        // bones (as roots) rather than vanishing.
        if (topological.size() < model.nodes.size()) {
            std::vector<bool> seen(model.nodes.size(), false);
            for (u32 n : topological) {
                seen[n] = true;
            }
            for (u32 n = 0; n < model.nodes.size(); ++n) {
                if (!seen[n]) {
                    topological.push_back(n);
                }
            }
        }
    }
    if (!ordered) {
        diagnostics.info(DiagCode::RigConventionChanged,
                         "bones reordered parents-first (the engine resolves the "
                         "hierarchy in one pass)",
                         ElementRef(ElementKind::Document, 0), profile);
    }
    for (const u32 n : topological) {
        const Node& node = model.nodes.nodes[n];
        bool carries = false;
        switch (node.kind) {
        case NodeKind::Bone:
        case NodeKind::Helper:
            carries = true;
            break;
        case NodeKind::Attachment:
        case NodeKind::Light:
        case NodeKind::Camera:
            carries = !identityLocal(node);
            break;
        default:
            break;
        }
        if (carries) {
            boneOf[n] = static_cast<u32>(out.bones.size());
            out.bones.emplace_back();
        }
    }
    // The nearest ancestor that owns a bone -- a parent that stayed boneless
    // (an event node, say) must not re-root its children.
    const auto nearestBone = [&](std::size_t n) -> u32 {
        for (u32 p = model.nodes.nodes[n].parent; p != kInvalidNode;
             p = model.nodes.nodes[p].parent) {
            if (p < boneOf.size() && boneOf[p] != 0xFFFFu) {
                return boneOf[p];
            }
        }
        return 0xFFFFu;
    };
    for (std::size_t n = 0; n < model.nodes.size(); ++n) {
        if (boneOf[n] == 0xFFFFu) {
            continue;
        }
        const Node& node = model.nodes.nodes[n];
        // Transform tracks flow to the bone. A light keeps its Light slot (its
        // colour and falloff are the tracks an m3 light can state) and a
        // camera its Camera slot; both are assigned below.
        if (node.kind != NodeKind::Light && node.kind != NodeKind::Camera) {
            animContext.nodeSlots[n] = {m3_anim::ExportContext::Slot::Bone, boneOf[n]};
        }
        m3::Bone bone;
        bone.name = node.name;
        bone.flags = FromNodeFlags(node.flags, static_cast<u32>(node.native.value("m3FlagBits")));
        const u32 parentBone = nearestBone(n);
        bone.parentIndex = parentBone == 0xFFFFu ? u16(0xFFFFu) : static_cast<u16>(parentBone);
        bone.position.initValue = Unrebase(node.local.translation);
        const Quaternion& rotation = node.local.rotation;
        bone.rotation.initValue = Quaternion{rotation.y, -rotation.x, rotation.z, rotation.w};
        bone.scale.initValue = node.local.scale;
        // The batch gate reads this and nothing else when no clip drives it.
        bone.visibility.initValue =
            VisibilityRest(model, static_cast<u32>(n)) > 0.5f ? 1u : 0u;
        out.bones[boneOf[n]] = std::move(bone);

        // IREF, one matrix per bone and in the same order. `poseMatrixOf`
        // answers with the stored matrix when the document carries one and
        // otherwise composes the schema's own derivation -- the inverse of the
        // composed rest chain -- which is the only answer a document from
        // another format can give. Without this the renderer binds every bone
        // as identity and the skin explodes.
        m3::InitialReference reference;
        reference.matrix = UnrebaseMatrix(model.nodes.inverseBindMatrix(static_cast<u32>(n)));
        out.initialReference.resize(out.bones.size());
        out.initialReference[boneOf[n]] = reference;
    }
    out.skinBoneCount = static_cast<u32>(out.bones.size());

    for (std::size_t n = 0; n < model.nodes.size(); ++n) {
        const Node& node = model.nodes.nodes[n];
        // The node's own bone when the pass above gave it one (it carries a
        // rest offset), the nearest ancestor's otherwise.
        const u32 carrier = boneOf[n] != 0xFFFFu ? boneOf[n] : nearestBone(n);
        const u32 parentBone = carrier;
        switch (node.kind) {
        case NodeKind::Attachment: {
            m3::AttachmentPoint point;
            point.name = node.name;
            point.boneIndex = parentBone == 0xFFFFu ? 0u : parentBone;
            if (boneOf[n] == 0xFFFFu) {
                animContext.nodeSlots[n] = {m3_anim::ExportContext::Slot::Attachment,
                                            static_cast<u32>(out.attachmentPoints.size())};
            }
            out.attachmentPoints.push_back(std::move(point));
            break;
        }
        case NodeKind::Light: {
            m3::Light light;
            light.boneIndex = static_cast<u16>(parentBone == 0xFFFFu ? 0u : parentBone);
            light.flags =
                static_cast<m3::LightFlag>(static_cast<u32>(node.native.value("m3LightFlags")));
            if (const auto* payload = std::get_if<LightPayload>(&node.payload)) {
                switch (payload->kind) {
                case LightKind::Spot:
                    light.lightType = m3::LightType::Spot;
                    break;
                case LightKind::Directional:
                    light.lightType = m3::LightType::Directional;
                    break;
                case LightKind::Ambient:
                    diagnostics.warn(DiagCode::FeatureDropped,
                                     "M3 has no ambient light; written as omni",
                                     ElementRef(ElementKind::Node, n), profile);
                    [[fallthrough]];
                default:
                    light.lightType = m3::LightType::Omni;
                    break;
                }
                light.diffuseColor.initValue = payload->color;
                light.intensityMultiplier.initValue = payload->intensity;
                light.attenuationStart.initValue = payload->attenuationStart;
                light.attenuationEnd = payload->attenuationEnd;
                light.hotSpot.initValue = payload->hotSpot;
                light.falloff.initValue = payload->falloff;
            }
            animContext.nodeSlots[n] = {m3_anim::ExportContext::Slot::Light,
                                        static_cast<u32>(out.lights.size())};
            out.lights.push_back(std::move(light));
            break;
        }
        case NodeKind::Camera: {
            m3::Camera camera;
            camera.name = node.name;
            camera.boneIndex = parentBone == 0xFFFFu ? 0u : parentBone;
            if (const auto* payload = std::get_if<CameraPayload>(&node.payload)) {
                camera.fieldOfView.initValue = payload->fov;
                camera.nearClip.initValue = payload->nearClip;
                camera.farClip.initValue = payload->farClip;
            }
            animContext.nodeSlots[n] = {m3_anim::ExportContext::Slot::Camera,
                                        static_cast<u32>(out.cameras.size())};
            out.cameras.push_back(std::move(camera));
            break;
        }
        default:
            break;
        }
    }

    // --- materials ----------------------------------------------------------
    m3_core::Context context;
    context.modelVersion = targetVersion;
    context.textureRefs = &document.textures;
    // The pass fold's inputs (WC3_SD_MATERIAL_TO_SC2_DESIGN.md §5): whether the
    // stacks are Warcraft III's, what the caller learned about the textures'
    // alpha, and where a composite's section entries may go -- after the slots.
    context.warcraftPasses = document.defaultProfile == ProfileId::Wc3Classic ||
                             document.defaultProfile == ProfileId::Wc3Reforged;
    context.exactPasses = settings.exactPasses;
    if (!settings.textureAlphaClasses.empty()) {
        context.textureAlphaClasses = &settings.textureAlphaClasses;
    }
    context.compositeSections = true;
    context.materialMapBase = static_cast<u32>(model.materialSlots.size());
    for (const TextureRef& ref : document.textures) {
        context.texturesByPath.emplace_back(ref.path,
                                            static_cast<u32>(context.texturesByPath.size()));
    }
    // One map entry per slot, so a batch's `materialIndex` is the slot index.
    // Resolved at the set's DEFAULT look, not look 0: a Diablo III document
    // carries one material per (slot, look) and the converter was told which
    // look this export wears (`D3ImportOptions::materialLook` set it).
    const ProfileMaterialSet* exportSet = model.setFor(profile);
    const u32 exportLook = exportSet != nullptr ? exportSet->defaultLook : 0u;
    for (std::size_t slot = 0; slot < model.materialSlots.size(); ++slot) {
        const Material* material = Resolve(model, static_cast<u32>(slot), profile, exportLook);
        if (material == nullptr) {
            out.materialMaps.push_back(m3::MaterialMap{});
            animContext.materialOrdinals.emplace_back();
            diagnostics.warn(DiagCode::SlotNotBound,
                             "slot " + model.materialSlots[slot] + " has no material",
                             ElementRef(ElementKind::Slot, slot), profile);
            continue;
        }
        // Which of the material's passes carry an alpha or texture-id track:
        // the fold's coverage switch and flipbook rules read them.
        m3_core::ExportHints hints;
        for (const AnimChannel& entry : model.animChannels.channels) {
            if (entry.target.kind != TrackTarget::Kind::MaterialLayer ||
                entry.target.material.profile != profile ||
                entry.target.material.slot != static_cast<u32>(slot) ||
                entry.target.sub == kWholeMaterial || entry.target.sub >= 64) {
                continue;
            }
            if (entry.target.channel == Channel::Alpha) {
                hints.alphaTrackedOrdinals |= u64{1} << entry.target.sub;
            } else if (entry.target.channel == Channel::TextureIndex) {
                hints.textureIndexTrackedOrdinals |= u64{1} << entry.target.sub;
            }
        }
        std::vector<u32> slotOrdinals;
        m3_core::ExportReport report;
        out.materialMaps.push_back(m3_core::ExportMaterial(*material, profile, context, out,
                                                           diagnostics, &slotOrdinals, &hints,
                                                           &report));
        animContext.materialOrdinals.push_back(std::move(slotOrdinals));
        if (report.coverageSwitchOrdinal != kInvalidIndex) {
            animContext.coverageSwitches.emplace(static_cast<u32>(slot),
                                                 report.coverageSwitchOrdinal);
        }
    }
    // A composite's sections name map entries of their own; they go after the
    // slots so a batch's `materialIndex` stays the slot index.
    for (const m3::MaterialMap& trailing : context.trailingMaps) {
        out.materialMaps.push_back(trailing);
        animContext.materialOrdinals.emplace_back();
    }
    context.trailingMaps.clear();

    // --- geometry -----------------------------------------------------------
    //
    // One division for the whole model, one region per mesh section. The game
    // (and this build's renderer) draws `divisions[0]` and nothing after it,
    // so a division per mesh silently dropped every mesh but the first the
    // moment a document carried more than one -- which a shipped `.m3` never
    // does (one division is the format's own shape; Blizzard's own Warcraft
    // III conversions put seven regions in one division) but every
    // cross-profile document did. Every region writes into the model's single
    // vertex buffer, so their `firstVertex` runs are the concatenation of the
    // meshes'.
    geom::RenderMeshDesc desc;
    desc.attributes = {
        {geom::names::kPosition, utils::AttributeClass::Position, utils::AttributeEncoding::Float32,
         3, 0},
        {geom::names::kNormal, utils::AttributeClass::Normal, utils::AttributeEncoding::Float32, 3,
         0},
        {geom::names::kTangent, utils::AttributeClass::Tangent,
         utils::AttributeEncoding::Float32, 4, 0},
    };
    desc.includeSkin = true;
    desc.maxInfluences = Profile(profile).maxBoneInfluences;

    // The vertex declaration follows the meshes, not a default: an `.m3` may
    // carry none, one or five UV sets and an optional colour, and writing the
    // one shape this converter used to assume both dropped sets a material
    // samples and invented one for a model that had none.
    const std::size_t maxUvSets =
        std::min<std::size_t>(Profile(profile).maxUvSets, M3VertexEncoder::kMaxUvSets);
    std::size_t uvCount = 0;
    bool hasColor = false;
    for (const Mesh& mesh : model.meshes) {
        for (std::size_t u = 0; u < maxUvSets; ++u) {
            if (mesh.attributes.has(geom::names::uv(static_cast<u32>(u)), geom::Domain::Halfedge) ||
                mesh.attributes.has(geom::names::uv(static_cast<u32>(u)), geom::Domain::Vertex)) {
                uvCount = u + 1;
            }
        }
        hasColor = hasColor ||
                   mesh.attributes.has(geom::names::color(0), geom::Domain::Halfedge) ||
                   mesh.attributes.has(geom::names::color(0), geom::Domain::Vertex);
    }
    for (std::size_t u = 0; u < uvCount; ++u) {
        desc.attributes.push_back({geom::names::uv(static_cast<u32>(u)),
                                   utils::AttributeClass::UV,
                                   utils::AttributeEncoding::Float32, 2, 0});
    }
    if (hasColor) {
        desc.attributes.push_back({geom::names::color(0), utils::AttributeClass::Color,
                                   utils::AttributeEncoding::UInt8, 4, 0});
    }

    M3VertexEncoder encoder(uvCount, hasColor);
    std::size_t writtenVertices = 0;
    m3::MeshDivision division;

    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        const geom::RenderMesh render = geom::BuildRenderMesh(mesh, desc);
        diagnostics.append(render.diagnostics);

        const std::vector<Vector3f> positions = render.vertices.getPositions();
        const std::vector<Vector3f> normals = render.vertices.getNormals();
        const std::vector<Vector4f> tangents = render.vertices.getTangents();
        std::array<std::vector<Vector2f>, M3VertexEncoder::kMaxUvSets> uvSets;
        for (std::size_t u = 0; u < uvCount; ++u) {
            uvSets[u] = render.vertices.getUVs(u);
        }
        // `UInt8` is an integer encoding: the components come back 0..255, not
        // 0..1, so they are the bytes the blob wants.
        const std::vector<Vector4f> colors =
            hasColor ? render.vertices.getColors() : std::vector<Vector4f>{};
        const std::vector<std::array<u32, 4>> boneIndices = render.vertices.getBoneIndices();
        const std::vector<std::array<f32, 4>> boneWeights = render.vertices.getBoneWeights();

        for (const geom::RenderRange& range : render.ranges) {
            // What this export must NOT draw (D3_TO_SC2_DESIGN.md §4). A
            // cross-profile `Hidden` section is a variant that is not worn,
            // and it cannot ride `RegionFlag::Hidden` — StarCraft II reads
            // that bit as the cloth-pair marker, never as "do not draw", so a
            // flagged region draws anyway. Only an m3-native section (its bag
            // carries the verbatim `regionFlags`) keeps emitting under the
            // flag, because there the bit really is the pair marker the round
            // trip must preserve. The second rule is Diablo III's per-look
            // bit: `MaterialFlags::Invisible` is the material saying this
            // piece is not worn under the export's look.
            if (range.section < mesh.sections.size()) {
                const MeshSection& skipTest = mesh.sections[range.section];
                if (hasFlag(skipTest.flags, SectionFlags::Hidden) &&
                    skipTest.native.find("regionFlags") == nullptr) {
                    continue;
                }
            }
            if (const Material* bound =
                    Resolve(model, range.materialSlot, profile, exportLook);
                bound != nullptr &&
                hasFlag(bound->Common().flags, MaterialFlags::Invisible)) {
                continue;
            }
            m3::Region region;
            region.index = static_cast<u32>(division.regions.size());
            region.firstVertex = static_cast<u32>(writtenVertices);
            region.firstIndex = static_cast<u32>(division.faces.size());
            region.indexCount = range.indexCount;
            region.boneWeightPairs = 4;
            region.boneIndexPairs = 4;
            // `M3VertexEncoder` writes `uv * 2048`, so the pair that reads it
            // back is the stock one. Stated rather than left at the struct's
            // uninitialised float, which a v5 reader would take literally --
            // and the region says it is a v5 record, or nothing reads them.
            region.setVersion(5);
            region.uvScale = 16.0f;
            region.uvOffset = 0.0f;
            region.firstBoneLookup = static_cast<u16>(out.boneLookup.size());

            if (range.section < mesh.sections.size()) {
                const MeshSection& section = mesh.sections[range.section];
                region.rootBone = static_cast<u16>(section.native.value("rootBone"));
                region.flags = static_cast<m3::RegionFlag>(static_cast<u32>(section.native.value(
                    "regionFlags", hasFlag(section.flags, SectionFlags::Hidden)
                                       ? static_cast<i64>(m3::RegionFlag::Hidden)
                                       : 0)));
            }

            // A region owns its vertices. Its faces index them from its own
            // `firstVertex`, and -- the reason it must own them -- a vertex's
            // four bone indices are slots in *this* region's bone-lookup window,
            // so the same vertex shared by two regions with different windows
            // could not be encoded once. Regions therefore get disjoint slices
            // of the model's one buffer, exactly as a shipped `.m3` has them,
            // and a vertex two regions use is written into both.
            std::vector<u32> window;
            const auto slotFor = [&window](u32 bone) -> u8 {
                for (std::size_t i = 0; i < window.size(); ++i) {
                    if (window[i] == bone) {
                        return static_cast<u8>(i);
                    }
                }
                window.push_back(bone);
                return static_cast<u8>(window.size() - 1);
            };

            // First use order, so the face indices below stay as close to the
            // source's as a re-emitted buffer can be.
            std::vector<u32> localOf(positions.size(), kInvalidIndex);
            std::vector<u32> sourceOf;
            for (u32 i = 0; i < range.indexCount; ++i) {
                const u32 index = render.indices[range.firstIndex + i];
                if (index >= localOf.size()) {
                    diagnostics.warn(DiagCode::IndexOutOfRange,
                                     "face corner past the mesh's vertex buffer",
                                     ElementRef(ElementKind::Mesh, m), profile);
                    division.faces.push_back(0);
                    continue;
                }
                if (localOf[index] == kInvalidIndex) {
                    localOf[index] = static_cast<u32>(sourceOf.size());
                    sourceOf.push_back(index);
                }
                division.faces.push_back(static_cast<u16>(localOf[index]));
            }
            if (sourceOf.size() > 0x10000u) {
                diagnostics.warn(DiagCode::IndexWidthExceeded,
                                 "region needs " + std::to_string(sourceOf.size()) +
                                     " vertices, past the u16 index a face corner is",
                                 ElementRef(ElementKind::Mesh, m), profile);
            }

            for (u32 source : sourceOf) {
                std::array<u8, 4> indices{0, 0, 0, 0};
                std::array<u8, 4> weights{0, 0, 0, 0};
                for (std::size_t k = 0; k < 4; ++k) {
                    if (source >= boneIndices.size() || source >= boneWeights.size() ||
                        boneWeights[source][k] <= 0.0f) {
                        continue;
                    }
                    const u32 node = boneIndices[source][k];
                    const u32 bone = node < boneOf.size() ? boneOf[node] : 0xFFFFu;
                    if (bone == 0xFFFFu) {
                        continue;
                    }
                    indices[k] = slotFor(bone);
                    weights[k] = static_cast<u8>(
                        std::clamp(boneWeights[source][k], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                // A mesh with no tangent layer -- one imported from a format that
                // has none -- gets the neutral frame the format's own default is.
                Vector4f tangent{1, 0, 0, 1};
                if (source < tangents.size()) {
                    const Vector3f axis = Unrebase(
                        Vector3f{tangents[source].x, tangents[source].y, tangents[source].z});
                    tangent = Vector4f{axis.x, axis.y, axis.z, tangents[source].w};
                }
                std::array<Vector2f, M3VertexEncoder::kMaxUvSets> uvs{};
                for (std::size_t u = 0; u < uvCount; ++u) {
                    if (source < uvSets[u].size()) {
                        uvs[u] = uvSets[u][source];
                    }
                }
                std::array<u8, 4> color{255, 255, 255, 255};
                if (source < colors.size()) {
                    for (std::size_t c = 0; c < 4; ++c) {
                        color[c] = static_cast<u8>(
                            std::clamp(colors[source].data[c], 0.0f, 255.0f) + 0.5f);
                    }
                }
                encoder.push(Unrebase(positions[source]),
                             source < normals.size() ? Unrebase(normals[source])
                                                     : Vector3f{0, 0, 1},
                             tangent, uvs, color, indices, weights);
            }
            region.vertexCount = static_cast<u32>(sourceOf.size());
            writtenVertices += sourceOf.size();

            const u32 paletteLimit = Profile(profile).maxBonesPerPalette;
            if (paletteLimit != 0 && window.size() > paletteLimit) {
                diagnostics.warn(DiagCode::BonePaletteLimit,
                                 "region needs " + std::to_string(window.size()) +
                                     " bones, past the profile's " +
                                     std::to_string(paletteLimit),
                                 ElementRef(ElementKind::Mesh, m), profile);
            }
            for (u32 bone : window) {
                out.boneLookup.push_back(static_cast<u16>(bone));
            }
            region.boneLookupCount = static_cast<u16>(window.size());

            m3::Batch batch;
            batch.regionIndex = static_cast<u16>(division.regions.size());
            batch.materialIndex = static_cast<u16>(range.materialSlot);
            batch.boneCount = 0xFFFFu;
            if (range.section < mesh.sections.size()) {
                batch.boneCount = static_cast<u16>(
                    mesh.sections[range.section].native.value(kSectionVisibilityNode,
                                                             kSectionAlwaysDrawn));
            }
            // A section with a keyed visibility and no stored gate -- a
            // Warcraft III geoset animation -- gets a dedicated gate bone: the
            // batch gates on a BONE's visibility and nothing else, and hanging
            // the keys on a skinned bone would hide its subtree instead of the
            // draw. The bone is a root with an identity rest, appended after
            // `skinBoneCount` because it skins nothing; the anim export wires
            // its AnimRef to the channel's own stream (`sectionGateBones`).
            if (batch.boneCount == static_cast<u16>(kSectionAlwaysDrawn)) {
                // A Warcraft III geoset "visibility" arrives as a Section
                // ALPHA channel (GEOA is an alpha track). Only a binary,
                // step-interpolated one is a gate; a fade needs a layer alpha
                // and drawing it un-faded beats hiding it outright.
                const auto binaryStep = [&document](u32 channelId) {
                    bool any = false;
                    for (const Clip& clip : document.clips) {
                        for (const SubTrackContainer& container : clip.containers) {
                            for (const SubTrack& track : container.subTracks) {
                                if (track.channel != channelId) {
                                    continue;
                                }
                                any = true;
                                const f32* values =
                                    reinterpret_cast<const f32*>(track.values.data());
                                const std::size_t count = track.values.size() / sizeof(f32);
                                // A one-key track steps by definition -- the
                                // WoW hide idiom is one zero key, tagged with
                                // whatever interp the file happened to carry.
                                if (track.interp != Interpolation::Step && count > 1) {
                                    return false;
                                }
                                for (std::size_t k = 0; k < count; ++k) {
                                    if (values[k] > 0.01f && values[k] < 0.99f) {
                                        return false;
                                    }
                                }
                            }
                        }
                    }
                    return any;
                };
                const AnimChannel* vis = nullptr;
                const AnimChannel* fade = nullptr;
                for (const AnimChannel& entry : model.animChannels.channels) {
                    if (entry.target.kind != TrackTarget::Kind::Section ||
                        entry.target.mesh != static_cast<u32>(m) ||
                        entry.target.sub != range.section) {
                        continue;
                    }
                    if (entry.target.channel == Channel::Visibility ||
                        (entry.target.channel == Channel::Alpha && binaryStep(entry.id))) {
                        vis = &entry;
                        break;
                    }
                    if (entry.target.channel == Channel::Alpha) {
                        fade = &entry;
                    }
                }
                if (vis == nullptr) {
                    // WoW hides a batch through its MATERIAL: the element
                    // alpha (`M2Color` times the unit-0 weight, the shipped
                    // one-zero-key idiom) targets the material slot, not the
                    // section. A binary one is the same statement a geoset
                    // visibility makes, and the gate bone is the spelling
                    // retail's submission skip (and this build's renderer)
                    // actually reads; a fade keeps riding the carrier alpha
                    // layer the anim export plants. Only a channel that ever
                    // REACHES zero gates -- a constant-one weight is most
                    // batches, and a gate bone per batch would be noise.
                    const auto everZero = [&document](u32 channelId) {
                        for (const Clip& clip : document.clips) {
                            for (const SubTrackContainer& container : clip.containers) {
                                for (const SubTrack& track : container.subTracks) {
                                    if (track.channel != channelId) {
                                        continue;
                                    }
                                    const f32* values =
                                        reinterpret_cast<const f32*>(track.values.data());
                                    const std::size_t count = track.values.size() / sizeof(f32);
                                    for (std::size_t k = 0; k < count; ++k) {
                                        if (values[k] <= 0.01f) {
                                            return true;
                                        }
                                    }
                                }
                            }
                        }
                        return false;
                    };
                    for (const AnimChannel& entry : model.animChannels.channels) {
                        if (entry.target.kind != TrackTarget::Kind::MaterialLayer ||
                            entry.target.material.profile != profile ||
                            entry.target.material.slot != range.materialSlot ||
                            entry.target.channel != Channel::Alpha) {
                            continue;
                        }
                        if (entry.target.sub != kWholeMaterial && entry.target.sub != 0) {
                            continue;
                        }
                        if (binaryStep(entry.id) && everZero(entry.id)) {
                            vis = &entry;
                            break;
                        }
                    }
                }
                // A fade cannot gate -- it rides the material as a Color-flag
                // alpha layer whose mapAlpha carries the keys (the layer the
                // oracle's own conversions animate). The construction smoke
                // over the Barracks is the canonical case: alpha 0 through
                // every Stand, easing in only while building.
                if (vis == nullptr && fade != nullptr &&
                    range.materialSlot < out.materialMaps.size() &&
                    animContext.sectionAlphaLayers.find(fade->id) ==
                        animContext.sectionAlphaLayers.end()) {
                    // Every standard material the slot draws with -- one, or
                    // a composite's sections: a geoset fade fades every pass.
                    std::vector<u32> targets;
                    const m3::MaterialMap& map = out.materialMaps[range.materialSlot];
                    if (map.materialType == m3::MaterialType::Standard) {
                        targets.push_back(map.materialIndex);
                    } else if (map.materialType == m3::MaterialType::Composite &&
                               map.materialIndex < out.compositeMaterials.size()) {
                        for (const m3::CompositeSection& section :
                             out.compositeMaterials[map.materialIndex].sections) {
                            if (section.materialIndex < out.materialMaps.size() &&
                                out.materialMaps[section.materialIndex].materialType ==
                                    m3::MaterialType::Standard) {
                                targets.push_back(
                                    out.materialMaps[section.materialIndex].materialIndex);
                            }
                        }
                    }
                    f32 rest = 1.0f;
                    if (fade->hasInitValue() && fade->initValue.size() >= sizeof(f32)) {
                        std::memcpy(&rest, fade->initValue.data(), sizeof(f32));
                    }
                    for (const u32 matIndex : targets) {
                        if (matIndex >= out.standardMaterials.size()) {
                            continue;
                        }
                        m3::StandardMaterial& mat = out.standardMaterials[matIndex];
                        // A free slot, or a Color-flag carrier the material
                        // fold planted for a static alpha: its multiply is
                        // the weight and its `mapAlpha` is free, so the two
                        // weights share the layer and multiply.
                        const auto shareable = [](const std::optional<m3::TextureLayer>& l) {
                            return l.has_value() &&
                                   hasFlag(l->flags, m3::TextureLayerFlag::Color) &&
                                   !l->mapAlpha.isAnimated();
                        };
                        u8 which = 0;
                        if (!mat.alphaLayer1.has_value()) {
                            which = 1;
                        } else if (!mat.alphaLayer2.has_value()) {
                            which = 2;
                        } else if (shareable(mat.alphaLayer1)) {
                            which = 1;
                        } else if (shareable(mat.alphaLayer2)) {
                            which = 2;
                        }
                        if (which == 0) {
                            continue;
                        }
                        std::optional<m3::TextureLayer>& carrierSlot =
                            which == 1 ? mat.alphaLayer1 : mat.alphaLayer2;
                        if (!carrierSlot.has_value()) {
                            m3::TextureLayer carrier;
                            carrier.flags = m3::TextureLayerFlag::Color;
                            carrier.color.initValue = m3::ColorBGRA{255, 255, 255, 255};
                            carrier.rgbMultiply.initValue = 1.0f;
                            carrierSlot = std::move(carrier);
                        }
                        carrierSlot->mapAlpha.initValue = rest;
                        animContext.sectionAlphaLayers[fade->id].emplace_back(matIndex, which);
                    }
                }
                if (vis != nullptr) {
                    auto gate = animContext.sectionGateBones.find(vis->id);
                    if (gate == animContext.sectionGateBones.end()) {
                        m3::Bone bone;
                        bone.name = mesh.name.empty()
                                        ? "section_vis_" + std::to_string(m)
                                        : mesh.name + "_vis";
                        bone.parentIndex = 0xFFFFu;
                        bone.rotation.initValue = Quaternion{0, 0, 0, 1};
                        bone.scale.initValue = Vector3f{1, 1, 1};
                        f32 rest = 1.0f;
                        if (vis->hasInitValue() &&
                            vis->initValue.size() >= sizeof(f32)) {
                            std::memcpy(&rest, vis->initValue.data(), sizeof(f32));
                        }
                        bone.visibility.initValue = rest != 0.0f ? 1u : 0u;
                        const u32 index = static_cast<u32>(out.bones.size());
                        out.bones.push_back(std::move(bone));
                        m3::InitialReference reference;
                        reference.matrix = Matrix44f::identity();
                        out.initialReference.push_back(reference);
                        gate = animContext.sectionGateBones.emplace(vis->id, index).first;
                    }
                    batch.boneCount = static_cast<u16>(gate->second);
                }
            }
            division.regions.push_back(std::move(region));
            division.batches.push_back(batch);
        }
    }
    // Every division states its bounding volume, and StarCraft II reads it
    // without first asking whether there is one: an import with no `MSEC` at
    // all crashes the Galaxy editor on a null array. 33,030 of the 33,060
    // shipped models carry one, 32,714 of them exactly one record against node
    // 0 — and 24,270 leave it unkeyed, which is what this writes. The animated
    // refinement (a `BNDS` run per sequence behind an `SDMB`) is the part we
    // have nothing to say about.
    m3::MeshSection section;
    section.nodeIndex = 0;
    section.bounds.initValue = out.bounds;
    // The bounds channel's id is a fixed one — every shipped model states it,
    // keyed or not. Inert here (nothing keys it), but a reader that looks the
    // channel up by id finds the one it expects.
    section.bounds.animId = kModelBoundsAnimId;
    division.msec.push_back(section);
    out.divisions.push_back(std::move(division));

    // The declaration the source stated, with only the bits this converter can
    // actually account for rewritten.
    constexpr u32 kLayoutBits =
        static_cast<u32>(m3::VertexFormatFlag::VertexColor) |
        static_cast<u32>(m3::VertexFormatFlag::UV1) | static_cast<u32>(m3::VertexFormatFlag::UV2) |
        static_cast<u32>(m3::VertexFormatFlag::UV3) | static_cast<u32>(m3::VertexFormatFlag::UV4) |
        static_cast<u32>(m3::VertexFormatFlag::UV5);
    constexpr m3::VertexFormatFlag kUvBit[M3VertexEncoder::kMaxUvSets] = {
        m3::VertexFormatFlag::UV1, m3::VertexFormatFlag::UV2, m3::VertexFormatFlag::UV3,
        m3::VertexFormatFlag::UV4, m3::VertexFormatFlag::UV5};
    u32 vertexFlags = set != nullptr
                          ? static_cast<u32>(set->native.value("vertexFlags", 0)) & ~kLayoutBits
                          : 0u;
    for (std::size_t u = 0; u < uvCount; ++u) {
        vertexFlags |= static_cast<u32>(kUvBit[u]);
    }
    if (hasColor) {
        vertexFlags |= static_cast<u32>(m3::VertexFormatFlag::VertexColor);
    }
    out.vertices.flags = static_cast<m3::VertexFormatFlag>(vertexFlags);
    out.vertices.data = std::move(encoder.data);
    out.vertices.initialize();

    // Last, because it re-imports the materials just written to recover which
    // ordinal each layer became, and puts an AnimRef back on the record that
    // owns the property.
    m3_anim::Export(document, 0, animContext, out, diagnostics);

    // The two parallel arrays MODL owes its scene objects. Every one of 1,535
    // shipped models with attachment points carries one addon entry per point
    // and every one of 343 with cameras carries one per camera -- always
    // 0xFFFF, no exceptions in either game. We wrote neither, leaving a count-0
    // Reference where the client walks an array in step with its owner.
    out.attachmentPointAddons.assign(out.attachmentPoints.size(), kNoAddon);
    out.camerasAddons.assign(out.cameras.size(), kNoAddon);

    result.value = std::move(out);
    return result;
}

// ============================================================================
// FormatConverter
// ============================================================================

Result<u32> M3Converter::mergeAnimation(Document& document, u32 model,
                                        const m3::Model& external) const {
    Result<u32> result;
    if (model >= document.models.size()) {
        result.diagnostics.error(DiagCode::ClipTargetMissing,
                                 "model " + std::to_string(model) + " of " +
                                     std::to_string(document.models.size()),
                                 ElementRef(ElementKind::Document, model));
        return result;
    }
    result.value = m3_anim::Merge(external, document, model, result.diagnostics);
    return result;
}

std::string M3Converter::formatId() const {
    return "m3";
}

std::string M3Converter::formatName() const {
    return "StarCraft II / Heroes of the Storm M3";
}

std::span<const ProfileId> M3Converter::profiles() const {
    return kM3Profiles;
}

bool M3Converter::supportsImport() const {
    return true;
}

bool M3Converter::supportsExport() const {
    return true;
}

u32 M3Converter::defaultExportVersion() const {
    return kHeroesModlVersion;
}

Result<Document> M3Converter::importFromBytes(std::span<const u8> data) const {
    m3::Parser parser;
    const m3::Model source = parser.parse(data);
    Result<Document> result = fromM3(source);
    for (const std::string& issue : parser.getIssues()) {
        result.diagnostics.warn(DiagCode::Unspecified, issue);
    }
    return result;
}

Result<std::vector<u8>> M3Converter::exportToBytes(const Document& document, ProfileId profile,
                                                   u32 version) const {
    Result<m3::Model> converted =
        toM3(document, profile, version);
    Result<std::vector<u8>> result;
    result.diagnostics = std::move(converted.diagnostics);
    if (!converted.ok()) {
        return result;
    }
    m3::Writer writer;
    result.value = writer.write(*converted);
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
