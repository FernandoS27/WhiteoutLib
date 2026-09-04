// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file d3_converter.cpp
 * @brief `.acr` / `.app` -> `Document` (design §9, §10.7, §14).
 *
 * ### One actor, one model
 *
 * An `.app` is the *parts*: a character's ships every armour variant it could
 * ever wear at once, and which of them draw is decided by what the character has
 * equipped — data in assets the `.app` never names. So the actor is the unit, the
 * caller supplies the wardrobe, and every sub-object still imports: the choice is
 * `SectionFlags::Hidden`, never a drop, so re-dressing a loaded document is a
 * flag flip.
 *
 * ### The five poses are five different poses
 *
 * `BoneStructure` carries five `PRSTransform`s and they are not redundant:
 *
 *     tTransform0  model-space bind pose A     tTransform1  exact inverse of A
 *     tTransform2  LOCAL parent-relative bind  tTransform3  model-space bind pose B
 *     tTransform4  exact inverse of B
 *
 * A and B disagree on 202 of 2,525 bones across 400 sampled files, by up to 19.9
 * units on a ~7-unit character. Skinning wants B's inverse; a hardpoint composes
 * against **A's** inverse, because it is authored in the same space as A rather
 * than inside the bone. Collapsing the five into one `inverseBindMatrix` throws
 * that away, which is why §10.5 puts a pose *schema* on the tree.
 *
 * ### What this does not import
 *
 * The per-bone collision shapes and constraints — D3's ragdoll rig — and the
 * appearance's octree and collision capsules. They are a physics asset that
 * happens to ship inside a drawable, and WEM is a model format. The counts ride
 * `native` so nothing goes missing silently.
 *
 * A sub-object's `ClothStructure` goes the same way and for a different reason.
 * §18 lets cloth ride along **as a native block**, and D3's is per sub-object —
 * particles, staples and two constraint sets — which is the one scope WEM has
 * no typed native block for: a *material* has one per format (§7.3) and a
 * section has only the shared name/value bag. The section keeps
 * `SectionFlags::ClothSimulated`, so nothing about it is a guess on the way
 * back; it simply draws skinned, and the export reports how many did.
 */

#include "whiteout/models/wem/d3_converter.h"
#include "whiteout/models/wem/geometry/builder.h"
#include "whiteout/models/wem/geometry/render_view.h"

#include <whiteout/sno/d3/native/geometry.h>

#include "../materials/d3_core.h"
#include "../native/d3_copy.h"
#include "d3_anim.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

namespace d3n = whiteout::sno::d3::native;

