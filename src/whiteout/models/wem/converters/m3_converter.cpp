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

#include "whiteout/models/m3/parser.h"
#include "whiteout/models/m3/writer.h"
#include "whiteout/models/wem/converters.h"
#include "whiteout/models/wem/geometry/builder.h"
#include "whiteout/models/wem/geometry/render_view.h"

#include "../materials/m3_core.h"
#include "m3_anim.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace whiteout {
namespace models {
namespace wem {

namespace {

constexpr ProfileId kM3Profiles[] = {ProfileId::Sc2, ProfileId::Heroes};

/// The first `MODL` version whose loader builds `MADD` (§7.2.6).
constexpr u32 kHeroesModlVersion = 30;

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
    return out;
}

/// M3 spells the inherit bits positively; WEM (and MDX) spell them as opt-outs.
NodeFlags ToNodeFlags(m3::BoneFlag source) {
    NodeFlags out = NodeFlags::None;
    if (!hasFlag(source, m3::BoneFlag::InheritTranslation)) {
        out |= NodeFlags::DontInheritTranslation;
    }
    if (!hasFlag(source, m3::BoneFlag::InheritScale)) {
        out |= NodeFlags::DontInheritScale;
    }
    if (!hasFlag(source, m3::BoneFlag::InheritRotation)) {
        out |= NodeFlags::DontInheritRotation;
    }
    // The two `Billboard` bits are deliberately not mapped: they are set on no
    // bone in the whole corpus, and billboarding comes from `BBSC`.
    return out;
}

m3::BoneFlag FromNodeFlags(NodeFlags source, u32 rawFallback) {
    u32 bits = rawFallback;
    const auto set = [&bits](m3::BoneFlag bit, bool on) {
        if (on) {
            bits |= static_cast<u32>(bit);
        } else {
            bits &= ~static_cast<u32>(bit);
        }
    };
    set(m3::BoneFlag::InheritTranslation, !hasFlag(source, NodeFlags::DontInheritTranslation));
    set(m3::BoneFlag::InheritScale, !hasFlag(source, NodeFlags::DontInheritScale));
    set(m3::BoneFlag::InheritRotation, !hasFlag(source, NodeFlags::DontInheritRotation));
    return static_cast<m3::BoneFlag>(bits);
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
        node.native.set("flagBits", static_cast<i64>(static_cast<u32>(bone.flags)));

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
        node.native.set("lightFlags", static_cast<i64>(static_cast<u32>(light.flags)));
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
    std::vector<std::vector<Vector2f>> uvSets;
    for (std::size_t u = 0; u < uvCount; ++u) {
        uvSets.push_back(source.vertices.getUVs(u));
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
            if (hasFlag(region.flags, m3::RegionFlag::Hidden)) {
                section.flags = SectionFlags::Hidden;
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
                section.native.set("visibilityBone", static_cast<i64>(record.boneCount));
            }
            sectionOfRegion[r] = builder.addSection(std::move(section));
        }

        for (std::size_t r = 0; r < division.regions.size(); ++r) {
            const m3::Region& region = division.regions[r];
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
                            builder.setCornerAttr(face, c, geom::names::uv(static_cast<u32>(u)),
                                                  uvSets[u][global]);
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
                                    u32 targetVersion) const {
    Result<m3::Model> result;
    if (!checkExportProfile(document, profile, result.diagnostics)) {
        return result;
    }
    checkRigConvention(document, profile, result.diagnostics);

    Diagnostics& diagnostics = result.diagnostics;
    m3::Model out;
    out.setVersion(static_cast<i32>(targetVersion));
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
    std::vector<u32> boneOf(model.nodes.size(), 0xFFFFu);
    for (std::size_t n = 0; n < model.nodes.size(); ++n) {
        if (model.nodes.nodes[n].kind != NodeKind::Bone) {
            continue;
        }
        const Node& node = model.nodes.nodes[n];
        boneOf[n] = static_cast<u32>(out.bones.size());
        animContext.nodeSlots[n] = {m3_anim::ExportContext::Slot::Bone,
                                    static_cast<u32>(out.bones.size())};
        m3::Bone bone;
        bone.name = node.name;
        bone.flags = FromNodeFlags(node.flags, static_cast<u32>(node.native.value("flagBits")));
        bone.parentIndex = 0xFFFFu;
        if (node.parent != kInvalidNode && node.parent < boneOf.size() &&
            boneOf[node.parent] != 0xFFFFu) {
            bone.parentIndex = static_cast<u16>(boneOf[node.parent]);
        }
        bone.position.initValue = Unrebase(node.local.translation);
        const Quaternion& rotation = node.local.rotation;
        bone.rotation.initValue = Quaternion{rotation.y, -rotation.x, rotation.z, rotation.w};
        bone.scale.initValue = node.local.scale;
        out.bones.push_back(std::move(bone));

        // IREF, one matrix per bone and in the same order. `poseMatrixOf`
        // answers with the stored matrix when the document carries one and
        // otherwise composes the schema's own derivation -- the inverse of the
        // composed rest chain -- which is the only answer a document from
        // another format can give. Without this the renderer binds every bone
        // as identity and the skin explodes.
        m3::InitialReference reference;
        reference.matrix = UnrebaseMatrix(model.nodes.inverseBindMatrix(static_cast<u32>(n)));
        out.initialReference.push_back(reference);
    }
    out.skinBoneCount = static_cast<u32>(out.bones.size());

    for (std::size_t n = 0; n < model.nodes.size(); ++n) {
        const Node& node = model.nodes.nodes[n];
        const u32 parentBone =
            node.parent != kInvalidNode && node.parent < boneOf.size() ? boneOf[node.parent] : 0u;
        switch (node.kind) {
        case NodeKind::Attachment: {
            m3::AttachmentPoint point;
            point.name = node.name;
            point.boneIndex = parentBone == 0xFFFFu ? 0u : parentBone;
            animContext.nodeSlots[n] = {m3_anim::ExportContext::Slot::Attachment,
                                        static_cast<u32>(out.attachmentPoints.size())};
            out.attachmentPoints.push_back(std::move(point));
            break;
        }
        case NodeKind::Light: {
            m3::Light light;
            light.boneIndex = static_cast<u16>(parentBone == 0xFFFFu ? 0u : parentBone);
            light.flags =
                static_cast<m3::LightFlag>(static_cast<u32>(node.native.value("lightFlags")));
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
    for (const TextureRef& ref : document.textures) {
        context.texturesByPath.emplace_back(ref.path,
                                            static_cast<u32>(context.texturesByPath.size()));
    }
    // One map entry per slot, so a batch's `materialIndex` is the slot index.
    for (std::size_t slot = 0; slot < model.materialSlots.size(); ++slot) {
        const Material* material = Resolve(model, static_cast<u32>(slot), profile);
        if (material == nullptr) {
            out.materialMaps.push_back(m3::MaterialMap{});
            diagnostics.warn(DiagCode::SlotNotBound,
                             "slot " + model.materialSlots[slot] + " has no material",
                             ElementRef(ElementKind::Slot, slot), profile);
            continue;
        }
        out.materialMaps.push_back(
            m3_core::ExportMaterial(*material, profile, context, out, diagnostics));
    }

    // --- geometry -----------------------------------------------------------
    //
    // One division per mesh. Every division writes into the model's single
    // vertex buffer, so the regions' `firstVertex` runs are the concatenation
    // of the meshes'.
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

    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        const geom::RenderMesh render = geom::BuildRenderMesh(mesh, desc);
        diagnostics.append(render.diagnostics);

        m3::MeshDivision division;
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
            m3::Region region;
            region.index = static_cast<u32>(division.regions.size());
            region.firstVertex = static_cast<u32>(writtenVertices);
            region.firstIndex = static_cast<u32>(division.faces.size());
            region.indexCount = range.indexCount;
            region.boneWeightPairs = 4;
            region.boneIndexPairs = 4;
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
                    mesh.sections[range.section].native.value("visibilityBone", 0xFFFF));
            }
            division.regions.push_back(std::move(region));
            division.batches.push_back(batch);
        }

        out.divisions.push_back(std::move(division));
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
        toM3(document, profile, version == 0 ? defaultExportVersion() : version);
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
