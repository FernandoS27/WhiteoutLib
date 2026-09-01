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
 */

#include "whiteout/models/wem/d3_converter.h"
#include "whiteout/models/wem/geometry/builder.h"

#include <whiteout/sno/d3/native/geometry.h>

#include "../materials/d3_core.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
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
    out.rotation = Quaternion{source.qRotation.x, source.qRotation.y, source.qRotation.z,
                              source.qRotation.w};
    out.scale = Vector3f{source.flScale, source.flScale, source.flScale};
    return out;
}

Transform ToTransform(const d3n::PRTransform& source) {
    Transform out;
    out.translation = source.vTranslation;
    out.rotation = Quaternion{source.qRotation.x, source.qRotation.y, source.qRotation.z,
                              source.qRotation.w};
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
            node.native.set("collisionShapeCount",
                            static_cast<i64>(bone.arCollisionShapes.size()));
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
        node.native.set("lightType", static_cast<i64>(light.nType));
        node.native.set("lightFlags", static_cast<i64>(light.dwFlags));
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
                builder.setCornerAttr(face, c, geom::names::kTangent,
                                      d3n::vertexTangent(vertex));
                builder.setCornerAttr(face, c, geom::names::kBinormal,
                                      d3n::vertexBinormal(vertex));
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
    const std::string wanted = options.materialLook.empty() ? kDefaultLookName
                                                            : options.materialLook;
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
                material.arVariants[look],
                material.szName + "#" + set.looks.looks[look].name, context, diagnostics);
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
                                     AssetSource& assets,
                                     const D3ImportOptions& options) const {
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
                set != nullptr &&
                (wantedLook.empty() ||
                 (set->defaultLook < set->looks.size() &&
                  IEquals(set->looks.looks[set->defaultLook].name, wantedLook)));
            if (sameLook) {
                result.value = existing;
                return result;
            }
        }
    }

    const d3n::Appearances* appearance = assets.appearance(source.snoAppearance.id);
    if (appearance == nullptr) {
        diagnostics.error(DiagCode::AssetUnresolved,
                          "appearance " + std::to_string(source.snoAppearance.id) +
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
    const u32 index = static_cast<u32>(document.models.size());
    document.declare(ProfileId::Diablo3);
    document.models.push_back(std::move(model));
    result.value = index;

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
                diagnostics.warn(DiagCode::HardpointUnresolved,
                                 "event names hardpoint '" + wanted + "', which the model has no "
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

} // namespace wem
} // namespace models
} // namespace whiteout