namespace {

constexpr ProfileId kD3Profiles[] = {ProfileId::Diablo3};

/// The 16-byte SNO header: magic, version, and eight bytes that are zero on disk.
constexpr std::size_t kSnoHeaderSize = 16;
constexpr u32 kSnoMagic = 0xDEADBEEFu;
constexpr u32 kAppearanceVersion = 260;
constexpr u32 kActorVersion = 282;

/// `Appearance_GetDefaultLook`'s answer when nothing was equipped.
constexpr const char* kDefaultLookName = "A";

/// The trigger types that spawn something. A Particle or Actor payload only
/// ever rides one of these two, so reading the payload group without the trigger
/// is what turns a *stop* (type 7, 2,496 actor events) into a spawn.
constexpr i32 kTriggerSpawn = 0;
constexpr i32 kTriggerSpawnAttached = 25;

bool IEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

/// Whether @p event puts something on a hardpoint.
///
/// Three conditions, and dropping any one of them is a known way to get this
/// wrong: 813 shipped items are authored with `nChance == 0` and never fire at
/// all (a decision, not a die roll); a Particle or Actor payload only ever rides
/// trigger 0 or 25, so reading the group without the trigger turns a *stop* into
/// a spawn; and a payload handle of -1 is no payload.
bool IsSpawn(const d3n::TriggerEvent& event) {
    if ((event.tConditions.nChance & 0xFF) == 0) {
        return false;
    }
    if (event.eTriggerType != kTriggerSpawn && event.eTriggerType != kTriggerSpawnAttached) {
        return false;
    }
    if (event.tPayload.dwNameHandle == -1) {
        return false;
    }
    const i32 group = event.tPayload.eSnoGroup;
    return group == static_cast<i32>(d3n::Group::Actor) ||
           group == static_cast<i32>(d3n::Group::Particle);
}

/// Whether any attach point in @p model carries something.
bool HasAttachedModel(const Model& model) {
    for (const Node& node : model.nodes.nodes) {
        const auto* payload = std::get_if<AttachmentPayload>(&node.payload);
        if (payload != nullptr && (!payload->asset.empty() || payload->model != kInvalidIndex)) {
            return true;
        }
    }
    return false;
}

Transform ToTransform(const d3n::PRSTransform& source) {
    Transform out;
    out.translation = source.vTranslation;
    out.rotation =
        Quaternion{source.qRotation.x, source.qRotation.y, source.qRotation.z, source.qRotation.w};
    out.scale = Vector3f{source.flScale, source.flScale, source.flScale};
    return out;
}

Transform ToTransform(const d3n::PRTransform& source) {
    Transform out;
    out.translation = source.vTranslation;
    out.rotation =
        Quaternion{source.qRotation.x, source.qRotation.y, source.qRotation.z, source.qRotation.w};
    return out;
}

Extent ToExtent(const d3n::AABB& box) {
    Extent out;
    out.minimum = Vector3f{box.vCenter.x - box.vHalfExtent.x, box.vCenter.y - box.vHalfExtent.y,
                           box.vCenter.z - box.vHalfExtent.z};
    out.maximum = Vector3f{box.vCenter.x + box.vHalfExtent.x, box.vCenter.y + box.vHalfExtent.y,
                           box.vCenter.z + box.vHalfExtent.z};
    FinishExtent(out);
    return out;
}

Sphere ToSphere(const d3n::Sphere& source) {
    Sphere out;
    out.center = source.vCenter;
    out.radius = source.flRadius;
    return out;
}

// ============================================================================
// Nodes (§10.7)
// ============================================================================

/// The five-entry schema every D3 bone carries values for. Order is the file's.
void FillPoseSchema(NodeTree& tree) {
    const auto add = [&tree](const char* name, PoseSpace space, bool inverse) {
        PoseSchema entry;
        entry.name = name;
        entry.space = space;
        entry.inverse = inverse;
        tree.poseSchema.push_back(std::move(entry));
    };
    add("bindA", PoseSpace::Model, false);
    add("bindAInverse", PoseSpace::Model, true);
    add("local", PoseSpace::ParentRelative, false);
    add("bindB", PoseSpace::Model, false);
    add("bindBInverse", PoseSpace::Model, true);
    // B is the pose skinning is defined against — entry 4 is its inverse, which
    // is what a skinning matrix palette wants. A is the hardpoint frame.
    tree.authoritativePose = 3;
    tree.rig = RigConvention::ExplicitBind;
}

struct NodeBuild {
    NodeTree tree;
    /// Bone index -> node index. The identity unless the file's bones were not
    /// parent-first and the tree had to be sorted.
    std::vector<u32> boneToNode;
};

NodeBuild ImportNodes(const d3n::Appearances& source, Diagnostics& out) {
    NodeBuild build;
    NodeTree& tree = build.tree;
    FillPoseSchema(tree);

    const u32 boneCount = static_cast<u32>(source.arBones.size());
    for (u32 b = 0; b < boneCount; ++b) {
        const d3n::BoneStructure& bone = source.arBones[b];
        Node node;
        node.name = bone.szName;
        node.kind = NodeKind::Bone;
        node.resetPayloadForKind();
        node.parent = bone.nParentIndex >= 0 && static_cast<u32>(bone.nParentIndex) < boneCount
                          ? static_cast<u32>(bone.nParentIndex)
                          : kInvalidNode;
        node.local = ToTransform(bone.tTransform2);
        // D3 stores ONE scale float per bone, so an exporter must not be handed
        // a non-uniform scale and left to pick a component.
        node.uniformScaleOnly = true;
        node.poses = {ToTransform(bone.tTransform0), ToTransform(bone.tTransform1),
                      ToTransform(bone.tTransform2), ToTransform(bone.tTransform3),
                      ToTransform(bone.tTransform4)};
        auto& payload = std::get<BonePayload>(node.payload);
        payload.bounds = ToExtent(bone.tBounds);
        payload.sphere = ToSphere(bone.tSphere);
        // The ragdoll rig is not imported; the counts say it was there.
        if (!bone.arCollisionShapes.empty()) {
            node.native.set("collisionShapeCount", static_cast<i64>(bone.arCollisionShapes.size()));
        }
        if (!bone.arConstraints.empty()) {
            node.native.set("constraintCount", static_cast<i64>(bone.arConstraints.size()));
        }
        tree.add(std::move(node));
    }

    build.boneToNode.resize(boneCount);
    for (u32 b = 0; b < boneCount; ++b) {
        build.boneToNode[b] = b;
    }
    if (!tree.parentsPrecedeChildren()) {
        const std::vector<u32> remap = tree.sortParentsFirst();
        if (remap.size() == boneCount) {
            build.boneToNode = remap;
        } else {
            out.warn(DiagCode::ConnectivityCorrupt,
                     "bone hierarchy has a cycle; the tree was left in file order");
        }
    }

    // A bone's particle system is a placement, and the system itself is named
    // rather than contained (§18).
    for (u32 b = 0; b < boneCount; ++b) {
        const d3n::BoneStructure& bone = source.arBones[b];
        if (!bone.snoParticle.valid()) {
            continue;
        }
        Node node;
        node.name = bone.szName + "_particle";
        node.kind = NodeKind::ParticleEmitter;
        node.resetPayloadForKind();
        node.parent = build.boneToNode[b];
        auto& payload = std::get<ParticlePayload>(node.payload);
        payload.system.group = static_cast<u32>(bone.snoParticle.group);
        payload.system.id = static_cast<u32>(bone.snoParticle.id);
        tree.add(std::move(node));
    }

    // A hardpoint is skeleton-relative data shipped with the skeleton, so it is
    // a node of the model and not a field of whatever rides it.
    for (const d3n::Hardpoint& hardpoint : source.arHardpoints) {
        Node node;
        node.name = hardpoint.szName;
        node.kind = NodeKind::Attachment;
        node.resetPayloadForKind();
        node.parent =
            hardpoint.nBoneIndex >= 0 && static_cast<u32>(hardpoint.nBoneIndex) < boneCount
                ? build.boneToNode[static_cast<u32>(hardpoint.nBoneIndex)]
                : kInvalidNode;
        node.local = ToTransform(hardpoint.tTransform);
        tree.add(std::move(node));
    }

    // Static lights are parentless: they are placed in the model, not on a bone.
    for (std::size_t l = 0; l < source.arStaticLights.size(); ++l) {
        const d3n::StaticLight& light = source.arStaticLights[l];
        Node node;
        node.name = "light_" + std::to_string(l);
        node.kind = NodeKind::Light;
        node.resetPayloadForKind();
        node.local.translation = light.vPosition;
        auto& payload = std::get<LightPayload>(node.payload);
        payload.kind = light.nType == 0 ? LightKind::Directional : LightKind::Omni;
        // The colour is packed RGBA8; only the three channels are a colour.
        payload.color = Vector3f{static_cast<f32>(light.dwColor & 0xFFu) / 255.0f,
                                 static_cast<f32>((light.dwColor >> 8) & 0xFFu) / 255.0f,
                                 static_cast<f32>((light.dwColor >> 16) & 0xFFu) / 255.0f};
        payload.intensity = light.flIntensity;
        payload.attenuationStart = light.flAttenuation0;
        payload.attenuationEnd = light.flAttenuation1;
        node.native.set("d3LightType", static_cast<i64>(light.nType));
        node.native.set("d3LightFlags", static_cast<i64>(light.dwFlags));
        tree.add(std::move(node));
    }

    return build;
}

// ============================================================================
// The wardrobe
// ============================================================================

/// Whether @p descriptor is a piece the caller asked for.
///
/// A slot nobody named draws its naked variant, which is what the engine's own
/// fallback bottoms out at. A descriptor with no slot — effect meshes, hair, the
/// five whose material name is the literal "HC" — is not armour and always
/// draws.
bool WardrobeShows(const d3n::GeosetName& descriptor,
                   const std::vector<D3WardrobePiece>& wardrobe) {
    if (!descriptor.parsed || descriptor.slot == d3n::LookSlot::Unknown) {
        return true;
    }
    for (const D3WardrobePiece& piece : wardrobe) {
        if (piece.slot != descriptor.slot) {
            continue;
        }
        return piece.weight == descriptor.weight && piece.variant == descriptor.variant;
    }
    return descriptor.weight == d3n::ArmourWeight::Naked && descriptor.variant == 0;
}

// ============================================================================
// Geometry
// ============================================================================

/// One `GeoSet` -> one `Mesh`. Sub-objects are self-contained — their indices
/// address their own vertex array — so the concatenation carries a base offset.
Mesh ImportGeoSet(const d3n::GeoSet& geoSet, const std::string& name, Model& model,
                  const std::vector<u32>& boneToNode, const D3ImportOptions& options,
                  Diagnostics& out) {
    geom::MeshBuilder builder;

    struct Pending {
        u32 section = 0;
        u32 base = 0;
    };
    std::vector<Pending> pending;
    pending.reserve(geoSet.arSubObjects.size());

    for (std::size_t s = 0; s < geoSet.arSubObjects.size(); ++s) {
        const d3n::SubObject& sub = geoSet.arSubObjects[s];

        MeshSection section;
        section.name = sub.szName;
        // **The join is on `szName`.** Not `szMaterialName`: that holds the Maya
        // shape name and carries the geoset descriptor instead. Corpus-verified
        // 441/441 across the 14 player appearances; using the other field leaves
        // every player character untextured.
        section.materialSlot = model.slotIndex(sub.szName);
        if (section.materialSlot == kInvalidIndex) {
            section.materialSlot = model.addSlot(sub.szName);
            out.warn(DiagCode::SlotNotBound,
                     "sub-object '" + sub.szName + "' names no appearance material",
                     ElementRef(ElementKind::Section, static_cast<u32>(s)), ProfileId::Diablo3);
        }
        section.bounds = ToExtent(sub.tBounds);
        if (!sub.arClothData.empty()) {
            section.flags |= SectionFlags::ClothSimulated;
        }
        if (sub.snoSurface.valid()) {
            section.native.set("surfaceSno", static_cast<i64>(sub.snoSurface.id));
        }
        section.native.set("vertexFormat", static_cast<i64>(sub.dwVertexFormat));
        // `nBoneIndex` is on **every** sub-object, not only the rigid ones: a
        // skinned one still names a bone, and `section.rigidNode` below records
        // it only where it is the whole binding. Kept as a node index, because
        // that is what survives a tree edit — the bone index does not.
        section.native.set(
            "boneNode", sub.nBoneIndex >= 0 && static_cast<u32>(sub.nBoneIndex) < boneToNode.size()
                            ? static_cast<i64>(boneToNode[static_cast<u32>(sub.nBoneIndex)])
                            : -1);
        // **The Maya shape name, kept verbatim.** Not redundant with the parsed
        // triple below: `ActorModel_ApplyLook` never parses this string, it does
        // a case-sensitive substring search for `"<category>_<value>"` in it, so
        // an oddly-spelled name is selected by the spelling and by nothing else.
        // A document that kept only the parse would re-dress differently from
        // the game on exactly the names the parse gets wrong.
        if (!sub.szMaterialName.empty()) {
            section.native.setText("materialName", sub.szMaterialName);
        }

        // §8: visibility is data. The parsed descriptor rides the section
        // whatever the wardrobe said, so a host can re-dress without reimporting.
        const d3n::GeosetName descriptor = d3n::parseGeosetName(sub);
        section.native.set("descriptorParsed", descriptor.parsed ? 1 : 0);
        if (descriptor.parsed) {
            section.native.set("lookSlot", static_cast<i64>(descriptor.slot));
            section.native.set("armourWeight", static_cast<i64>(descriptor.weight));
            section.native.set("variant", static_cast<i64>(descriptor.variant));
            section.native.set("cloth", descriptor.cloth ? 1 : 0);
        }
        if (!WardrobeShows(descriptor, options.wardrobe)) {
            section.flags |= SectionFlags::Hidden;
        }

        // ~91% of shipped sub-objects carry no influences at all and ride one
        // bone rigidly (§5.6), which is a section fact and not a per-vertex one.
        if (sub.arVertexInfluences.empty() && sub.nBoneIndex >= 0 &&
            static_cast<u32>(sub.nBoneIndex) < boneToNode.size()) {
            section.rigidNode = boneToNode[static_cast<u32>(sub.nBoneIndex)];
        }

        Pending entry;
        entry.base = builder.vertexCount();
        entry.section = builder.addSection(std::move(section));
        pending.push_back(entry);

        for (const d3n::FatVertex& vertex : sub.arVertices) {
            builder.addVertex(vertex.vPosition);
        }
        for (std::size_t v = 0; v < sub.arVertexInfluences.size() && v < sub.arVertices.size();
             ++v) {
            const d3n::VertInfluences& influences = sub.arVertexInfluences[v];
            const d3n::Influence* three[3] = {&influences.tInfluence0, &influences.tInfluence1,
                                              &influences.tInfluence2};
            for (const d3n::Influence* influence : three) {
                if (influence->flWeight <= 0.0f || influence->nBoneIndex < 0) {
                    continue;
                }
                const u32 bone = static_cast<u32>(influence->nBoneIndex);
                if (bone >= boneToNode.size()) {
                    out.warn(DiagCode::DanglingNodeReference,
                             "influence names bone " + std::to_string(bone));
                    continue;
                }
                builder.addInfluence(geom::VertexId(entry.base + static_cast<u32>(v)),
                                     boneToNode[bone], influence->flWeight);
            }
        }
    }

    for (std::size_t s = 0; s < geoSet.arSubObjects.size(); ++s) {
        const d3n::SubObject& sub = geoSet.arSubObjects[s];
        const Pending& entry = pending[s];
        for (std::size_t i = 0; i + 2 < sub.arIndices.size(); i += 3) {
            const u32 corners[3] = {entry.base + sub.arIndices[i + 0],
                                    entry.base + sub.arIndices[i + 1],
                                    entry.base + sub.arIndices[i + 2]};
            if (corners[0] >= builder.vertexCount() || corners[1] >= builder.vertexCount() ||
                corners[2] >= builder.vertexCount()) {
                out.warn(DiagCode::IndexOutOfRange,
                         "sub-object face corner past its own vertex array",
                         ElementRef(ElementKind::Section, static_cast<u32>(s)));
                continue;
            }
            const geom::FaceId face =
                builder.addTriangle(geom::VertexId(corners[0]), geom::VertexId(corners[1]),
                                    geom::VertexId(corners[2]), entry.section);
            for (u32 c = 0; c < 3; ++c) {
                const d3n::FatVertex& vertex = sub.arVertices[corners[c] - entry.base];
                builder.setCornerAttr(face, c, geom::names::kNormal, d3n::vertexNormal(vertex));
                builder.setCornerAttr(face, c, geom::names::uv(0), d3n::vertexTexCoord0(vertex));
                builder.setCornerAttr(face, c, geom::names::uv(1), d3n::vertexTexCoord1(vertex));
                // The binormal is its own layer rather than a tangent sign: D3
                // authors both, and reconstructing one from the other assumes an
                // orthonormal frame this content does not promise.
                builder.setCornerAttr(face, c, geom::names::kTangent, d3n::vertexTangent(vertex));
                builder.setCornerAttr(face, c, geom::names::kBinormal, d3n::vertexBinormal(vertex));
                // Both colour channels. D3's vertex shaders read `color0` as a
                // tint and `color1` as the per-vertex ambient occlusion / team
                // mask, so dropping either is a visible change and not a
                // rounding one.
                const d3n::VertexColor color = d3n::vertexColor(vertex);
                const d3n::VertexColor aux = d3n::vertexAuxColor(vertex);
                builder.setCornerAttr(face, c, geom::names::color(0),
                                      std::array<u8, 4>{color.r, color.g, color.b, color.a});
                builder.setCornerAttr(face, c, geom::names::color(1),
                                      std::array<u8, 4>{aux.r, aux.g, aux.b, aux.a});
            }
        }
    }

    geom::MeshBuilder::BuildOutcome outcome = builder.build();
    outcome.mesh.name = name;
    outcome.mesh.recomputeBounds();
    return std::move(outcome.mesh);
}

} // namespace

// ============================================================================
// AssetSource
// ============================================================================

struct AssetSource::Impl {
    sno::d3::native::AssetProvider& provider;
    Stats stats;

