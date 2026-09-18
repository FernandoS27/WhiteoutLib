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
#include "m3_emitters.h"
#include "skin_skeleton.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>

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

/// Reorders `MATM` so every composite follows the entries its sections name,
/// and repoints everything that indexes the map. The editor builds the map in
/// index order and resolves a section through the material it already built at
/// that index, so a section naming a later entry hands it a null material
/// (SC2Editor 5.0.16: `ACCESS_VIOLATION reading 0x78`). No shipped file does:
/// 2,519 composites over 40,036 models, each right after its sections. `toM3`
/// numbers a slot's entry by the slot and appends the sections after every
/// slot, so it runs last -- everything before it reads a map index as a slot.
void PlaceSectionsFirst(m3::Model& out) {
    const std::size_t count = out.materialMaps.size();
    std::vector<u32> order;
    order.reserve(count);
    std::vector<u8> placed(count, 0);
    const auto place = [&](u32 m) {
        if (!placed[m]) {
            placed[m] = 1;
            order.push_back(m);
        }
    };
    for (u32 m = 0; m < count; ++m) {
        const m3::MaterialMap& map = out.materialMaps[m];
        if (map.materialType == m3::MaterialType::Composite &&
            map.materialIndex < out.compositeMaterials.size()) {
            for (const m3::CompositeSection& section :
                 out.compositeMaterials[map.materialIndex].sections) {
                // A composite never nests another (0 in either corpus).
                if (section.materialIndex < count &&
                    out.materialMaps[section.materialIndex].materialType !=
                        m3::MaterialType::Composite) {
                    place(section.materialIndex);
                }
            }
        }
        place(m);
    }

    std::vector<u32> remap(count);
    bool moved = false;
    for (u32 k = 0; k < count; ++k) {
        remap[order[k]] = k;
        moved = moved || order[k] != k;
    }
    if (!moved) {
        return;
    }
    std::vector<m3::MaterialMap> maps;
    maps.reserve(count);
    for (const u32 m : order) {
        maps.push_back(out.materialMaps[m]);
    }
    out.materialMaps = std::move(maps);

    const auto repoint = [&](u32 index) { return index < count ? remap[index] : index; };
    for (m3::CompositeMaterial& composite : out.compositeMaterials) {
        for (m3::CompositeSection& section : composite.sections) {
            section.materialIndex = repoint(section.materialIndex);
        }
    }
    for (m3::MeshDivision& division : out.divisions) {
        for (m3::Batch& batch : division.batches) {
            batch.materialIndex = static_cast<u16>(repoint(batch.materialIndex));
        }
    }
    for (m3::ParticleEmitter& emitter : out.particleEmitters) {
        emitter.materialIndex = repoint(emitter.materialIndex);
    }
    for (m3::RibbonEmitter& ribbon : out.ribbonEmitters) {
        ribbon.materialIndex = repoint(ribbon.materialIndex);
    }
    for (m3::Projector& projector : out.projections) {
        projector.materialReferenceIndex = repoint(projector.materialReferenceIndex);
    }
}

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