    std::unordered_map<i32, std::optional<d3n::Appearances>> appearances;
    std::unordered_map<i32, std::optional<d3n::Actor>> actors;
    std::unordered_map<i32, std::optional<d3n::Anim>> anims;
    std::unordered_map<i32, std::optional<d3n::AnimSet>> animSets;
    std::unordered_map<i32, std::optional<d3n::ShaderMap>> shaderMaps;
    std::unordered_map<i32, std::optional<d3n::Shaders>> shaders;
    std::unordered_map<i32, std::optional<d3n::Material>> materials;

    explicit Impl(sno::d3::native::AssetProvider& source) : provider(source) {}

    /// One cache lookup, one provider round trip at most. A failed load is
    /// cached as a failure, so a missing asset is asked for once per document
    /// rather than once per reference.
    template <class T, class Parse>
    const T* fetch(std::unordered_map<i32, std::optional<T>>& map, d3n::Group group, i32 snoId,
                   Parse parse) {
        ++stats.requests;
        const auto found = map.find(snoId);
        if (found != map.end()) {
            ++stats.hits;
            return found->second.has_value() ? &*found->second : nullptr;
        }
        ++stats.loads;
        std::optional<T> parsed;
        const std::vector<u8> bytes = provider.load(group, snoId);
        if (!bytes.empty()) {
            parsed = parse(bytes);
        }
        if (!parsed.has_value()) {
            ++stats.failures;
        }
        std::optional<T>& slot = map.emplace(snoId, std::move(parsed)).first->second;
        return slot.has_value() ? &*slot : nullptr;
    }
};

AssetSource::AssetSource(sno::d3::native::AssetProvider& provider)
    : pImpl(std::make_unique<Impl>(provider)) {}

AssetSource::~AssetSource() = default;

const d3n::Appearances* AssetSource::appearance(i32 snoId) {
    return pImpl->fetch(pImpl->appearances, d3n::Group::Appearance, snoId,
                        [](const std::vector<u8>& bytes) { return d3n::parseAppearances(bytes); });
}

const d3n::Actor* AssetSource::actor(i32 snoId) {
    return pImpl->fetch(pImpl->actors, d3n::Group::Actor, snoId,
                        [](const std::vector<u8>& bytes) { return d3n::parseActor(bytes); });
}

const d3n::Anim* AssetSource::anim(i32 snoId) {
    return pImpl->fetch(pImpl->anims, d3n::Group::Anim, snoId,
                        [](const std::vector<u8>& bytes) { return d3n::parseAnim(bytes); });
}

const d3n::AnimSet* AssetSource::animSet(i32 snoId) {
    return pImpl->fetch(pImpl->animSets, d3n::Group::AnimSet, snoId,
                        [](const std::vector<u8>& bytes) { return d3n::parseAnimSet(bytes); });
}

const d3n::ShaderMap* AssetSource::shaderMap(i32 snoId) {
    return pImpl->fetch(pImpl->shaderMaps, d3n::Group::ShaderMap, snoId,
                        [](const std::vector<u8>& bytes) { return d3n::parseShaderMap(bytes); });
}

const d3n::Shaders* AssetSource::shaders(i32 snoId) {
    return pImpl->fetch(pImpl->shaders, d3n::Group::Shaders, snoId,
                        [](const std::vector<u8>& bytes) { return d3n::parseShaders(bytes); });
}

const d3n::Material* AssetSource::material(i32 snoId) {
    return pImpl->fetch(pImpl->materials, d3n::Group::Material, snoId,
                        [](const std::vector<u8>& bytes) { return d3n::parseMaterial(bytes); });
}

const AssetSource::Stats& AssetSource::stats() const {
    return pImpl->stats;
}

// ============================================================================
// D3Converter — the interface
// ============================================================================

std::string D3Converter::formatId() const {
    return "d3";
}

std::string D3Converter::formatName() const {
    return "Diablo III";
}

std::span<const ProfileId> D3Converter::profiles() const {
    return std::span<const ProfileId>(kD3Profiles, 1);
}

bool D3Converter::supportsImport() const {
    return true;
}

bool D3Converter::supportsExport() const {
    return false;
}

u32 D3Converter::defaultExportVersion() const {
    return 0;
}

Result<std::vector<u8>> D3Converter::exportToBytes(const Document&, ProfileId, u32) const {
    Result<std::vector<u8>> result;
    result.diagnostics.error(DiagCode::OperationUnsupported,
                             "writing Diablo III SNO assets is out of scope for WEM v3 (§18)");
    return result;
}

d3n::Group D3Converter::SniffGroup(std::span<const u8> data) {
    if (data.size() < kSnoHeaderSize) {
        return d3n::Group::Unknown;
    }
    u32 magic = 0;
    u32 version = 0;
    std::memcpy(&magic, data.data(), sizeof(magic));
    std::memcpy(&version, data.data() + 4, sizeof(version));
    if (magic != kSnoMagic) {
        return d3n::Group::Unknown;
    }
    // Last resort, and only that: these version words are not a namespace and
    // other groups reuse them.
    switch (version) {
    case kAppearanceVersion:
        return d3n::Group::Appearance;
    case kActorVersion:
        return d3n::Group::Actor;
    default:
        return d3n::Group::Unknown;
    }
}

u32 D3Converter::FindAppearanceModel(const Document& document, i32 snoId) {
    for (std::size_t m = 0; m < document.models.size(); ++m) {
        const ProfileMaterialSet* set = document.models[m].setFor(ProfileId::Diablo3);
        if (set == nullptr) {
            continue;
        }
        const NativeBag::Entry* entry = set->native.find("appearanceSnoId");
        if (entry != nullptr && entry->value == snoId) {
            return static_cast<u32>(m);
        }
    }
    return kInvalidIndex;
}

Result<Document> D3Converter::importFromBytes(std::span<const u8> data) const {
    return importFromBytes(data, AssetHint::Unknown, nullptr, D3ImportOptions{});
}

Result<Document> D3Converter::importFromBytes(std::span<const u8> data, AssetHint hint,
                                              AssetSource* assets,
                                              const D3ImportOptions& options) const {
    Result<Document> result;
    const d3n::Group group = hint == AssetHint::Appearance ? d3n::Group::Appearance
                             : hint == AssetHint::Actor    ? d3n::Group::Actor
                                                           : SniffGroup(data);
    if (group == d3n::Group::Appearance) {
        auto parsed = d3n::parseAppearances(data);
        if (!parsed.has_value()) {
            result.diagnostics.error(DiagCode::UnsupportedVersion, "not a readable `.app`");
            return result;
        }
        return fromAppearance(*parsed, assets, options);
    }
    if (group == d3n::Group::Actor) {
        auto parsed = d3n::parseActor(data);
        if (!parsed.has_value()) {
            result.diagnostics.error(DiagCode::UnsupportedVersion, "not a readable `.acr`");
            return result;
        }
        if (assets == nullptr) {
            result.diagnostics.error(DiagCode::AssetUnresolved,
                                     "an actor names its appearance by SNO id; importing one "
                                     "needs a provider");
            return result;
        }
        return fromActor(*parsed, *assets, options);
    }
    result.diagnostics.error(DiagCode::UnsupportedVersion,
                             "bytes are neither a `.app` nor a `.acr`");
    return result;
}

// ============================================================================
// fromAppearance
// ============================================================================

Result<Document> D3Converter::fromAppearance(const d3n::Appearances& source, AssetSource* assets,
                                             const D3ImportOptions& options) const {
    Result<Document> result;
    Diagnostics& diagnostics = result.diagnostics;

    Document document;
    document.space = CoordSpace::Blizzard;
    document.declare(ProfileId::Diablo3);
    document.defaultProfile = ProfileId::Diablo3;
    document.bounds = ToExtent(source.tBounds);

    Model model;
    model.bounds = document.bounds;

    NodeBuild nodes = ImportNodes(source, diagnostics);
    model.nodes = std::move(nodes.tree);

    // --- looks and slots -----------------------------------------------------
    ProfileMaterialSet set;
    set.profile = ProfileId::Diablo3;
    for (const d3n::AppearanceLook& look : source.arLooks) {
        set.looks.add(look.szName);
    }
    if (set.looks.empty()) {
        // A table is never empty (§8); one look degenerates to one material per
        // slot, which is what an appearance with no look list has.
        set.looks = LookTable::Single();
    }
    const std::string wanted =
        options.materialLook.empty() ? kDefaultLookName : options.materialLook;
    const u32 chosen = set.looks.find(wanted);
    if (chosen == kInvalidIndex) {
        if (!options.materialLook.empty()) {
            diagnostics.warn(DiagCode::LookDropped,
                             "look '" + options.materialLook + "' is not in this appearance",
                             ElementRef(), ProfileId::Diablo3);
        }
        set.defaultLook = 0;
    } else {
        set.defaultLook = chosen;
    }

    // The slot list is the appearance's own material names, in file order, so a
    // sub-object's `szName` is a direct lookup.
    for (const d3n::AppearanceMaterial& material : source.arMaterials) {
        model.addSlot(material.szName);
    }
    set.resizeBindings(model.materialSlots.size());

    d3_core::Context context;
    context.sourceVersion = kAppearanceVersion;
    context.internUnknownIds = true;
    context.assets = assets;

    for (std::size_t m = 0; m < source.arMaterials.size(); ++m) {
        const d3n::AppearanceMaterial& material = source.arMaterials[m];
        const u32 slot = model.slotIndex(material.szName);
        if (slot == kInvalidIndex) {
            continue;
        }
        // Every `AppearanceMaterial` holds exactly `arLooks.size()` variants —
        // 233/233 across the 14 player appearances — so the look index IS the
        // variant index and a mismatch is a parse bug, not content variation.
        if (material.arVariants.size() != set.looks.size()) {
            diagnostics.warn(DiagCode::LookBindingMalformed,
                             material.szName + " has " +
                                 std::to_string(material.arVariants.size()) + " variants for " +
                                 std::to_string(set.looks.size()) + " looks",
                             ElementRef(ElementKind::Slot, slot), ProfileId::Diablo3);
        }
        for (std::size_t look = 0; look < material.arVariants.size() && look < set.looks.size();
             ++look) {
            Material imported = d3_core::ImportVariant(
                material.arVariants[look], material.szName + "#" + set.looks.looks[look].name,
                context, diagnostics);
            set.slotBindings[slot].byLook[look] = static_cast<u32>(set.materials.size());
            set.materials.push_back(std::move(imported));
        }
    }

    // The texture table is whatever the materials interned, in first-use order.
    // `path` stays empty: a D3 texture has no name, only an id, and `Describe`
    // already spells the key as `sno:44:<id>` for a diagnostic. Filling it with
    // that same string would make the reference look like it had a path.
    document.textures.resize(context.texturesBySno.size());
    for (const auto& [sno, index] : context.texturesBySno) {
        TextureRef ref;
        ref.key = TextureSnoId{static_cast<u32>(d3n::Group::Textures), sno};
        document.textures[index] = std::move(ref);
    }

    // --- geometry ------------------------------------------------------------
    //
    // Two geosets, in file order: the second is the shadow / low-detail set on
    // the appearances that carry one, and it is a mesh of its own rather than a
    // LOD of the first, because nothing in the file says the two agree.
    model.meshes.push_back(
        ImportGeoSet(source.tGeoSet0, "geoset0", model, nodes.boneToNode, options, diagnostics));
    if (!source.tGeoSet1.arSubObjects.empty()) {
        Mesh second =
            ImportGeoSet(source.tGeoSet1, "geoset1", model, nodes.boneToNode, options, diagnostics);
        second.lodLevel = 1;
        model.meshes.push_back(std::move(second));
    }

    // Sub-objects can add slots the material list did not have, so the bindings
    // are sized last.
    set.resizeBindings(model.materialSlots.size());

    set.native.set("appearanceSnoId", static_cast<i64>(source.dwSnoId));
    set.native.set("sourceVersion", static_cast<i64>(kAppearanceVersion));
    set.native.set("objectType", static_cast<i64>(source.eObjectType));
    if (!source.arCollisionCapsules.empty()) {
        set.native.set("collisionCapsuleCount",
                       static_cast<i64>(source.arCollisionCapsules.size()));
    }
    if (!source.arConstraints.empty()) {
        set.native.set("constraintCount", static_cast<i64>(source.arConstraints.size()));
    }

    model.profileSets.push_back(std::move(set));
    document.models.push_back(std::move(model));
    result.value = std::move(document);
    return result;
}

// ============================================================================
// fromActor / appendActor
// ============================================================================

Result<u32> D3Converter::appendActor(Document& document, const d3n::Actor& source,
                                     AssetSource& assets, const D3ImportOptions& options) const {
    Result<u32> result;
    Diagnostics& diagnostics = result.diagnostics;

    if (!source.snoAppearance.valid()) {
        diagnostics.error(DiagCode::AssetUnresolved, "actor names no appearance");
        return result;
    }

    // The events that would become attach points. Collected before anything is
    // built, because whether there are any decides whether this actor can share
    // a model with one already imported.
    std::vector<const d3n::TriggerEvent*> spawns;
    if (options.attachmentDepth > 0) {
        for (const d3n::MsgTriggeredEvent& message : source.arMsgTriggeredEvents) {
            if (IsSpawn(message.tEvent)) {
                spawns.push_back(&message.tEvent);
            }
        }
    }

    // The look this actor asks for, needed before the model exists so the reuse
    // test below can compare it.
    const std::string wantedLook =
        !options.materialLook.empty() ? options.materialLook : source.tLook0.szName;

    /// **Sharing is conditional, and the condition is the whole point.**
    ///
    /// 2.24 actors per appearance and worst case 594, so reusing a model matters
    /// — but an actor *is* one model, and two actors that equip differently are
    /// two models. Reuse only when the second actor would build the identical
    /// thing: same appearance, same look, and neither side contributes an attach
    /// point. Sharing unconditionally silently drops the second actor's
    /// attachments, which is the shape this used to have.
    ///
    /// What is shared unconditionally is the *parse* — that is `AssetSource`,
    /// and it is where the 594x actually is.
    if (spawns.empty()) {
        const u32 existing = FindAppearanceModel(document, source.snoAppearance.id);
        if (existing != kInvalidIndex && !HasAttachedModel(document.models[existing])) {
            const ProfileMaterialSet* set = document.models[existing].setFor(ProfileId::Diablo3);
            const bool sameLook =
                set != nullptr && (wantedLook.empty() ||
                                   (set->defaultLook < set->looks.size() &&
                                    IEquals(set->looks.looks[set->defaultLook].name, wantedLook)));
            const NativeBag::Entry* animSet =
                set != nullptr ? set->native.find("animSetSnoId") : nullptr;
            const i64 wantedAnimSet = source.snoAnimSet.valid() ? source.snoAnimSet.id : -1;
            const bool sameAnimSet = animSet != nullptr && animSet->value == wantedAnimSet;
            if (sameLook && sameAnimSet) {
                result.value = existing;
                return result;
            }
        }
    }

    const d3n::Appearances* appearance = assets.appearance(source.snoAppearance.id);
    if (appearance == nullptr) {
        diagnostics.error(DiagCode::AssetUnresolved, "appearance " +
                                                         std::to_string(source.snoAppearance.id) +
                                                         " did not load");
        return result;
    }

    // `wantedLook` above already folded the actor's own choice into the
    // caller's: `tLook0` is the first of eight weighted entries, and the weights
    // are a die roll the converter does not make, so the first named look is the
    // one built.
    D3ImportOptions resolved = options;
    resolved.materialLook = wantedLook;

    Result<Document> built = fromAppearance(*appearance, &assets, resolved);
    diagnostics.append(built.diagnostics);
    if (!built.ok() || built->models.empty()) {
        diagnostics.error(DiagCode::AssetUnresolved, "appearance did not convert");
        return result;
    }

    // Merge the built document into @p document: one model, and its textures
    // rebased onto the document-wide table.
    Document& builtDocument = *built.value;
    Model model = std::move(builtDocument.models.front());
    std::vector<u32> textureRemap(builtDocument.textures.size(), kInvalidIndex);
    // Every D3 texture reference is a SNO id, so that id is the identity. The
    // key variant has no `operator==` -- its alternatives are plain structs --
    // and giving it one for this would be defining equality for three addressing
    // schemes to answer a question about one.
    const auto snoOf = [](const TextureRef& texture) -> u32 {
        const TextureSnoId* key = std::get_if<TextureSnoId>(&texture.key);
        return key != nullptr ? key->id : kInvalidIndex;
    };
    for (std::size_t t = 0; t < builtDocument.textures.size(); ++t) {
        const TextureRef& texture = builtDocument.textures[t];
        u32 found = kInvalidIndex;
        for (std::size_t e = 0; e < document.textures.size(); ++e) {
            if (snoOf(document.textures[e]) == snoOf(texture)) {
                found = static_cast<u32>(e);
                break;
            }
        }
        if (found == kInvalidIndex) {
            found = static_cast<u32>(document.textures.size());
            document.textures.push_back(texture);
        }
        textureRemap[t] = found;
    }
    for (ProfileMaterialSet& set : model.profileSets) {
        for (Material& material : set.materials) {
            CommonMaterial& common = material.InitCommon();
            for (u32 ordinal = 0; ordinal < common.ordinalCount(); ++ordinal) {
                TextureInput* input = common.inputAt(ordinal);
                if (input != nullptr && input->texture < textureRemap.size()) {
                    input->texture = textureRemap[input->texture];
                }
            }
        }
    }

    model.name = "actor_" + std::to_string(source.dwSnoId);
    // The reuse key's second half. An actor is one model and an actor names one
    // anim set, so two actors over one appearance with different animations are
    // two models — the same rule the look and the attach points already state.
    if (ProfileMaterialSet* set = model.setFor(ProfileId::Diablo3); set != nullptr) {
        set->native.set("animSetSnoId",
                        static_cast<i64>(source.snoAnimSet.valid() ? source.snoAnimSet.id : -1));
    }
    const u32 index = static_cast<u32>(document.models.size());
    document.declare(ProfileId::Diablo3);
    document.models.push_back(std::move(model));
    result.value = index;

    if (options.importAnimation && source.snoAnimSet.valid()) {
        Result<u32> animSet = importAnimSet(document, index, source.snoAnimSet.id, assets);
        diagnostics.append(animSet.diagnostics);
    }

    // --- what rides the hardpoints -------------------------------------------
    //
    // `spawns` was collected before the model was built, so the filtering rules
    // live in `IsSpawn` and are applied once.
    for (const d3n::TriggerEvent* spawn : spawns) {
        const d3n::TriggerEvent& event = *spawn;
        const i32 group = event.tPayload.eSnoGroup;
        const i32 payload = event.tPayload.dwNameHandle;

        // The hardpoint lookup is case-insensitive: the engine interns
        // `HP_head` and shipped events spell it `HP_Head`, and a byte compare
        // loses five hundred real attachments. An empty or `Default` name means
        // the model origin, which is a normal answer.
        u32 node = kInvalidNode;
        const std::string& wanted = event.tHardpoint0.szName;
        if (!wanted.empty() && !IEquals(wanted, "Default") && !IEquals(wanted, "- None -") &&
            !IEquals(wanted, "Don't Override")) {
            // Scoped: the recursion below pushes into `document.models`, and a
            // reference into that vector would not survive it.
            const NodeTree& hostNodes = document.models[index].nodes;
            for (u32 n = 0; n < hostNodes.size(); ++n) {
                if (hostNodes.nodes[n].kind == NodeKind::Attachment &&
                    IEquals(hostNodes.nodes[n].name, wanted)) {
                    node = n;
                    break;
                }
            }
            if (node == kInvalidNode) {
                diagnostics.warn(DiagCode::HardpointUnresolved, "event names hardpoint '" + wanted +
                                                                    "', which the model has no "
                                                                    "attachment node for");
            }
        }

        Node attach;
        attach.name = wanted.empty() ? "attach" : wanted + "_attach";
        attach.kind = NodeKind::Attachment;
        attach.resetPayloadForKind();
        attach.parent = node;
        auto& attachPayload = std::get<AttachmentPayload>(attach.payload);
        attachPayload.asset.group = static_cast<u32>(group);
        attachPayload.asset.id = static_cast<u32>(payload);

        if (group == static_cast<i32>(d3n::Group::Actor)) {
            const d3n::Actor* child = assets.actor(payload);
            if (child == nullptr) {
                diagnostics.warn(DiagCode::AssetUnresolved,
                                 "attached actor " + std::to_string(payload) + " did not load");
            } else {
                D3ImportOptions childOptions = options;
                childOptions.attachmentDepth = options.attachmentDepth - 1;
                // The wardrobe is the host character's; a weapon does not wear
                // armour, and passing it down would hide the child's own parts.
                childOptions.wardrobe.clear();
                childOptions.materialLook.clear();
                Result<u32> childModel = appendActor(document, *child, assets, childOptions);
                diagnostics.append(childModel.diagnostics);
                if (childModel.ok() && *childModel.value != index) {
                    attachPayload.model = *childModel.value;
                } else if (childModel.ok()) {
                    // The child shares this actor's appearance and contributed
                    // nothing, so the reuse above handed back this very model. A
                    // model riding itself is a cycle; the `AssetKey` still names
                    // what the event said, which is all a host needs.
                    diagnostics.info(DiagCode::AssetUnresolved,
                                     "attached actor " + std::to_string(payload) +
                                         " resolves to this model; left named, not linked");
                }
            }
        }
        document.models[index].nodes.add(std::move(attach));
    }

    return result;
}