/// A vertex's bone weights as the bytes it stores: in proportion and summing to
/// exactly 255, as 1,002,734 shipped vertices do against a few hundred in one
/// file. Rounding each weight alone wrote a two-bone Warcraft III vertex 128 + 128.
std::array<u8, 4> QuantizeWeights(const std::array<f32, 4>& weights, std::size_t count) {
    std::array<u8, 4> out{0, 0, 0, 0};
    f32 total = 0.0f;
    for (std::size_t k = 0; k < count; ++k) {
        total += weights[k];
    }
    if (count == 0 || total <= 0.0f) {
        return out;
    }
    std::array<f32, 4> remainder{};
    u32 assigned = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const f32 exact = weights[k] / total * 255.0f;
        out[k] = static_cast<u8>(std::floor(exact));
        remainder[k] = exact - static_cast<f32>(out[k]);
        assigned += out[k];
    }
    // What rounding down left goes to the largest remainders, the earlier slot on a tie.
    while (assigned < 255) {
        std::size_t best = 0;
        for (std::size_t k = 1; k < count; ++k) {
            if (remainder[k] > remainder[best]) {
                best = k;
            }
        }
        ++out[best];
        remainder[best] -= 1.0f;
        ++assigned;
    }
    return out;
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

    // The emitter systems (§10.9), whole: each record a node under the bone it
    // names, at identity, since the placement is the bone's. A `PARC` is an
    // emission point of its source's system and imports as a node of the same
    // kind naming the source; copies follow every `PAR_`, each in its
    // `copyIndices` order, which is the slot order the engine numbers them in.
    // `m3_anim::NodeBases` states the same order.
    const m3_anim::NodeBases bases = m3_anim::NodeBases::Of(source);
    m3_emitters::ImportLinks links;
    links.boneCount = boneCount;
    links.particleBase = bases.particle;
    links.particleCount = static_cast<u32>(source.particleEmitters.size());
    links.ribbonBase = bases.ribbon;
    links.ribbonCount = static_cast<u32>(source.ribbonEmitters.size());

    for (std::size_t p = 0; p < source.particleEmitters.size(); ++p) {
        const m3::ParticleEmitter& emitter = source.particleEmitters[p];
        Node node;
        node.name = "particle_" + std::to_string(p);
        node.kind = NodeKind::Sc2ParticleEmitter;
        node.parent = links.bone(emitter.boneIndex);
        node.payload = m3_emitters::ImportParticle(emitter, links);
        tree.add(std::move(node));
    }
    for (std::size_t p = 0; p < source.particleEmitters.size(); ++p) {
        u32 slot = 0;
        for (const u32 copy : source.particleEmitters[p].copyIndices) {
            if (copy >= source.particleEmitterCopies.size()) {
                continue;
            }
            const m3::ParticleEmitterCopy& record = source.particleEmitterCopies[copy];
            Node node;
            node.name = "particle_" + std::to_string(p) + "_copy_" + std::to_string(++slot);
            node.kind = NodeKind::Sc2ParticleEmitter;
            node.parent = links.bone(record.boneIndex);
            node.payload = m3_emitters::ImportCopy(record, bases.particle + static_cast<u32>(p));
            tree.add(std::move(node));
        }
    }

    for (std::size_t r = 0; r < source.ribbonEmitters.size(); ++r) {
        const m3::RibbonEmitter& ribbon = source.ribbonEmitters[r];
        Node node;
        node.name = "ribbon_" + std::to_string(r);
        node.kind = NodeKind::Sc2RibbonEmitter;
        node.parent = links.bone(ribbon.boneIndex);
        node.payload = m3_emitters::ImportRibbon(ribbon, links);
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
                                 ElementRef(ElementKind::Section, static_cast<u32>(r)), profile);
            } else {
                const m3::Batch& record = division.batches[batch];
                if (record.materialIndex < model.materialSlots.size()) {
                    section.materialSlot = record.materialIndex;
                } else {
                    diagnostics.warn(DiagCode::IndexOutOfRange,
                                     "batch names material map entry " +
                                         std::to_string(record.materialIndex) + ", past the end",
                                     ElementRef(ElementKind::Section, static_cast<u32>(r)),
                                     profile);
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
                                     ElementRef(ElementKind::Mesh, static_cast<u32>(d)));
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
                                         ElementRef(ElementKind::Mesh, static_cast<u32>(d)));
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
                                    u32 targetVersion, const M3ExportSettings& settings,
                                    M3ExportMap* map) const {
    Result<m3::Model> result;
    if (!checkExportProfile(document, profile, result.diagnostics)) {
        return result;
    }
    checkRigConvention(document, profile, result.diagnostics);
    checkNodeKinds(document, profile, result.diagnostics);

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
    // `collisionBounds` goes with the collision mesh, and a model that carries
    // no `collisionVerts` states no volume for it: all 2,448 shipped v29 models
    // leave it zero, this converter emits no collision mesh, and copying the
    // render bounds there claimed one that does not exist.
    const ProfileMaterialSet* set = model.setFor(profile);
    // Only when the bag actually carried it. A Warcraft III source reaches here
    // with a StarCraft II set the material pass built and no `modelFlags` in
    // it, and reading the absent key as 0 wiped the struct's own default.
    if (set != nullptr && set->native.find("modelFlags") != nullptr) {
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
    // What each node keys, for the Warcraft III carriers: a node whose
    // transform or visibility moves needs a bone of its own to move, and an
    // identity attachment that rides its parent's bone would otherwise take its
    // keys nowhere. A visibility that rests hidden counts -- it has to hide
    // something.
    const std::size_t nodeCount = model.nodes.size();
    std::vector<u8> keyedTransform(nodeCount, 0);
    std::vector<u8> keyedVisibility(nodeCount, 0);
    if (settings.effectNodeBones) {
        for (const AnimChannel& channel : model.animChannels.channels) {
            if (channel.target.kind != TrackTarget::Kind::Node ||
                channel.target.node >= nodeCount) {
                continue;
            }
            switch (channel.target.channel) {
            case Channel::Translation:
            case Channel::Rotation:
            case Channel::Scale:
                keyedTransform[channel.target.node] = 1;
                break;
            case Channel::Visibility:
                keyedVisibility[channel.target.node] = 1;
                break;
            default:
                break;
            }
        }
        for (u32 n = 0; n < nodeCount; ++n) {
            if (VisibilityRest(model, n) <= 0.5f) {
                keyedVisibility[n] = 1;
            }
        }
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
            carries = !identityLocal(node) ||
                      (settings.effectNodeBones && (keyedTransform[n] || keyedVisibility[n]));
            break;
        // A StarCraft II emitter record names a bone like those three do, and
        // the import puts it under that bone at identity. A Warcraft III
        // emitter crossed into one is its bone (§2.3), unless it is a second
        // record of its parent's system: at identity, keying nothing, drawn
        // only while the parent is (`cross/wc3_sc2_emitters`).
        case NodeKind::Sc2ParticleEmitter:
        case NodeKind::Sc2RibbonEmitter:
            carries = !identityLocal(node) ||
                      (settings.effectNodeBones &&
                       (keyedTransform[n] || keyedVisibility[n] ||
                        node.native.find(kNodeSharesParentVisibility) == nullptr));
            break;
        case NodeKind::ParticleEmitter:
        case NodeKind::RibbonEmitter:
        case NodeKind::Wc3ParticleEmitter1:
        case NodeKind::Wc3ParticleEmitter2:
        case NodeKind::Wc3RibbonEmitter:
        case NodeKind::CollisionShape:
            // An emitter record names a bone, and a Warcraft III emitter moves
            // and hides on its own node: the bone is the node (§2.3). So does
            // the hit test a collision shape crosses as (C8.2).
            carries = settings.effectNodeBones;
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
        // A `ModelSpace` node's local is its world (`NodeTree::worldBind` stops
        // there, and so does the IREF below): under a parent bone it would be
        // composed a second time.
        const u32 parentBone =
            hasFlag(node.flags, NodeFlags::ModelSpace) ? 0xFFFFu : nearestBone(n);
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
    // The visibility leaves (§2.3). A StarCraft II bone that is hidden hides
    // everything under it; a Warcraft III node's visibility hides that node
    // alone -- an attachment's children, an emitter's child emitters, keep
    // drawing. So a node that keys its visibility AND has children keeps its
    // transform on its own bone and moves the visibility onto an identity
    // child that nothing else hangs off: the shape of Blizzard's own
    // conversions, transform on the `_OffsetDummy`, visibility on the leaf.
    std::vector<u32> visBoneOf(nodeCount, 0xFFFFu);
    if (settings.effectNodeBones) {
        std::vector<u8> hasChildren(nodeCount, 0);
        for (u32 n = 0; n < nodeCount; ++n) {
            const Node& child = model.nodes.nodes[n];
            if (child.parent != kInvalidNode && child.parent < nodeCount &&
                child.native.find(kNodeSharesParentVisibility) == nullptr) {
                hasChildren[child.parent] = 1;
            }
        }
        for (const u32 n : topological) {
            const Node& node = model.nodes.nodes[n];
            const bool ownVisibility = node.kind == NodeKind::Attachment ||
                                       node.kind == NodeKind::Light || IsEmitterKind(node.kind);
            if (!ownVisibility || boneOf[n] == 0xFFFFu || !keyedVisibility[n] || !hasChildren[n]) {
                continue;
            }
            m3::Bone leaf;
            leaf.name = node.name + "_Vis";
            leaf.parentIndex = static_cast<u16>(boneOf[n]);
            leaf.rotation.initValue = Quaternion{0, 0, 0, 1};
            leaf.scale.initValue = Vector3f{1, 1, 1};
            leaf.visibility.initValue = out.bones[boneOf[n]].visibility.initValue;
            // The owner rests visible: its children are not what hides.
            out.bones[boneOf[n]].visibility.initValue = 1u;
            visBoneOf[n] = static_cast<u32>(out.bones.size());
            out.bones.push_back(std::move(leaf));
            // An identity child binds where its owner does.
            out.initialReference.push_back(out.initialReference[boneOf[n]]);
        }
        animContext.nodeBone.assign(nodeCount, kInvalidIndex);
        animContext.nodeVisBone.assign(nodeCount, kInvalidIndex);
        for (u32 n = 0; n < nodeCount; ++n) {
            if (boneOf[n] == 0xFFFFu) {
                continue;
            }
            animContext.nodeBone[n] = boneOf[n];
            animContext.nodeVisBone[n] = visBoneOf[n] != 0xFFFFu ? visBoneOf[n] : boneOf[n];
        }
    }
    out.skinBoneCount = static_cast<u32>(out.bones.size());

    // --- billboards (WC3_TO_SC2_COMPLETION_PLAN.md §4.2) ---------------------
    //
    // A Warcraft III node states which way it billboards in its flags; a
    // StarCraft II bone states it in a BBSC record. Both evaluate in model
    // space, before the children compose onto the result, and both aim from
    // the node at the eye -- Warcraft III takes `camera - pivot`, so every
    // record asks `cameraLookAt`, not the view direction Blizzard's own
    // conversions mostly chose.
    //
    // The axes cross through the basis change: Warcraft III's +X (the facing
    // axis) is StarCraft II's -Y and its +Y is +X, so a node locked to its Y
    // is a bone locked to X (type 0) and one locked to its X is a bone locked
    // to Y (type 1). Measured in `wc3_sc2_equivalence_test` (E1), which also
    // found that Warcraft III's frame for a lock on X is left-handed: no
    // rotation reproduces it, and type 1 unturned is the one that keeps the
    // facing and standing axes, mirroring the locked one. The locked types
    // hold the MODEL's axis where Warcraft III holds the node's own current
    // one; the two agree while nothing above the node turns it, and a turning
    // ancestor is reported.
    //
    // Only for a Warcraft III document: the `.m2` importer sets the same bits
    // with its own semantics, and an `.m3` source carries no NodeFlags at all.
    const bool warcraftNodes = document.defaultProfile == ProfileId::Wc3Classic ||
                               document.defaultProfile == ProfileId::Wc3Reforged;
    if (warcraftNodes) {
        constexpr i64 kCameraAnchored = 0x80;
        const auto rotates = [&](u32 n) {
            for (u32 p = n; p != kInvalidNode && p < nodeCount; p = model.nodes.nodes[p].parent) {
                for (const AnimChannel& channel : model.animChannels.channels) {
                    if (channel.target.kind == TrackTarget::Kind::Node && channel.target.node == p &&
                        channel.target.channel == Channel::Rotation) {
                        return true;
                    }
                }
            }
            return false;
        };
        for (const u32 n : topological) {
            const Node& node = model.nodes.nodes[n];
            const bool full = hasFlag(node.flags, NodeFlags::Billboarded);
            const bool lockX = hasFlag(node.flags, NodeFlags::BillboardLockX);
            const bool lockY = hasFlag(node.flags, NodeFlags::BillboardLockY);
            const bool lockZ = hasFlag(node.flags, NodeFlags::BillboardLockZ);
            const bool anchored = (node.native.value("mdxFlagBits", 0) & kCameraAnchored) != 0;
            if (!full && !lockX && !lockY && !lockZ) {
                if (anchored) {
                    diagnostics.warn(DiagCode::FeatureDropped,
                                     "node '" + node.name +
                                         "' is camera-anchored without a billboard; StarCraft II "
                                         "has no spelling for the pivot sliding toward the eye",
                                     ElementRef(ElementKind::Node, n), profile);
                }
                continue;
            }
            if (boneOf[n] == 0xFFFFu) {
                continue;
            }
            m3::BillboardBehavior record;
            record.boneIndex = static_cast<u16>(boneOf[n]);
            record.cameraLookAt = 1;
            record.up = Quaternion{0, 0, 0, 1};
            record.forward = Quaternion{0, 0, 0, 1};
            // Warcraft III tests the free billboard first, then X, Y and Z.
            if (full) {
                record.billboardType = 6;
            } else if (lockX) {
                record.billboardType = 1;
            } else if (lockY) {
                record.billboardType = 0;
            } else {
                record.billboardType = 2;
            }
            if ((lockX || lockY) && !full && rotates(n)) {
                diagnostics.warn(DiagCode::AnimTrackApproximated,
                                 "node '" + node.name +
                                     "' locks its billboard to its own axis, which something "
                                     "above it turns; the bone locks the model's",
                                 ElementRef(ElementKind::Node, n), profile);
            }
            if (anchored) {
                diagnostics.warn(DiagCode::FeatureDropped,
                                 "node '" + node.name +
                                     "' is camera-anchored; the billboard crosses and the pivot "
                                     "sliding toward the eye does not",
                                 ElementRef(ElementKind::Node, n), profile);
            }
            out.billboardBehaviors.push_back(std::move(record));
        }
    }

    // The emitter systems' record numbers, first, because they name one another
    // -- a trail, a collision spawn, a bounce's ribbon, a copy's source -- and a
    // node's number has to be known before the record that names it is
    // written. Node order is record order, which keeps a copy's slot.
    // A copy is numbered only when its source is a particle emitter that gets
    // a `PAR_` of its own, so a copy of nothing leaves no hole in `PARC`.
    std::vector<u32> particleOf(nodeCount, kInvalidIndex);
    std::vector<u32> copyRecordOf(nodeCount, kInvalidIndex);
    std::vector<u32> ribbonOf(nodeCount, kInvalidIndex);
    std::vector<u32> recordBone(nodeCount, kInvalidIndex);
    // The bone a node's records ride: its visibility leaf, its own bone, or --
    // for one drawn only while its parent is -- the bone that hides the
    // parent; the nearest ancestor's otherwise.
    const auto recordBoneOf = [&](std::size_t n) -> u32 {
        if (visBoneOf[n] != 0xFFFFu) {
            return visBoneOf[n];
        }
        if (boneOf[n] != 0xFFFFu) {
            return boneOf[n];
        }
        const Node& node = model.nodes.nodes[n];
        if (node.native.find(kNodeSharesParentVisibility) != nullptr && node.parent < nodeCount &&
            visBoneOf[node.parent] != 0xFFFFu) {
            return visBoneOf[node.parent];
        }
        return nearestBone(n);
    };
    const auto particleAt = [&](std::size_t n) -> const Sc2ParticleEmitterPayload* {
        const Node& node = model.nodes.nodes[n];
        return node.kind == NodeKind::Sc2ParticleEmitter
                   ? std::get_if<Sc2ParticleEmitterPayload>(&node.payload)
                   : nullptr;
    };
    u32 copyCount = 0;
    {
        u32 particles = 0;
        u32 ribbons = 0;
        for (std::size_t n = 0; n < nodeCount; ++n) {
            const Node& node = model.nodes.nodes[n];
            const u32 bone = recordBoneOf(n);
            recordBone[n] = bone == 0xFFFFu ? kInvalidIndex : bone;
            if (const auto* particle = particleAt(n); particle != nullptr && !particle->isCopy()) {
                particleOf[n] = particles++;
            } else if (node.kind == NodeKind::Sc2RibbonEmitter &&
                       std::holds_alternative<Sc2RibbonEmitterPayload>(node.payload)) {
                ribbonOf[n] = ribbons++;
            }
        }
        for (std::size_t n = 0; n < nodeCount; ++n) {
            const auto* particle = particleAt(n);
            if (particle != nullptr && particle->isCopy() && particle->copyOf < nodeCount &&
                particleOf[particle->copyOf] != kInvalidIndex) {
                copyRecordOf[n] = copyCount++;
            }
        }
    }
    out.particleEmitterCopies.resize(copyCount);
    m3_emitters::ExportLinks emitterLinks;
    emitterLinks.particleOf = &particleOf;
    emitterLinks.ribbonOf = &ribbonOf;
    emitterLinks.boneOf = &recordBone;

    for (std::size_t n = 0; n < model.nodes.size(); ++n) {
        const Node& node = model.nodes.nodes[n];
        // The node's own bone when the pass above gave it one (it carries a
        // rest offset), the nearest ancestor's otherwise -- and its visibility
        // leaf over both, since a record there hides with the node.
        const u32 carrier = boneOf[n] != 0xFFFFu ? boneOf[n] : nearestBone(n);
        const u32 parentBone = recordBoneOf(n);
        if (map != nullptr) {
            map->nodeBone.resize(nodeCount, kInvalidIndex);
            map->nodeVisBone.resize(nodeCount, kInvalidIndex);
            map->nodeBone[n] = carrier == 0xFFFFu ? kInvalidIndex : carrier;
            map->nodeVisBone[n] = parentBone == 0xFFFFu ? kInvalidIndex : parentBone;
        }
        switch (node.kind) {
        case NodeKind::Attachment: {
            // An ATT_ has no model slot: what a Warcraft III point spawns is the
            // game's to attach, and nothing in the file can name it.
            if (const auto* payload = std::get_if<AttachmentPayload>(&node.payload);
                payload != nullptr && !payload->asset.path.empty()) {
                diagnostics.info(DiagCode::FeatureDropped,
                                 "attachment '" + node.name + "' spawns '" + payload->asset.path +
                                     "'; an ATT_ names no model, so it is not attached",
                                 ElementRef(ElementKind::Node, static_cast<u32>(n)), profile);
            }
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
            // Every shipped light states LightOpaque (0x08 on 1,440 of 1,440,
            // Blizzard's two War3_* lights included), so a light no `.m3`
            // authored states it too.
            light.flags = static_cast<m3::LightFlag>(
                node.native.find("m3LightFlags") != nullptr
                    ? static_cast<u32>(node.native.value("m3LightFlags"))
                    : static_cast<u32>(m3::LightFlag::LightOpaque));
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
                                     ElementRef(ElementKind::Node, static_cast<u32>(n)), profile);
                    [[fallthrough]];
                default:
                    light.lightType = m3::LightType::Omni;
                    break;
                }
                light.diffuseColor.initValue = payload->color;
                light.intensityMultiplier.initValue = payload->intensity;
                light.attenuationStart.initValue = payload->attenuationStart;
                light.attenuationEnd = payload->attenuationEnd;
                // The far attenuation's AnimRef rests at the same distance: the
                // struct's `decay` is that AnimRef, not an exponent -- its init
                // equals the plain float on 1,947 of 2,001 sampled shipped
                // lights, the two histograms identical. Left at zero it stated
                // a light that reaches nowhere.
                light.decay.initValue = payload->attenuationEnd;
                light.decay.nullValue = payload->attenuationEnd;
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
        case NodeKind::Sc2ParticleEmitter: {
            const auto* payload = std::get_if<Sc2ParticleEmitterPayload>(&node.payload);
            if (payload == nullptr) {
                break;
            }
            // The record's own properties are AnimRefs on it, so the slot is the
            // record's even where the node has a bone of its own: its
            // transform rides that bone through `nodeBone` like a light's.
            if (payload->isCopy()) {
                if (copyRecordOf[n] == kInvalidIndex) {
                    diagnostics.warn(DiagCode::DanglingNodeReference,
                                     "particle copy '" + node.name +
                                         "' copies no particle emitter; not written",
                                     ElementRef(ElementKind::Node, static_cast<u32>(n)), profile);
                    break;
                }
                m3::ParticleEmitterCopy copy = m3_emitters::ExportCopy(*payload);
                copy.boneIndex = parentBone == 0xFFFFu ? 0u : parentBone;
                animContext.nodeSlots[n] = {m3_anim::ExportContext::Slot::ParticleCopy,
                                            copyRecordOf[n]};
                out.particleEmitterCopies[copyRecordOf[n]] = std::move(copy);
                break;
            }
            m3::ParticleEmitter emitter = m3_emitters::ExportParticle(*payload, emitterLinks);
            emitter.boneIndex = parentBone == 0xFFFFu ? 0u : parentBone;
            // A copy names its source, and the source lists its copies: every
            // node copying this one, in node order -- the slot order.
            for (std::size_t c = 0; c < nodeCount; ++c) {
                const auto* other = particleAt(c);
                if (other != nullptr && copyRecordOf[c] != kInvalidIndex && other->copyOf == n) {
                    emitter.copyIndices.push_back(copyRecordOf[c]);
                }
            }
            animContext.nodeSlots[n] = {m3_anim::ExportContext::Slot::ParticleEmitter,
                                        particleOf[n]};
            out.particleEmitters.push_back(std::move(emitter));
            break;
        }
        case NodeKind::Sc2RibbonEmitter: {
            const auto* payload = std::get_if<Sc2RibbonEmitterPayload>(&node.payload);
            if (payload == nullptr) {
                break;
            }
            m3::RibbonEmitter ribbon = m3_emitters::ExportRibbon(*payload, emitterLinks);
            ribbon.boneIndex = static_cast<u16>(parentBone == 0xFFFFu ? 0u : parentBone);
            animContext.nodeSlots[n] = {m3_anim::ExportContext::Slot::RibbonEmitter,
                                        ribbonOf[n]};
            out.ribbonEmitters.push_back(std::move(ribbon));
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
                             ElementRef(ElementKind::Slot, static_cast<u32>(slot)), profile);
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
    const SkinSkeleton skinSkeleton(model.nodes);
    skinSkeleton.describe(desc);

    // The vertex declaration follows the meshes, not a default: an `.m3` may
    // carry none, one or five UV sets and an optional colour, and writing the
    // one shape this converter used to assume both dropped sets a material
    // samples and invented one for a model that had none. The buffer is one
    // declaration for every mesh, so it takes the widest: a count that followed
    // the last mesh dropped the flowing light Zhao Yun's spear reads from its
    // one geoset with a second set, and every later geoset had one.
    const std::size_t maxUvSets =
        std::min<std::size_t>(Profile(profile).maxUvSets, M3VertexEncoder::kMaxUvSets);
    std::size_t uvCount = 0;
    bool hasColor = false;
    for (const Mesh& mesh : model.meshes) {
        for (std::size_t u = 0; u < maxUvSets; ++u) {
            if (mesh.attributes.has(geom::names::uv(static_cast<u32>(u)), geom::Domain::Halfedge) ||
                mesh.attributes.has(geom::names::uv(static_cast<u32>(u)), geom::Domain::Vertex)) {
                uvCount = std::max(uvCount, u + 1);
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

    // Every standard material a slot draws with: one, or a composite's
    // sections -- a geoset's tint or fade reaches every pass.
    const auto standardsOf = [&out](u32 slot) {
        std::vector<u32> targets;
        const m3::MaterialMap& map = out.materialMaps[slot];
        if (map.materialType == m3::MaterialType::Standard) {
            targets.push_back(map.materialIndex);
        } else if (map.materialType == m3::MaterialType::Composite &&
                   map.materialIndex < out.compositeMaterials.size()) {
            for (const m3::CompositeSection& section :
                 out.compositeMaterials[map.materialIndex].sections) {
                if (section.materialIndex < out.materialMaps.size() &&
                    out.materialMaps[section.materialIndex].materialType ==
                        m3::MaterialType::Standard) {
                    targets.push_back(out.materialMaps[section.materialIndex].materialIndex);
                }
            }
        }
        return targets;
    };
    // The static half of a Warcraft III geoset animation, and its colour keys.
    // The tint rides a Color-flag emissive layer on the Mod op, which the
    // shader multiplies into the lit colour (or, after an additive emissive,
    // into the emissive) -- where every one of the 604 static geoset colours
    // found in Blizzard's own conversions sits. A partial static alpha rides
    // the alpha carrier a fade would.
    //
    // A material belongs to every geoset that draws with it, and a geoset
    // animation to one geoset. Where the sections drawing a standard material
    // disagree on their tint or their fade, each other signature draws a copy of
    // the material and carries its own (52 of the 751 tinted shipped geosets,
    // and 44 geosets on 17 materials a fade would otherwise take whole --
    // GryphonRider's body under one fading geoset). A composite keeps the old
    // rule: its carrier is planted only where every section agrees.
    const auto tintOf = [&model](const MeshSection& section, u32 mesh, u32 sectionIndex) {
        std::string signature;
        for (const char* name : {"geosetColorR", "geosetColorG", "geosetColorB", "geosetAlpha"}) {
            const NativeBag::Entry* entry = section.native.find(name);
            signature += entry != nullptr ? std::to_string(entry->value) : std::string("-");
            signature += ',';
        }
        for (const AnimChannel& entry : model.animChannels.channels) {
            if (entry.target.kind == TrackTarget::Kind::Section && entry.target.mesh == mesh &&
                entry.target.sub == sectionIndex && entry.target.channel == Channel::Color) {
                signature += "keyed" + std::to_string(entry.id);
            }
        }
        return signature;
    };
    // A Warcraft III geoset "visibility" arrives as a Section ALPHA channel
    // (GEOA is an alpha track). Only a binary, step-interpolated one is a gate;
    // a fade needs a layer alpha and drawing it un-faded beats hiding it
    // outright.
    const auto binaryStep = [&document](u32 channelId) {
        bool any = false;
        for (const Clip& clip : document.clips) {
            for (const SubTrackContainer& container : clip.containers) {
                for (const SubTrack& track : container.subTracks) {
                    if (track.channel != channelId) {
                        continue;
                    }
                    any = true;
                    const f32* values = reinterpret_cast<const f32*>(track.values.data());
                    const std::size_t count = track.values.size() / sizeof(f32);
                    // A one-key track steps by definition -- the WoW hide idiom
                    // is one zero key, tagged with whatever interp the file
                    // happened to carry.
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
    const auto fadeOf = [&model, &binaryStep](u32 mesh, u32 sectionIndex) {
        std::string signature;
        for (const AnimChannel& entry : model.animChannels.channels) {
            if (entry.target.kind == TrackTarget::Kind::Section && entry.target.mesh == mesh &&
                entry.target.sub == sectionIndex && entry.target.channel == Channel::Alpha &&
                !binaryStep(entry.id)) {
                signature += "fade" + std::to_string(entry.id);
            }
        }
        return signature;
    };
    const bool warcraftMaterials = document.defaultProfile == ProfileId::Wc3Classic ||
                                   document.defaultProfile == ProfileId::Wc3Reforged;
    std::map<u32, std::string> slotTint;
    std::set<u32> slotTintShared;
    // The material map entry a section draws, where it is not its slot's.
    std::map<std::pair<u32, u32>, u32> sectionDrawSlot;
    std::map<std::pair<u32, std::string>, u32> slotCopy;
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        for (std::size_t s = 0; s < mesh.sections.size(); ++s) {
            const u32 slot = mesh.sections[s].materialSlot;
            const std::string signature =
                tintOf(mesh.sections[s], static_cast<u32>(m), static_cast<u32>(s)) +
                (warcraftMaterials ? fadeOf(static_cast<u32>(m), static_cast<u32>(s))
                                   : std::string());
            const auto [entry, fresh] = slotTint.emplace(slot, signature);
            if (fresh || entry->second == signature) {
                continue;
            }
            if (!warcraftMaterials || slot >= out.materialMaps.size() ||
                out.materialMaps[slot].materialType != m3::MaterialType::Standard ||
                out.materialMaps[slot].materialIndex >= out.standardMaterials.size()) {
                slotTintShared.insert(slot);
                continue;
            }
            const auto [copy, made] = slotCopy.emplace(std::make_pair(slot, signature), 0u);
            if (made) {
                m3::MaterialMap copyMap = out.materialMaps[slot];
                m3::StandardMaterial material = out.standardMaterials[copyMap.materialIndex];
                material.name +=
                    "_" + std::to_string(animContext.materialClones[slot].size() + 1);
                copyMap.materialIndex = static_cast<u32>(out.standardMaterials.size());
                out.standardMaterials.push_back(std::move(material));
                copy->second = static_cast<u32>(out.materialMaps.size());
                out.materialMaps.push_back(copyMap);
                std::vector<u32> ordinals = slot < animContext.materialOrdinals.size()
                                                ? animContext.materialOrdinals[slot]
                                                : std::vector<u32>{};
                animContext.materialOrdinals.push_back(std::move(ordinals));
                animContext.materialClones[slot].push_back(copy->second);
            }
            sectionDrawSlot[{static_cast<u32>(m), static_cast<u32>(s)}] = copy->second;
        }
    }
    std::set<std::pair<u32, u32>> tinted;
    // Geoset fades reported as dropped, once each however many batches draw them.
    std::set<u32> fadesDropped;
    const auto plantGeosetTint = [&](m3::Model& target, const MeshSection& section, u32 mesh,
                                     u32 sectionIndex, u32 slot, u16 gate) {
        const auto floatOf = [&section](const char* name, f32& value) {
            const NativeBag::Entry* entry = section.native.find(name);
            if (entry == nullptr) {
                return false;
            }
            value = std::bit_cast<f32>(static_cast<u32>(entry->value));
            return true;
        };
        Vector3f rest{1.0f, 1.0f, 1.0f};
        const bool staticColor = floatOf("geosetColorR", rest.x) &&
                                 floatOf("geosetColorG", rest.y) &&
                                 floatOf("geosetColorB", rest.z);
        f32 alpha = 1.0f;
        const bool staticAlpha = floatOf("geosetAlpha", alpha);
        const AnimChannel* keyedColor = nullptr;
        bool keyedAlpha = false;
        for (const AnimChannel& entry : model.animChannels.channels) {
            if (entry.target.kind != TrackTarget::Kind::Section || entry.target.mesh != mesh ||
                entry.target.sub != sectionIndex) {
                continue;
            }
            if (entry.target.channel == Channel::Color) {
                keyedColor = &entry;
            } else if (entry.target.channel == Channel::Alpha) {
                keyedAlpha = true;
            }
        }
        const bool tint = staticColor || keyedColor != nullptr;
        const bool fadeOnly = staticAlpha && !keyedAlpha &&
                              gate == static_cast<u16>(kSectionAlwaysDrawn);
        if ((!tint && !fadeOnly) || !tinted.emplace(mesh, sectionIndex).second) {
            return;
        }
        if (slotTintShared.count(slot) != 0) {
            diagnostics.warn(DiagCode::FeatureDropped,
                             "geoset '" + section.name +
                                 "' is tinted or partly transparent, and shares its composite "
                                 "material with a geoset that is not; neither is written",
                             ElementRef(ElementKind::Mesh, mesh), profile);
            return;
        }
        const auto byte = [](f32 unit) {
            return static_cast<u8>(std::clamp(unit, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        for (const u32 matIndex : standardsOf(slot)) {
            if (matIndex >= target.standardMaterials.size()) {
                continue;
            }
            m3::StandardMaterial& mat = target.standardMaterials[matIndex];
            if (tint) {
                u8 which = 0;
                if (!mat.emissiveLayer1.has_value()) {
                    which = 1;
                } else if (!mat.emissiveLayer2.has_value()) {
                    which = 2;
                }
                if (which == 0) {
                    diagnostics.warn(DiagCode::FeatureDropped,
                                     "geoset '" + section.name +
                                         "' is tinted, and its material has no free emissive "
                                         "layer to carry the tint",
                                     ElementRef(ElementKind::Mesh, mesh), profile);
                } else {
                    m3::TextureLayer carrier;
                    carrier.flags = m3::TextureLayerFlag::Color;
                    carrier.color.initValue =
                        m3::ColorBGRA{byte(rest.z), byte(rest.y), byte(rest.x), 255};
                    // The null stays 0. The engine writes a layer colour only
                    // when its init differs from its null or it animates, so a
                    // still tint restated as its null was never written and the
                    // Mod multiplied whatever the slot held: an additive tinted
                    // geoset vanished in the editor.
                    carrier.rgbMultiply.initValue = 1.0f;
                    carrier.rgbMultiply.nullValue = 1.0f;
                    carrier.mapAlpha.initValue = 1.0f;
                    carrier.mapAlpha.nullValue = 1.0f;
                    (which == 1 ? mat.emissiveLayer1 : mat.emissiveLayer2) = std::move(carrier);
                    (which == 1 ? mat.emissiveBlendMode1 : mat.emissiveBlendMode2) =
                        m3::LayerBlendOp::Mod;
                    if (keyedColor != nullptr) {
                        animContext.sectionColorLayers[keyedColor->id].emplace_back(matIndex,
                                                                                     which);
                    }
                }
            }
            if (fadeOnly) {
                u8 which = 0;
                if (!mat.alphaLayer1.has_value()) {
                    which = 1;
                } else if (!mat.alphaLayer2.has_value()) {
                    which = 2;
                }
                if (which == 0) {
                    diagnostics.warn(DiagCode::FeatureDropped,
                                     "geoset '" + section.name +
                                         "' is partly transparent, and its material has no free "
                                         "alpha layer to carry it",
                                     ElementRef(ElementKind::Mesh, mesh), profile);
                    continue;
                }
                m3::TextureLayer carrier;
                carrier.flags = m3::TextureLayerFlag::Color;
                carrier.color.initValue = m3::ColorBGRA{255, 255, 255, 255};
                carrier.rgbMultiply.initValue = 1.0f;
                // Null 1, like the tint's: a still map alpha at its null is
                // never written.
                carrier.mapAlpha.initValue = alpha;
                (which == 1 ? mat.alphaLayer1 : mat.alphaLayer2) = std::move(carrier);
            }
        }
    };

    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        const bool warcraftDocument = document.defaultProfile == ProfileId::Wc3Classic ||
                                      document.defaultProfile == ProfileId::Wc3Reforged;
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
            // A region carries ONE bone palette, and the vertex shader holds a
            // fixed number of matrix registers for it: no shipped v5 region
            // names more than 63 bones -- exactly 63 in 436 of the corpus's
            // 67,320 regions and 64 in none of them -- and a draw whose palette
            // overruns the registers fails with D3DERR_INVALIDCALL. The Galaxy
            // editor answers that by queueing a device reset, so it recurs every
            // frame and the viewport stays black with the model loaded and no
            // complaint made about the model. A range whose skin needs more
            // bones is split into as many regions as it takes; they share the
            // material, so each gets its own batch naming it.
            const u32 paletteLimit = Profile(profile).maxBonesPerPalette;

            // The distinct bones one vertex skins to. A corner names at most
            // four, so a triangle names at most twelve, and no profile states a
            // cap under twelve -- one would be unsatisfiable.
            const auto bonesOf = [&](u32 source, std::array<u32, 4>& slots) -> std::size_t {
                std::size_t n = 0;
                for (std::size_t k = 0; k < 4; ++k) {
                    if (source >= boneIndices.size() || source >= boneWeights.size() ||
                        boneWeights[source][k] <= 0.0f) {
                        continue;
                    }
                    const u32 node = boneIndices[source][k];
                    const u32 bone = node < boneOf.size() ? boneOf[node] : 0xFFFFu;
                    if (bone != 0xFFFFu &&
                        std::find(slots.begin(), slots.begin() + n, bone) == slots.begin() + n) {
                        slots[n++] = bone;
                    }
                }
                return n;
            };

            // Whole triangles, in order, so every run is a contiguous slice of
            // the face buffer -- which is all a region's `firstIndex` can state.
            std::vector<std::pair<u32, u32>> runs;
            {
                const u32 corners = range.indexCount - range.indexCount % 3;
                std::vector<u32> window;
                std::vector<u32> adding;
                u32 first = 0;
                for (u32 i = 0; i < corners; i += 3) {
                    // Twice at most: once against the running window, and again
                    // against an empty one after the run is closed.
                    for (int pass = 0; pass < 2; ++pass) {
                        adding.clear();
                        for (u32 c = 0; c < 3; ++c) {
                            const u32 index = render.indices[range.firstIndex + i + c];
                            std::array<u32, 4> slots{};
                            const std::size_t n =
                                index < positions.size() ? bonesOf(index, slots) : 0;
                            for (std::size_t s = 0; s < n; ++s) {
                                if (std::find(window.begin(), window.end(), slots[s]) ==
                                        window.end() &&
                                    std::find(adding.begin(), adding.end(), slots[s]) ==
                                        adding.end()) {
                                    adding.push_back(slots[s]);
                                }
                            }
                        }
                        if (window.empty() || paletteLimit == 0 ||
                            window.size() + adding.size() <= paletteLimit) {
                            break;
                        }
                        runs.emplace_back(first, i - first);
                        first = i;
                        window.clear();
                    }
                    window.insert(window.end(), adding.begin(), adding.end());
                }
                // The tail, and with it any corners a range short of a whole
                // triangle left over: they stay on the last run, not dropped.
                runs.emplace_back(first, range.indexCount - first);
            }

            const std::size_t firstRegion = division.regions.size();
            for (const std::pair<u32, u32>& run : runs) {
                m3::Region region;
                region.index = static_cast<u32>(division.regions.size());
                region.firstVertex = static_cast<u32>(writtenVertices);
                region.firstIndex = static_cast<u32>(division.faces.size());
                region.indexCount = run.second;
                // `M3VertexEncoder` writes `uv * 2048`, so the pair that reads it
                // back is the stock one. Stated rather than left at the struct's
                // uninitialised float, which a v5 reader would take literally --
                // and the region says it is a v5 record, or nothing reads them.
                region.setVersion(5);
                region.uvScale = 16.0f;
                region.uvOffset = 0.0f;
                region.firstBoneLookup = static_cast<u16>(out.boneLookup.size());

                bool carriedRoot = false;
                if (range.section < mesh.sections.size()) {
                    const MeshSection& section = mesh.sections[range.section];
                    carriedRoot = section.native.find("rootBone") != nullptr;
                    region.rootBone = static_cast<u16>(section.native.value("rootBone"));
                    region.flags = static_cast<m3::RegionFlag>(
                        static_cast<u32>(section.native.value(
                            "regionFlags", hasFlag(section.flags, SectionFlags::Hidden)
                                               ? static_cast<i64>(m3::RegionFlag::Hidden)
                                               : 0)));
                }

                // A region owns its vertices. Its faces index them from its own
                // `firstVertex`, and -- the reason it must own them -- a vertex's
                // four bone indices are slots in *this* region's bone-lookup
                // window, so the same vertex shared by two regions with different
                // windows could not be encoded once. Regions therefore get
                // disjoint slices of the model's one buffer, exactly as a shipped
                // `.m3` has them, and a vertex two regions use is written twice.
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
                for (u32 i = 0; i < run.second; ++i) {
                    const u32 index = render.indices[range.firstIndex + run.first + i];
                    if (index >= localOf.size()) {
                        diagnostics.warn(
                            DiagCode::IndexOutOfRange, "face corner past the mesh's vertex buffer",
                            ElementRef(ElementKind::Mesh, static_cast<u32>(m)), profile);
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
                                     ElementRef(ElementKind::Mesh, static_cast<u32>(m)), profile);
                }

                bool rigid = true;
                for (u32 source : sourceOf) {
                    // Packed into the leading slots -- an influence on a node that is not
                    // a bone leaves no gap -- so a one-bone vertex names its bone in slot 0.
                    std::array<std::pair<f32, u8>, 4> influences{};
                    std::size_t count = 0;
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
                        influences[count++] = {std::clamp(boneWeights[source][k], 0.0f, 1.0f),
                                               slotFor(bone)};
                    }
                    std::array<f32, 4> shares{0, 0, 0, 0};
                    std::array<u8, 4> indices{0, 0, 0, 0};
                    for (std::size_t k = 0; k < count; ++k) {
                        shares[k] = influences[k].first;
                        indices[k] = influences[k].second;
                    }
                    const std::array<u8, 4> weights = QuantizeWeights(shares, count);
                    rigid = rigid && count <= 1;
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
                // One weight pair where no vertex blends: all 730 shipped regions whose
                // vertices each follow one bone say 1, blended ones 4.
                region.boneWeightPairs = rigid ? 1 : 4;
                // The index pairs follow the weight pairs: 829 of 840 shipped regions
                // state the two equal, 1/1 rigid and 4/4 blended.
                region.boneIndexPairs = region.boneWeightPairs;
                writtenVertices += sourceOf.size();

                // The split above is what keeps this quiet; it fires only for a
                // profile whose cap a single triangle could already break.
                if (paletteLimit != 0 && window.size() > paletteLimit) {
                    diagnostics.warn(DiagCode::BonePaletteLimit,
                                     "region needs " + std::to_string(window.size()) +
                                         " bones, past the profile's " +
                                         std::to_string(paletteLimit),
                                     ElementRef(ElementKind::Mesh, static_cast<u32>(m)), profile);
                }
                for (u32 bone : window) {
                    out.boneLookup.push_back(static_cast<u16>(bone));
                }
                region.boneLookupCount = static_cast<u16>(window.size());
                // Every one of 840 shipped regions names its FIRST lookup bone here,
                // rigid and blended alike; a source with no region record of its own
                // (an .mdx) reached the engine naming bone 0 on all of them. A native
                // record keeps what it carried.
                if (!carriedRoot && !window.empty()) {
                    region.rootBone = static_cast<u16>(window.front());
                }
                // 874 of 874 shipped regions repeat the lookup count here.
                region.unknown2 = region.boneLookupCount;
                division.regions.push_back(std::move(region));
            }
            const std::size_t regionCount = division.regions.size() - firstRegion;

            const auto drawn = sectionDrawSlot.find({static_cast<u32>(m), range.section});
            const u32 drawSlot = drawn != sectionDrawSlot.end() ? drawn->second : range.materialSlot;
            m3::Batch batch;
            batch.regionIndex = static_cast<u16>(firstRegion);
            batch.materialIndex = static_cast<u16>(drawSlot);
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
                // A binary step gates (`binaryStep`, above); a fade rides the
                // material.
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
                if (vis == nullptr && fade != nullptr && drawSlot < out.materialMaps.size() &&
                    animContext.sectionAlphaLayers.find(fade->id) ==
                        animContext.sectionAlphaLayers.end()) {
                    // Every standard material the slot draws with -- one, or
                    // a composite's sections: a geoset fade fades every pass.
                    std::vector<u32> targets;
                    const m3::MaterialMap& drawMap = out.materialMaps[drawSlot];
                    if (drawMap.materialType == m3::MaterialType::Standard) {
                        targets.push_back(drawMap.materialIndex);
                    } else if (drawMap.materialType == m3::MaterialType::Composite &&
                               drawMap.materialIndex < out.compositeMaterials.size()) {
                        for (const m3::CompositeSection& section :
                             out.compositeMaterials[drawMap.materialIndex].sections) {
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
                            if (!fadesDropped.insert(fade->id).second) {
                                continue;
                            }
                            diagnostics.warn(DiagCode::AnimTrackDropped,
                                             "a geoset fade found both alpha layers taken",
                                             ElementRef(ElementKind::Channel, fade->id), profile);
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
            // A Warcraft III geoset's tint -- GEOA's static colour or KGAC -- and
            // its static partial alpha (WC3_TO_SC2_COMPLETION_PLAN.md C2.3-C2.4).
            // Warcraft III multiplies both into every pass of the geoset.
            if (warcraftDocument && range.section < mesh.sections.size() &&
                drawSlot < out.materialMaps.size()) {
                plantGeosetTint(out, mesh.sections[range.section], static_cast<u32>(m),
                                range.section, drawSlot, batch.boneCount);
            }
            // One batch per region the range became: a split changed how many
            // draws the material takes, not which material it is.
            for (std::size_t r = 0; r < regionCount; ++r) {
                m3::Batch copy = batch;
                copy.regionIndex = static_cast<u16>(firstRegion + r);
                division.batches.push_back(copy);
            }
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
    // A division states how many copies of itself the client draws, and 0 is
    // not "one": 5,502 of 5,502 shipped divisions say 1, and the Galaxy editor
    // reading a 0 builds no index buffer for the model -- it logs
    // "Index buffer not set" once per batch per frame and fails the draw with
    // D3DERR_INVALIDCALL, so the mesh is there, bound to nothing, and the
    // viewport stays black.
    division.instances = 1;
    out.divisions.push_back(std::move(division));

    // Last, because the gate bones the material pass raises are bones too.
    // `nullValue` is what a property rests at in a sequence whose `STC_` does
    // not name its `animId`, and it is a constant per property: all 736,433
    // bones in the corpus state these four and nothing else, so restating them
    // over a parsed model changes nothing. The struct's own default is zero --
    // a null rotation and a zero scale, which collapse whatever the bone skins
    // the moment one clip leaves it out.
    for (m3::Bone& bone : out.bones) {
        bone.position.nullValue = Vector3f{0, 0, 0};
        bone.rotation.nullValue = Quaternion{0, 0, 0, 1};
        bone.scale.nullValue = Vector3f{1, 1, 1};
        bone.visibility.nullValue = 1u;
    }

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
    // What every shipped declaration states besides its layout: 0x01800061 is
    // set in all 5,510 corpus models and 0 of them omit any of its bits, so a
    // source with nothing to say about the format (anything but an `.m3`) is
    // given it rather than left declaring only its UV sets.
    constexpr u32 kBaseVertexFlags = 0x01800061u;
    u32 vertexFlags = kBaseVertexFlags;
    if (set != nullptr) {
        vertexFlags |= static_cast<u32>(set->native.value("vertexFlags", 0)) & ~kLayoutBits;
    }
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
    m3_anim::Export(document, 0, animContext, out, diagnostics, map);
    if (map != nullptr) {
        // Past every id a stream or AnimRef in the file names, the fixed ones
        // included, so a caller's new streams join nothing already there.
        u32 highest = kModelBoundsAnimId;
        for (const m3::SubTrackContainer& stc : out.subTrackCollections) {
            for (const u32 id : stc.animIds) {
                highest = (std::max)(highest, id);
            }
        }
        for (const AnimChannel& channel : model.animChannels.channels) {
            highest = (std::max)(highest, channel.id);
        }
        map->nextAnimId = highest + 1;
    }

    PlaceSectionsFirst(out);

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