namespace {

/// The 30 tag maps of an `.ans`, in the order the runtime numbers them. Named
/// rather than indexed because a set is looked up by what the character holds,
/// and "map 17" is not a thing a caller can ask for.
using TagMapField = std::vector<d3n::AnimSetTagMapEntry> d3n::AnimSet::*;

struct TagMap {
    const char* name;
    TagMapField field;
};

const TagMap kTagMaps[] = {
    {"core", &d3n::AnimSet::tCoreTagMap},
    {"hth", &d3n::AnimSet::tmHth},
    {"1HSwing", &d3n::AnimSet::tm1HSwing},
    {"1HThrust", &d3n::AnimSet::tm1HThrust},
    {"2HSwing", &d3n::AnimSet::tm2HSwing},
    {"2HThrust", &d3n::AnimSet::tm2HThrust},
    {"staff", &d3n::AnimSet::tmStaff},
    {"bow", &d3n::AnimSet::tmBow},
    {"xbow", &d3n::AnimSet::tmXBow},
    {"wand", &d3n::AnimSet::tmWand},
    {"dualWield", &d3n::AnimSet::tmDualWield},
    {"hthWithOrb", &d3n::AnimSet::tmHthWithOrb},
    {"1HSwingWithOrb", &d3n::AnimSet::tm1HSwingWithOrb},
    {"1HThrustWithOrb", &d3n::AnimSet::tm1HThrustWithOrb},
    {"dualWieldSwordFist", &d3n::AnimSet::tmDualWieldSwordFist},
    {"dualWieldFistFist", &d3n::AnimSet::tmDualWieldFistFist},
    {"1HFist", &d3n::AnimSet::tm1HFist},
    {"2HAxeMace", &d3n::AnimSet::tm2HAxeMace},
    {"handXBow", &d3n::AnimSet::tmHandXBow},
    {"wandWithOrb", &d3n::AnimSet::tmWandWithOrb},
    {"1HSwingWithShield", &d3n::AnimSet::tm1HSwingWithShield},
    {"1HThrustWithShield", &d3n::AnimSet::tm1HThrustWithShield},
    {"hthWithShield", &d3n::AnimSet::tmHthWithShield},
    {"2HSwingWithShield", &d3n::AnimSet::tm2HSwingWithShield},
    {"2HThrustWithShield", &d3n::AnimSet::tm2HThrustWithShield},
    {"staffWithShield", &d3n::AnimSet::tmStaffWithShield},
    {"2HFlail", &d3n::AnimSet::tm2HFlail},
    {"2HFlailWithShield", &d3n::AnimSet::tm2HFlailWithShield},
    {"onHorse", &d3n::AnimSet::tmOnHorse},
};

} // namespace

Result<u32> D3Converter::importAnimSet(Document& document, u32 model, i32 snoId,
                                       AssetSource& assets) const {
    Result<u32> result;
    if (model >= document.models.size()) {
        result.diagnostics.error(DiagCode::ClipTargetMissing,
                                 "model " + std::to_string(model) + " of " +
                                     std::to_string(document.models.size()));
        return result;
    }
    const d3n::AnimSet* source = assets.animSet(snoId);
    if (source == nullptr) {
        result.diagnostics.warn(DiagCode::AssetUnresolved,
                                "anim set " + std::to_string(snoId) + " did not load");
        return result;
    }

    // One `.ani` imported once, however many tags name it — an `.ans` reuses an
    // animation across weapon classes constantly, and a clip per reference would
    // multiply the corpus by the number of maps.
    std::vector<std::pair<i32, u32>> clipOfAnim;
    const auto clipFor = [&](i32 animSno) -> u32 {
        for (const auto& entry : clipOfAnim) {
            if (entry.first == animSno) {
                return entry.second;
            }
        }
        const d3n::Anim* anim = assets.anim(animSno);
        if (anim == nullptr) {
            clipOfAnim.emplace_back(animSno, kInvalidIndex);
            return kInvalidIndex;
        }
        const std::vector<u32> clips =
            d3_anim::ImportAnim(*anim, document, model, result.diagnostics);
        // A tag names the animation; which permutation plays is a weighted roll
        // the runtime makes, so the tag points at the first and the rest are in
        // the document beside it.
        const u32 first = clips.empty() ? kInvalidIndex : clips.front();
        clipOfAnim.emplace_back(animSno, first);
        return first;
    };

    const u32 core = static_cast<u32>(document.animSets.size());
    u32 unresolved = 0;
    for (std::size_t m = 0; m < sizeof(kTagMaps) / sizeof(kTagMaps[0]); ++m) {
        const std::vector<d3n::AnimSetTagMapEntry>& rows = source->*(kTagMaps[m].field);
        if (m != 0 && rows.empty()) {
            continue; // Only the core map is worth an empty set.
        }
        AnimSet set;
        set.name = kTagMaps[m].name;
        // The fallback field spelling the fallback: a weapon map with no row for
        // a tag uses core's, which is what `baseAnimSet` means.
        set.baseAnimSet = m == 0 ? kInvalidIndex : core;
        for (const d3n::AnimSetTagMapEntry& row : rows) {
            if (!row.snoAnim.valid()) {
                continue;
            }
            const u32 clip = clipFor(row.snoAnim.id);
            if (clip == kInvalidIndex) {
                ++unresolved;
                continue;
            }
            set.byTag.push_back(AnimTag{static_cast<u32>(row.dwTagId), clip});
        }
        document.animSets.push_back(std::move(set));
    }

    if (unresolved != 0) {
        result.diagnostics.warn(DiagCode::AssetUnresolved,
                                std::to_string(unresolved) +
                                    " tags name an animation that did not load");
    }
    document.models[model].animSet = core;
    result.value = core;
    return result;
}

Result<Document> D3Converter::fromActor(const d3n::Actor& source, AssetSource& assets,
                                        const D3ImportOptions& options) const {
    Result<Document> result;
    Document document;
    document.space = CoordSpace::Blizzard;
    document.defaultProfile = ProfileId::Diablo3;
    document.declare(ProfileId::Diablo3);

    Result<u32> root = appendActor(document, source, assets, options);
    result.diagnostics = std::move(root.diagnostics);
    if (!root.ok()) {
        return result;
    }
    document.name = document.models[*root.value].name;
    result.value = std::move(document);
    return result;
}

// ============================================================================
// toAppearance — the native model, in memory (§18's line, and which side of it)
// ============================================================================

namespace {

d3n::PRSTransform FromTransform(const Transform& source) {
    d3n::PRSTransform out;
    out.qRotation =
        Vector4f{source.rotation.x, source.rotation.y, source.rotation.z, source.rotation.w};
    out.vTranslation = source.translation;
    // One float. `Node::uniformScaleOnly` is the import's promise that this is
    // safe; the caller checks it and reports rather than silently taking x.
    out.flScale = source.scale.x;
    return out;
}

d3n::PRTransform FromTransformPR(const Transform& source) {
    d3n::PRTransform out;
    out.qRotation =
        Vector4f{source.rotation.x, source.rotation.y, source.rotation.z, source.rotation.w};
    out.vTranslation = source.translation;
    return out;
}

d3n::AABB FromExtent(const Extent& extent) {
    d3n::AABB out;
    out.vCenter = Vector3f{(extent.minimum.x + extent.maximum.x) * 0.5f,
                           (extent.minimum.y + extent.maximum.y) * 0.5f,
                           (extent.minimum.z + extent.maximum.z) * 0.5f};
    out.vHalfExtent = Vector3f{(extent.maximum.x - extent.minimum.x) * 0.5f,
                               (extent.maximum.y - extent.minimum.y) * 0.5f,
                               (extent.maximum.z - extent.minimum.z) * 0.5f};
    return out;
}

d3n::Sphere FromSphere(const Sphere& sphere) {
    d3n::Sphere out;
    out.vCenter = sphere.center;
    out.flRadius = sphere.radius;
    return out;
}

/// The inverse of `d3n::unpackVertexVector`. Rounds rather than truncates: the
/// decode is `b * 2/255 - 1`, so 127.5 is the zero and truncation biases every
/// component of every normal one step negative.
u32 PackVertexVector(const Vector3f& value) {
    const auto channel = [](f32 v) -> u32 {
        const f32 scaled = (std::clamp(v, -1.0f, 1.0f) + 1.0f) * (255.0f / 2.0f);
        return static_cast<u32>(std::clamp(scaled + 0.5f, 0.0f, 255.0f));
    };
    // The fourth byte is zero in every shipped vertex.
    return channel(value.x) | (channel(value.y) << 8) | (channel(value.z) << 16);
}

/// The inverse of `d3n::unpackTexCoord`: two u16 in 8.8 fixed point, biased 64.
/// The range is [-64, 63.998]; a UV outside it wraps rather than clamping in the
/// original, but a converter that wrapped would move a coordinate the source
/// held exactly, so this clamps and the caller counts.
u32 PackTexCoord(const Vector2f& value, bool& clamped) {
    const auto channel = [&clamped](f32 v) -> u32 {
        const f32 scaled = (v + 64.0f) * 512.0f;
        if (scaled < 0.0f || scaled > 65535.0f) {
            clamped = true;
        }
        return static_cast<u32>(std::clamp(scaled + 0.5f, 0.0f, 65535.0f));
    };
    return channel(value.x) | (channel(value.y) << 16);
}

u32 PackVertexColor(const std::array<u8, 4>& rgba) {
    return static_cast<u32>(rgba[0]) | (static_cast<u32>(rgba[1]) << 8) |
           (static_cast<u32>(rgba[2]) << 16) | (static_cast<u32>(rgba[3]) << 24);
}

/// A bone's five `PRSTransform`s, from whatever the tree actually carries.
///
/// A document imported from D3 carries all five and they leave untouched — they
/// are five *different* poses (see the file comment) and deriving any of them
/// from another loses the 8% of bones where A and B disagree. A document from
/// anywhere else has no such schema, and then the honest answer is the one every
/// other format states: A and B are both the composed bind, and the local is the
/// local.
std::array<Transform, 5> FivePoses(const NodeTree& tree, u32 node, bool& derived) {
    const Node& source = tree.nodes[node];
    if (tree.poseSchema.size() == 5 && source.poses.size() == 5) {
        return {source.poses[0], source.poses[1], source.poses[2], source.poses[3],
                source.poses[4]};
    }
    derived = true;
    const Transform world = tree.worldBind(node);
    const Transform inverse = Inverse(world);
    return {world, inverse, source.local, world, inverse};
}

/// A `Material`'s D3 block, or null. Never the common projection: §7.1 says a
/// consumer reads the native block when it is authoritative, and for D3 it
/// always is — the import attaches it that way because the common material is a
/// projection of a shader the game compiled.
const native::D3Material* D3BlockOf(const Material& material) {
    return std::get_if<native::D3Material>(&material.Native());
}

/// One `wem::Material` back to the per-look record a sub-object resolves.
d3n::SubObjectAppearance ExportVariant(const Material& material) {
    d3n::SubObjectAppearance out;
    const native::D3Material* block = D3BlockOf(material);
    if (block == nullptr) {
        // Nothing to restore. The record is still written — a slot with no
        // variant draws nothing at all — and the caller reports it once.
        out.dwUnknown00 = 1;
        return out;
    }
    out.dwUnknown00 = block->variantFlags;
    CopyFromNative(block->cloth, out.snoCloth);
    CopyFromNative(block->baseMaterial, out.snoMaterial);
    CopyFromNative(block->uber, out.tMaterial);
    out.arShaderParams.reserve(block->shaderParams.size());
    for (const native::D3TagValue& value : block->shaderParams) {
        d3n::TagMapEntry entry;
        CopyFromNative(value, entry);
        out.arShaderParams.push_back(entry);
    }
    return out;
}

/// The vertex attributes a D3 `FatVertex` holds, as the render view wants them.
geom::RenderMeshDesc D3VertexDesc() {
    geom::RenderMeshDesc desc;
    desc.attributes = {
        {geom::names::kPosition, utils::AttributeClass::Position, utils::AttributeEncoding::Float32,
         3, 0},
        {geom::names::kNormal, utils::AttributeClass::Normal, utils::AttributeEncoding::Float32, 3,
         0},
        {geom::names::uv(0), utils::AttributeClass::UV, utils::AttributeEncoding::Float32, 2, 0},
        {geom::names::uv(1), utils::AttributeClass::UV, utils::AttributeEncoding::Float32, 2, 0},
        {geom::names::kTangent, utils::AttributeClass::Tangent, utils::AttributeEncoding::Float32,
         3, 0},
        {geom::names::kBinormal, utils::AttributeClass::Binormal, utils::AttributeEncoding::Float32,
         3, 0},
        {geom::names::color(0), utils::AttributeClass::Color, utils::AttributeEncoding::UInt8, 4,
         0},
        {geom::names::color(1), utils::AttributeClass::Color, utils::AttributeEncoding::UInt8, 4,
         0},
    };
    desc.includeSkin = true;
    desc.maxInfluences = Profile(ProfileId::Diablo3).maxBoneInfluences;
    desc.splitBySection = true;
    return desc;
}

/// The state one `toAppearance` accumulates. A class because the geometry pass
/// needs the bone map, the diagnostics and the counters, and threading five
/// out-parameters through four functions is how they get out of step.
class AppearanceWriter {
public:
    AppearanceWriter(const Document& document, const Model& model, const ProfileMaterialSet& set,
                     Diagnostics& out)
        : document_(document), model_(model), set_(set), out_(out) {}

    D3AppearanceExport run(u32 look) {
        D3AppearanceExport result;
        result.look = look;
        d3n::Appearances& appearance = result.appearance;

        appearance.dwSnoId = static_cast<i32>(set_.native.value("appearanceSnoId", -1));
        appearance.eObjectType = static_cast<i32>(set_.native.value("objectType", 0));
        appearance.tBounds = FromExtent(model_.bounds.valid() ? model_.bounds : document_.bounds);

        writeBones(appearance);
        writeLooks(appearance);
        writeGeometry(appearance, result);
        // Both lists are all-default far more often than not, and an empty one
        // is what a host reads as "nothing to apply".
        if (overrides_ == 0) {
            result.looks.clear();
        }
        if (dyed_ == 0) {
            result.dyes.clear();
        }

        appearance.dwBoneCount = static_cast<i32>(appearance.arBones.size());
        appearance.dwHardpointCount = static_cast<i32>(appearance.arHardpoints.size());
        appearance.dwLookCount = static_cast<i32>(appearance.arLooks.size());
        appearance.dwMaterialCount = static_cast<i32>(appearance.arMaterials.size());
        appearance.dwStaticLightCount = static_cast<i32>(appearance.arStaticLights.size());
        appearance.tGeoSet0.dwSubObjectCount =
            static_cast<i32>(appearance.tGeoSet0.arSubObjects.size());
        appearance.tGeoSet1.dwSubObjectCount =
            static_cast<i32>(appearance.tGeoSet1.arSubObjects.size());

        // §18 lets rigid bodies, joints and cloth ride along **as native
        // blocks**, and D3's cloth is per sub-object — a `ClothStructure` of
        // particles, staples and two constraint sets — which is the one scope
        // WEM has no typed native block for. So the section keeps
        // `ClothSimulated` and loses the simulation: the piece draws, skinned,
        // where it would have hung.
        if (clothSections_ != 0) {
            out_.warn(DiagCode::OperationUnsupported,
                      std::to_string(clothSections_) +
                          " sections are cloth-simulated and the simulation is not carried; they "
                          "draw skinned",
                      ElementRef(), ProfileId::Diablo3);
        }

        // The ragdoll rig, which the import never took either. Counting it is
        // the whole point of the counts it did keep: a model that had 26 bone
        // collision shapes and comes back with none is a fact worth hearing.
        const i64 capsules = set_.native.value("collisionCapsuleCount", 0);
        const i64 constraints = set_.native.value("constraintCount", 0);
        if (capsules != 0 || constraints != 0 || droppedBoneShapes_ != 0) {
            out_.info(DiagCode::OperationUnsupported,
                      "the ragdoll rig is not carried: " + std::to_string(droppedBoneShapes_) +
                          " bone collision shapes, " + std::to_string(capsules) +
                          " collision capsules and " + std::to_string(constraints) +
                          " constraints were counted on import and never stored",
                      ElementRef(), ProfileId::Diablo3);
        }
        return result;
    }

private:
    // --- bones, hardpoints, lights -----------------------------------------

    void writeBones(d3n::Appearances& appearance) {
        const NodeTree& tree = model_.nodes;
        boneOf_.assign(tree.size(), kInvalidIndex);
        for (u32 n = 0; n < tree.size(); ++n) {
            if (tree.nodes[n].kind == NodeKind::Bone) {
                boneOf_[n] = static_cast<u32>(appearance.arBones.size());
                appearance.arBones.emplace_back();
            }
        }

        bool derivedAny = false;
        u32 nonUniform = 0;
        for (u32 n = 0; n < tree.size(); ++n) {
            if (boneOf_[n] == kInvalidIndex) {
                continue;
            }
            const Node& node = tree.nodes[n];
            d3n::BoneStructure& bone = appearance.arBones[boneOf_[n]];
            bone.szName = node.name;
            bone.nParentIndex = static_cast<i32>(nearestBone(n));
            const auto& payload = std::get<BonePayload>(node.payload);
            bone.tBounds = FromExtent(payload.bounds);
            bone.tSphere = FromSphere(payload.sphere);

            const std::array<Transform, 5> poses = FivePoses(tree, n, derivedAny);
            d3n::PRSTransform* slots[5] = {&bone.tTransform0, &bone.tTransform1, &bone.tTransform2,
                                           &bone.tTransform3, &bone.tTransform4};
            for (std::size_t p = 0; p < 5; ++p) {
                *slots[p] = FromTransform(poses[p]);
                if (!Uniform(poses[p].scale)) {
                    ++nonUniform;
                }
            }
            droppedBoneShapes_ += static_cast<u32>(node.native.value("collisionShapeCount", 0));
        }

        if (derivedAny) {
            out_.warn(DiagCode::BindPoseRecomposed,
                      "the tree carries no five-entry D3 pose schema; bind poses A and B were "
                      "both composed from the local chain",
                      ElementRef(), ProfileId::Diablo3);
        }
        if (nonUniform != 0) {
            out_.warn(DiagCode::NonUniformScaleFlattened,
                      std::to_string(nonUniform) +
                          " bind poses carry a non-uniform scale; a D3 `PRSTransform` holds one "
                          "float and the x component was written",
                      ElementRef(), ProfileId::Diablo3);
        }

        // A hardpoint is an attachment node that carries nothing. The ones that
        // do are the actor's spawn points (§9.5) — they were never in the `.app`
        // and putting them back would invent hardpoints the skeleton never had.
        for (u32 n = 0; n < tree.size(); ++n) {
            const Node& node = tree.nodes[n];
            if (node.kind != NodeKind::Attachment) {
                continue;
            }
            const auto& payload = std::get<AttachmentPayload>(node.payload);
            if (!payload.asset.empty() || payload.model != kInvalidIndex) {
                continue;
            }
            d3n::Hardpoint point;
            point.szName = node.name;
            point.nBoneIndex = static_cast<i32>(nearestBone(n));
            point.tTransform = FromTransformPR(node.local);
            appearance.arHardpoints.push_back(std::move(point));
        }

        for (u32 n = 0; n < tree.size(); ++n) {
            const Node& node = tree.nodes[n];
            if (node.kind == NodeKind::ParticleEmitter) {
                // `nearestBone` answers `kInvalidIndex` for a node under no
                // bone, which the bounds check already rejects.
                const u32 bone = nearestBone(n);
                if (bone < appearance.arBones.size()) {
                    const auto& payload = std::get<ParticlePayload>(node.payload);
                    appearance.arBones[bone].snoParticle.id = static_cast<i32>(payload.system.id);
                    appearance.arBones[bone].snoParticle.group =
                        static_cast<d3n::Group>(payload.system.group);
                }
                continue;
            }
            if (node.kind != NodeKind::Light) {
                continue;
            }
            const auto& payload = std::get<LightPayload>(node.payload);
            d3n::StaticLight light;
            light.nType = static_cast<i32>(node.native.value("d3LightType", 0));
            light.dwFlags = static_cast<i32>(node.native.value("d3LightFlags", 0));
            light.vPosition = node.local.translation;
            light.flIntensity = payload.intensity;
            light.flAttenuation0 = payload.attenuationStart;
            light.flAttenuation1 = payload.attenuationEnd;
            const auto byte = [](f32 v) {
                return static_cast<u32>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
            };
            light.dwColor = byte(payload.color.x) | (byte(payload.color.y) << 8) |
                            (byte(payload.color.z) << 16);
            appearance.arStaticLights.push_back(light);
        }
    }

    /// The bone index of @p node's nearest bone ancestor, or `kInvalidIndex` —
    /// which is -1 as the `i32` a `nParentIndex` is.
    ///
    /// A walk, not a lookup: a document from another format can put a Helper
    /// between two bones, and D3's parent field addresses bones only.
    u32 nearestBone(u32 node) const {
        for (u32 walk = model_.nodes.nodes[node].parent; walk != kInvalidNode;
             walk = model_.nodes.nodes[walk].parent) {
            if (boneOf_[walk] != kInvalidIndex) {
                return boneOf_[walk];
            }
        }
        return kInvalidIndex;
    }

    static bool Uniform(const Vector3f& scale) {
        const f32 tolerance = 1e-5f;
        return std::fabs(scale.x - scale.y) <= tolerance &&
               std::fabs(scale.x - scale.z) <= tolerance;
    }

    // --- looks and materials -----------------------------------------------

    void writeLooks(d3n::Appearances& appearance) {
        for (const Look& entry : set_.looks.looks) {
            d3n::AppearanceLook out;
            out.szName = entry.name;
            appearance.arLooks.push_back(std::move(out));
        }
        if (appearance.arLooks.empty()) {
            appearance.arLooks.push_back(d3n::AppearanceLook{});
        }

        // One `AppearanceMaterial` per slot, one variant per look — the shape
        // the join expects, and the reason `D3VariantFor` can index
        // `arVariants[lookIndex]` at all.
        u32 improvised = 0;
        for (std::size_t slot = 0; slot < model_.materialSlots.size(); ++slot) {
            d3n::AppearanceMaterial material;
            material.szName = model_.materialSlots[slot];
            bool bound = false;
            for (std::size_t l = 0; l < appearance.arLooks.size(); ++l) {
                const Material* resolved = Resolve(model_, static_cast<u32>(slot),
                                                   ProfileId::Diablo3, static_cast<u32>(l));
                if (resolved == nullptr) {
                    material.arVariants.push_back(d3n::SubObjectAppearance{});
                    continue;
                }
                bound = true;
                if (D3BlockOf(*resolved) == nullptr) {
                    ++improvised;
                }
                material.arVariants.push_back(ExportVariant(*resolved));
            }
            if (!bound) {
                out_.warn(DiagCode::SlotNotBound,
                          "slot '" + material.szName + "' has no Diablo III material",
                          ElementRef(ElementKind::Slot, static_cast<u32>(slot)),
                          ProfileId::Diablo3);
            }
            appearance.arMaterials.push_back(std::move(material));
        }
        if (improvised != 0) {
            out_.warn(DiagCode::DroppedNativeBlock,
                      std::to_string(improvised) +
                          " materials carry no Diablo III block; they were written empty rather "
                          "than derived, because a `UberMaterial` names textures by SNO id and "
                          "the common view holds document indices",
                      ElementRef(), ProfileId::Diablo3);
        }
    }

    // --- geometry ----------------------------------------------------------

    void writeGeometry(d3n::Appearances& appearance, D3AppearanceExport& result) {
        const geom::RenderMeshDesc desc = D3VertexDesc();
        bool clamped = false;

        for (std::size_t m = 0; m < model_.meshes.size(); ++m) {
            const Mesh& mesh = model_.meshes[m];
            // Two geosets and no more: the second is the appearance's shadow /
            // low-detail set, and a document carrying a third mesh has nowhere
            // in an `.app` to put it.
            d3n::GeoSet& target = mesh.lodLevel == 0 ? appearance.tGeoSet0 : appearance.tGeoSet1;
            if (mesh.lodLevel > 1) {
                out_.warn(DiagCode::LayerDropped,
                          "mesh '" + mesh.name + "' is LOD " + std::to_string(mesh.lodLevel) +
                              "; an appearance holds two geosets and it was written into the "
                              "second",
                          ElementRef(ElementKind::Mesh, static_cast<u32>(m)), ProfileId::Diablo3);
            }

            const geom::RenderMesh render = geom::BuildRenderMesh(mesh, desc);
            out_.append(render.diagnostics);

            // A section the §5.3 repair emptied writes no record, so say so:
            // 156 sub-objects across the shipped corpus are two- or
            // three-triangle non-manifold `invis_` collision placeholders that
            // lose everything, and a record silently missing from the middle of
            // a geoset is exactly what a counts-only gate never sees.
            for (std::size_t sec = 0; sec < mesh.sections.size(); ++sec) {
                if (mesh.facesOfSection(static_cast<u32>(sec)).empty()) {
                    out_.warn(DiagCode::DegenerateFaceDropped,
                              "section '" + mesh.sections[sec].name +
                                  "' has no faces left and writes no sub-object",
                              ElementRef(ElementKind::Section, static_cast<u32>(sec)),
                              ProfileId::Diablo3);
                }
            }

            const std::vector<Vector3f> positions = render.vertices.getPositions();
            const std::vector<Vector3f> normals = render.vertices.getNormals();
            const std::vector<Vector2f> uv0 = render.vertices.getUVs(0);
            const std::vector<Vector2f> uv1 = render.vertices.getUVs(1);
            const std::vector<Vector4f> tangents = render.vertices.getTangents();
            const std::vector<Vector3f> binormals = render.vertices.getBinormals();
            const std::vector<Vector4f> color0 = render.vertices.getColors(0);
            const std::vector<Vector4f> color1 = render.vertices.getColors(1);
            const std::vector<std::array<u32, 4>> boneIndices = render.vertices.getBoneIndices();
            const std::vector<std::array<f32, 4>> boneWeights = render.vertices.getBoneWeights();

            for (const geom::RenderRange& range : render.ranges) {
                if (range.indexCount == 0) {
                    // No faces, no sub-object. That keeps `result.hidden`
                    // aligned with the geosets a renderer emits, which skips
                    // exactly the sub-objects with no geometry.
                    continue;
                }
                const MeshSection* section =
                    range.section < mesh.sections.size() ? &mesh.sections[range.section] : nullptr;

                d3n::SubObject sub;
                sub.szName = section != nullptr ? section->name : std::string();
                // The join key is `szName` and the descriptor is
                // `szMaterialName`, and the two are different strings — see the
                // import. A section that lost the second gets one built from the
                // first, which parses to "no slot" and therefore always draws.
                if (section != nullptr) {
                    sub.szMaterialName = section->native.text("materialName");
                    if (sub.szMaterialName.empty()) {
                        sub.szMaterialName = sub.szName + "Shape_" + sub.szName + "_001";
                    }
                    sub.dwVertexFormat = static_cast<i32>(section->native.value("vertexFormat", 0));
                    sub.tBounds = FromExtent(section->bounds);
                    const i64 surface = section->native.value("surfaceSno", -1);
                    if (surface >= 0) {
                        sub.snoSurface.id = static_cast<i32>(surface);
                        sub.snoSurface.group = d3n::Group::Surface;
                    }
                }

                const Slices slices{positions, normals, uv0,    uv1,         tangents,
                                    binormals, color0,  color1, boneIndices, boneWeights};
                writeSubObject(render, range, slices, section, sub, clamped);

                if (section != nullptr && hasFlag(section->flags, SectionFlags::ClothSimulated)) {
                    ++clothSections_;
                }
                // **Geoset order, not mesh order.** A renderer walks `tGeoSet0`
                // and then `tGeoSet1`, and a document from another format can
                // interleave its LODs — mesh 0 at LOD 0, mesh 1 at LOD 1, mesh 2
                // at LOD 0 again — so appending in mesh order would put a
                // geoset-1 entry in the middle of geoset 0's run and shift every
                // mask a host applies.
                Emitted& emitted = &target == &appearance.tGeoSet0 ? first_ : second_;
                emitted.hidden.push_back(
                    section != nullptr && hasFlag(section->flags, SectionFlags::Hidden) ? 1u : 0u);
                // The two per-piece overrides an `.app` also has no field for.
                // They ride the section's bag because they are per (section),
                // unlike the visibility bit, which is per (section, look).
                const i64 look =
                    section != nullptr ? section->native.value("lookOverride", -1) : -1;
                const i64 dye = section != nullptr ? section->native.value("dye", 0) : 0;
                emitted.looks.push_back(look >= 0 ? static_cast<u32>(look) : kInvalidIndex);
                emitted.dyes.push_back(static_cast<i32>(dye));
                if (look >= 0) {
                    ++overrides_;
                }
                if (dye != 0) {
                    ++dyed_;
                }
                emitted.sections.emplace_back(static_cast<u32>(m), range.section);
                target.arSubObjects.push_back(std::move(sub));
            }
        }

        // `tGeoSet0`'s run, then `tGeoSet1`'s — the order a renderer emits.
        for (const Emitted* emitted : {&first_, &second_}) {
            result.hidden.insert(result.hidden.end(), emitted->hidden.begin(),
                                 emitted->hidden.end());
            result.looks.insert(result.looks.end(), emitted->looks.begin(), emitted->looks.end());
            result.dyes.insert(result.dyes.end(), emitted->dyes.begin(), emitted->dyes.end());
            result.sourceSections.insert(result.sourceSections.end(), emitted->sections.begin(),
                                         emitted->sections.end());
        }

        if (clamped) {
            out_.warn(DiagCode::AttributeCountMismatch,
                      "a UV fell outside the [-64, 64) range a D3 texture coordinate encodes and "
                      "was clamped",
                      ElementRef(), ProfileId::Diablo3);
        }
    }

    /// One mesh's decoded vertex arrays. A struct because a `FatVertex` has
    /// eight of them, and passing eight parallel vectors down a call is how one
    /// ends up indexed against another's bound.
    struct Slices {
        const std::vector<Vector3f>& positions;
        const std::vector<Vector3f>& normals;
        const std::vector<Vector2f>& uv0;
        const std::vector<Vector2f>& uv1;
        const std::vector<Vector4f>& tangents;
        const std::vector<Vector3f>& binormals;
        const std::vector<Vector4f>& color0;
        const std::vector<Vector4f>& color1;
        const std::vector<std::array<u32, 4>>& boneIndices;
        const std::vector<std::array<f32, 4>>& boneWeights;
    };

    void writeSubObject(const geom::RenderMesh& render, const geom::RenderRange& range,
                        const Slices& slices, const MeshSection* section, d3n::SubObject& sub,
                        bool& clamped) {
        const std::vector<Vector3f>& positions = slices.positions;
        const std::vector<std::array<u32, 4>>& boneIndices = slices.boneIndices;
        const std::vector<std::array<f32, 4>>& boneWeights = slices.boneWeights;
        // **A sub-object owns its vertices.** Its face corners are `u16` into
        // its own array, so the range gets a disjoint slice in first-use order
        // — the same rule `toM3` follows for a region, and for the same reason.
        std::vector<u32> localOf(positions.size(), kInvalidIndex);
        std::vector<u32> sourceOf;
        sub.arIndices.reserve(range.indexCount);
        for (u32 i = 0; i < range.indexCount; ++i) {
            const u32 index = render.indices[range.firstIndex + i];
            if (index >= localOf.size()) {
                sub.arIndices.push_back(0);
                continue;
            }
            if (localOf[index] == kInvalidIndex) {
                localOf[index] = static_cast<u32>(sourceOf.size());
                sourceOf.push_back(index);
            }
            sub.arIndices.push_back(static_cast<u16>(localOf[index]));
        }
        if (sourceOf.size() > 0x10000u) {
            out_.warn(DiagCode::IndexWidthExceeded,
                      "sub-object '" + sub.szName + "' needs " + std::to_string(sourceOf.size()) +
                          " vertices, past the u16 a D3 face corner is",
                      ElementRef(), ProfileId::Diablo3);
        }

        // §5.6: a rigid section names one node and every vertex binds there, so
        // it ships `nBoneIndex` and no influence array at all — 91% of the
        // shipped corpus. Anything else writes three influences per vertex.
        const bool rigid = section != nullptr && section->rigidNode.has_value();
        // Every sub-object names a bone, rigid or not — what `rigidNode` adds is
        // that the name is the whole binding. The skinned ones' node came in on
        // the section's native bag, and dropping it moves a skinned sub-object's
        // fallback frame to the root.
        const u32 node =
            rigid ? *section->rigidNode
                  : (section != nullptr ? static_cast<u32>(section->native.value("boneNode", -1))
                                        : kInvalidIndex);
        sub.nBoneIndex = node < boneOf_.size() && boneOf_[node] != kInvalidIndex
                             ? static_cast<i32>(boneOf_[node])
                             : -1;

        // `UInt8` is an integer encoding, so a colour comes back 0..255 and the
        // components already are the bytes the blob wants. A mesh with no colour
        // layer gets opaque white, which is the identity for both channels.
        const auto colorAt = [](const std::vector<Vector4f>& layer, u32 index) -> u32 {
            if (index >= layer.size()) {
                return 0xFFFFFFFFu;
            }
            std::array<u8, 4> bytes{};
            for (std::size_t c = 0; c < 4; ++c) {
                bytes[c] = static_cast<u8>(std::clamp(layer[index].data[c], 0.0f, 255.0f) + 0.5f);
            }
            return PackVertexColor(bytes);
        };

        sub.arVertices.reserve(sourceOf.size());
        for (const u32 source : sourceOf) {
            d3n::FatVertex vertex;
            vertex.vPosition = positions[source];
            vertex.dwNormal = PackVertexVector(
                source < slices.normals.size() ? slices.normals[source] : Vector3f{0, 0, 1});
            vertex.dwTexCoord0 = PackTexCoord(
                source < slices.uv0.size() ? slices.uv0[source] : Vector2f{0, 0}, clamped);
            vertex.dwTexCoord1 = PackTexCoord(
                source < slices.uv1.size() ? slices.uv1[source] : Vector2f{0, 0}, clamped);
            vertex.dwTangent = source < slices.tangents.size()
                                   ? PackVertexVector(Vector3f{slices.tangents[source].x,
                                                               slices.tangents[source].y,
                                                               slices.tangents[source].z})
                                   : PackVertexVector(Vector3f{1, 0, 0});
            vertex.dwBinormal = PackVertexVector(
                source < slices.binormals.size() ? slices.binormals[source] : Vector3f{0, 1, 0});
            vertex.dwColor = colorAt(slices.color0, source);
            vertex.dwAuxColor = colorAt(slices.color1, source);
            sub.arVertices.push_back(vertex);
        }

        if (!rigid) {
            sub.arVertexInfluences.reserve(sourceOf.size());
            for (const u32 source : sourceOf) {
                d3n::VertInfluences influences;
                d3n::Influence* three[3] = {&influences.tInfluence0, &influences.tInfluence1,
                                            &influences.tInfluence2};
                for (std::size_t k = 0; k < 3; ++k) {
                    three[k]->nBoneIndex = 0;
                    three[k]->flWeight = 0.0f;
                    if (source >= boneIndices.size() || source >= boneWeights.size() ||
                        boneWeights[source][k] <= 0.0f) {
                        continue;
                    }
                    const u32 node = boneIndices[source][k];
                    const u32 bone = node < boneOf_.size() ? boneOf_[node] : kInvalidIndex;
                    if (bone == kInvalidIndex) {
                        continue;
                    }
                    three[k]->nBoneIndex = static_cast<i32>(bone);
                    three[k]->flWeight = boneWeights[source][k];
                }
                sub.arVertexInfluences.push_back(influences);
            }
        }

        sub.dwVertexCount = static_cast<i32>(sub.arVertices.size());
        sub.dwIndexCount = static_cast<i32>(sub.arIndices.size());
    }

    const Document& document_;
    const Model& model_;
    const ProfileMaterialSet& set_;
    Diagnostics& out_;
    /// One geoset's worth of the per-sub-object lists, accumulated while the
    /// meshes are walked and concatenated in geoset order at the end.
    struct Emitted {
        std::vector<u8> hidden;
        std::vector<u32> looks;
        std::vector<i32> dyes;
        std::vector<std::pair<u32, u32>> sections;
    };
    Emitted first_;
    Emitted second_;

    std::vector<u32> boneOf_;
    u32 droppedBoneShapes_ = 0;
    u32 clothSections_ = 0;
    u32 overrides_ = 0;
    u32 dyed_ = 0;
};

} // namespace

Result<D3AppearanceExport> D3Converter::toAppearance(const Document& document,
                                                     const D3ExportOptions& options) const {
    Result<D3AppearanceExport> result;
    if (options.model >= document.models.size()) {
        result.diagnostics.error(DiagCode::ClipTargetMissing,
                                 "model " + std::to_string(options.model) + " of " +
                                     std::to_string(document.models.size()));
        return result;
    }
    const Model& model = document.models[options.model];
    const ProfileMaterialSet* set = model.setFor(ProfileId::Diablo3);
    if (set == nullptr) {
        result.diagnostics.error(DiagCode::ProfileNotCarried,
                                 "the model carries no Diablo III material set; derive one first",
                                 ElementRef(), ProfileId::Diablo3);
        return result;
    }

    u32 look = set->defaultLook;
    if (!options.look.empty()) {
        const u32 named = set->looks.find(options.look);
        if (named == kInvalidIndex) {
            result.diagnostics.warn(DiagCode::LookDropped,
                                    "look '" + options.look + "' is not in this set", ElementRef(),
                                    ProfileId::Diablo3);
        } else {
            look = named;
        }
    }

    result.value = AppearanceWriter(document, model, *set, result.diagnostics).run(look);
    return result;
}

// ============================================================================
// toAnimSet
// ============================================================================

Result<D3Converter::D3AnimExport> D3Converter::toAnimSet(const Document& document,
                                                         u32 model) const {
    Result<D3AnimExport> result;
    if (model >= document.models.size()) {
        result.diagnostics.error(DiagCode::ClipTargetMissing,
                                 "model " + std::to_string(model) + " of " +
                                     std::to_string(document.models.size()));
        return result;
    }

    D3AnimExport out;
    out.anims = d3_anim::ExportAnims(document, model, result.diagnostics);

    // The `.ani` a clip index belongs to, so a tag row can name it. Built from
    // the same walk `ExportAnims` made, and by the same rule, because the two
    // disagreeing would point a tag at an animation that is not there.
    std::vector<i32> animOfClip(document.clips.size(), -1);
    {
        i32 synthetic = d3_anim::kSyntheticAnimBase;
        for (std::size_t c = 0; c < document.clips.size(); ++c) {
            const Clip& clip = document.clips[c];
            if (clip.model != model &&
                !(clip.model == kInvalidIndex && document.models.size() == 1)) {
                continue;
            }
            const i32 id = static_cast<i32>(clip.native.value("animSnoId", -1));
            animOfClip[c] = id >= 0 ? id : synthetic++;
        }
    }

    const Model& source = document.models[model];
    const u32 core = source.animSet;
    if (core == kInvalidIndex || core >= document.animSets.size()) {
        result.value = std::move(out);
        return result;
    }
    out.animSet.dwSnoId =
        static_cast<i32>(source.setFor(ProfileId::Diablo3) != nullptr
                             ? source.setFor(ProfileId::Diablo3)->native.value("animSetSnoId", -1)
                             : -1);

    // Back into the 30 tag maps the import spread them over. The join is the
    // set's `name`, which is the map's own — an `.ans` numbers its maps and this
    // library names them, and a name is what survives an edit that reorders
    // `Document::animSets`.
    u32 unnamed = 0;
    for (std::size_t s = 0; s < document.animSets.size(); ++s) {
        const AnimSet& set = document.animSets[s];
        if (s != core && set.baseAnimSet != core) {
            continue; // Another model's set.
        }
        std::vector<d3n::AnimSetTagMapEntry>* target = nullptr;
        for (const TagMap& map : kTagMaps) {
            if (set.name == map.name) {
                target = &(out.animSet.*(map.field));
                break;
            }
        }
        if (target == nullptr) {
            ++unnamed;
            continue;
        }
        for (const AnimTag& tag : set.byTag) {
            if (tag.clip >= animOfClip.size() || animOfClip[tag.clip] < 0) {
                continue;
            }
            d3n::AnimSetTagMapEntry row;
            row.dwTagId = static_cast<i32>(tag.tagId);
            row.snoAnim.id = animOfClip[tag.clip];
            row.snoAnim.group = d3n::Group::Anim;
            target->push_back(row);
        }
    }
    if (unnamed != 0) {
        result.diagnostics.warn(DiagCode::AnimTrackDropped,
                                std::to_string(unnamed) +
                                    " anim sets name no `.ans` tag map and were not written",
                                ElementRef(), ProfileId::Diablo3);
    }

    result.value = std::move(out);
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
