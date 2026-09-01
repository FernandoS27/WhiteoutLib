// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file mdx_converter.cpp
 * @brief `.mdx` <-> `Document` (design §14, §10.7, §5.11).
 *
 * ### One file, two profiles
 *
 * The classic/Reforged split is per *layer*, so a single `.mdx` material can
 * feed a `Wc3Classic` set and a `Wc3Reforged` set at once. Import therefore does
 * not choose a profile: it asks `mdx_core::HasLayersFor` per material per
 * profile, declares only the profiles something actually answers for, and gives
 * each section a mask naming the profiles its material can be drawn in. A
 * classic-only file produces one set, not an empty second one.
 *
 * ### The pivot is the rest pose
 *
 * MDX composes a node pivot-relative — `T(-pivot) * S * R * T(pivot + t)` — so
 * with rest-pose tracks a node's world translation *is* its `PIVT` entry, which
 * is absolute model space. WEM's `worldBind` composes `local` down the chain, so
 * `local.translation` imports as `pivot - parentPivot` and `pivot` is carried
 * verbatim beside it. Both are needed: the difference reconstructs the bind
 * pose, and the absolute value is what export writes back and what an animation
 * evaluator needs to reproduce the pivot-relative composition.
 */

#include "whiteout/models/mdx/parser.h"
#include "whiteout/models/mdx/writer.h"
#include "whiteout/models/wem/converters.h"
#include "whiteout/models/wem/geometry/builder.h"
#include "whiteout/models/wem/geometry/render_view.h"

#include "../materials/mdx_core.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>

namespace whiteout {
namespace models {
namespace wem {

namespace {

constexpr ProfileId kMdxProfiles[] = {ProfileId::Wc3Classic, ProfileId::Wc3Reforged};

Extent ToExtent(const mdx::Extent& source) {
    Extent out;
    out.minimum = source.minimum;
    out.maximum = source.maximum;
    out.sphereRadius = source.boundsRadius;
    return out;
}

mdx::Extent FromExtent(const Extent& source) {
    mdx::Extent out;
    out.minimum = source.minimum;
    out.maximum = source.maximum;
    out.boundsRadius = source.sphereRadius;
    return out;
}

std::string SlotName(std::size_t materialIndex) {
    return "material_" + std::to_string(materialIndex);
}

// ============================================================================
// Nodes
// ============================================================================

/// Where a node came from, so its payload can be filled without a second pass
/// over every MDX array.
enum class Origin : u8 {
    Helper,
    Bone,
    Light,
    Attachment,
    ParticleEmitter,
    ParticleEmitter2,
    CornEmitter,
    RibbonEmitter,
    Event,
    Collision,
};

struct PendingNode {
    const mdx::Node* source = nullptr;
    Origin origin = Origin::Helper;
    u32 sourceIndex = 0;
};

NodeKind KindOf(Origin origin) {
    switch (origin) {
    case Origin::Bone:
        return NodeKind::Bone;
    case Origin::Light:
        return NodeKind::Light;
    case Origin::Attachment:
        return NodeKind::Attachment;
    case Origin::ParticleEmitter:
    case Origin::ParticleEmitter2:
    case Origin::CornEmitter:
        return NodeKind::ParticleEmitter;
    case Origin::RibbonEmitter:
        return NodeKind::RibbonEmitter;
    case Origin::Event:
        return NodeKind::Event;
    case Origin::Collision:
        return NodeKind::CollisionShape;
    case Origin::Helper:
        break;
    }
    return NodeKind::Helper;
}

/// The inherit and billboard bits, which M3 spells the same way. Everything else
/// in `NodeFlag` is per-kind alias territory and rides in `native` as the raw
/// value, because bit 0x8000 means three different things depending on the kind.
NodeFlags ToNodeFlags(mdx::Node::NodeFlag source) {
    const u32 bits = static_cast<u32>(source);
    NodeFlags out = NodeFlags::None;
    if (bits & static_cast<u32>(mdx::Node::NodeFlag::DontInheritTranslation)) {
        out |= NodeFlags::DontInheritTranslation;
    }
    if (bits & static_cast<u32>(mdx::Node::NodeFlag::DontInheritRotation)) {
        out |= NodeFlags::DontInheritRotation;
    }
    if (bits & static_cast<u32>(mdx::Node::NodeFlag::DontInheritScaling)) {
        out |= NodeFlags::DontInheritScale;
    }
    if (bits & static_cast<u32>(mdx::Node::NodeFlag::Billboarded)) {
        out |= NodeFlags::Billboarded;
    }
    if (bits & static_cast<u32>(mdx::Node::NodeFlag::BillboardedLockX)) {
        out |= NodeFlags::BillboardLockX;
    }
    if (bits & static_cast<u32>(mdx::Node::NodeFlag::BillboardedLockY)) {
        out |= NodeFlags::BillboardLockY;
    }
    if (bits & static_cast<u32>(mdx::Node::NodeFlag::BillboardedLockZ)) {
        out |= NodeFlags::BillboardLockZ;
    }
    if (bits & static_cast<u32>(mdx::Node::NodeFlag::ModelSpace)) {
        out |= NodeFlags::ModelSpace;
    }
    return out;
}

mdx::Node::NodeFlag FromNodeFlags(NodeFlags source, u32 rawFallback) {
    // The raw value round-trips the per-kind aliases; the shared bits are
    // rewritten from `NodeFlags` so an edit to those survives.
    u32 bits = rawFallback;
    constexpr u32 kShared = static_cast<u32>(mdx::Node::NodeFlag::DontInheritTranslation) |
                            static_cast<u32>(mdx::Node::NodeFlag::DontInheritRotation) |
                            static_cast<u32>(mdx::Node::NodeFlag::DontInheritScaling) |
                            static_cast<u32>(mdx::Node::NodeFlag::Billboarded) |
                            static_cast<u32>(mdx::Node::NodeFlag::BillboardedLockX) |
                            static_cast<u32>(mdx::Node::NodeFlag::BillboardedLockY) |
                            static_cast<u32>(mdx::Node::NodeFlag::BillboardedLockZ) |
                            static_cast<u32>(mdx::Node::NodeFlag::ModelSpace);
    bits &= ~kShared;
    if (hasFlag(source, NodeFlags::DontInheritTranslation)) {
        bits |= static_cast<u32>(mdx::Node::NodeFlag::DontInheritTranslation);
    }
    if (hasFlag(source, NodeFlags::DontInheritRotation)) {
        bits |= static_cast<u32>(mdx::Node::NodeFlag::DontInheritRotation);
    }
    if (hasFlag(source, NodeFlags::DontInheritScale)) {
        bits |= static_cast<u32>(mdx::Node::NodeFlag::DontInheritScaling);
    }
    if (hasFlag(source, NodeFlags::Billboarded)) {
        bits |= static_cast<u32>(mdx::Node::NodeFlag::Billboarded);
    }
    if (hasFlag(source, NodeFlags::BillboardLockX)) {
        bits |= static_cast<u32>(mdx::Node::NodeFlag::BillboardedLockX);
    }
    if (hasFlag(source, NodeFlags::BillboardLockY)) {
        bits |= static_cast<u32>(mdx::Node::NodeFlag::BillboardedLockY);
    }
    if (hasFlag(source, NodeFlags::BillboardLockZ)) {
        bits |= static_cast<u32>(mdx::Node::NodeFlag::BillboardedLockZ);
    }
    if (hasFlag(source, NodeFlags::ModelSpace)) {
        bits |= static_cast<u32>(mdx::Node::NodeFlag::ModelSpace);
    }
    return static_cast<mdx::Node::NodeFlag>(bits);
}

/// Every MDX node chunk, in `objectId` order.
///
/// The order matters twice: `PIVT` is indexed by `objectId`, and the node array
/// has to keep parents before children for `NodeTree`'s hierarchy view. MDX
/// numbers nodes depth-first from the roots, so `objectId` order gives both.
std::vector<PendingNode> CollectNodes(const mdx::Model& source) {
    std::vector<PendingNode> pending;
    const auto push = [&pending](const mdx::Node& node, Origin origin, std::size_t index) {
        pending.push_back({&node, origin, static_cast<u32>(index)});
    };

    for (std::size_t i = 0; i < source.bones.size(); ++i) {
        push(source.bones[i].node, Origin::Bone, i);
    }
    for (std::size_t i = 0; i < source.helpers.size(); ++i) {
        push(source.helpers[i].node, Origin::Helper, i);
    }
    for (std::size_t i = 0; i < source.lights.size(); ++i) {
        push(source.lights[i].node, Origin::Light, i);
    }
    for (std::size_t i = 0; i < source.attachments.size(); ++i) {
        push(source.attachments[i].node, Origin::Attachment, i);
    }
    for (std::size_t i = 0; i < source.particleEmitters.size(); ++i) {
        push(source.particleEmitters[i].node, Origin::ParticleEmitter, i);
    }
    for (std::size_t i = 0; i < source.particleEmitters2.size(); ++i) {
        push(source.particleEmitters2[i].node, Origin::ParticleEmitter2, i);
    }
    for (std::size_t i = 0; i < source.cornEmitters.size(); ++i) {
        push(source.cornEmitters[i].node, Origin::CornEmitter, i);
    }
    for (std::size_t i = 0; i < source.ribbonEmitters.size(); ++i) {
        push(source.ribbonEmitters[i].node, Origin::RibbonEmitter, i);
    }
    for (std::size_t i = 0; i < source.eventObjects.size(); ++i) {
        push(source.eventObjects[i].node, Origin::Event, i);
    }
    for (std::size_t i = 0; i < source.collisionShapes.size(); ++i) {
        push(source.collisionShapes[i].node, Origin::Collision, i);
    }

    std::stable_sort(pending.begin(), pending.end(),
                     [](const PendingNode& a, const PendingNode& b) {
                         return a.source->objectId < b.source->objectId;
                     });
    return pending;
}

void FillPayload(const mdx::Model& source, const PendingNode& pending, Node& node) {
    node.kind = KindOf(pending.origin);
    node.resetPayloadForKind();

    switch (pending.origin) {
    case Origin::Bone: {
        const mdx::Bone& bone = source.bones[pending.sourceIndex];
        node.native.set("geosetId", static_cast<i64>(bone.geosetId));
        node.native.set("geosetAnimationId", static_cast<i64>(bone.geosetAnimationId));
        break;
    }
    case Origin::Light: {
        const mdx::Light& light = source.lights[pending.sourceIndex];
        auto& payload = std::get<LightPayload>(node.payload);
        switch (light.type) {
        case mdx::Light::LightType::Directional:
            payload.kind = LightKind::Directional;
            break;
        case mdx::Light::LightType::Ambient:
            payload.kind = LightKind::Ambient;
            break;
        case mdx::Light::LightType::Omni:
        default:
            payload.kind = LightKind::Omni;
            break;
        }
        payload.color = light.color;
        payload.intensity = light.intensity;
        payload.attenuationStart = light.attenuationStart;
        payload.attenuationEnd = light.attenuationEnd;
        node.native.set("ambientIntensity", static_cast<i64>(light.ambientIntensity * 1000.0f));
        node.native.set("shadowIntensity", static_cast<i64>(light.shadowIntensity * 1000.0f));
        break;
    }
    case Origin::Attachment: {
        const mdx::Attachment& attachment = source.attachments[pending.sourceIndex];
        node.native.set("attachmentId", static_cast<i64>(attachment.attachmentId));
        break;
    }
    case Origin::ParticleEmitter: {
        const mdx::ParticleEmitter& emitter = source.particleEmitters[pending.sourceIndex];
        std::get<ParticlePayload>(node.payload).system.path = emitter.spawnModelFileName;
        break;
    }
    case Origin::ParticleEmitter2: {
        const mdx::ParticleEmitter2& emitter = source.particleEmitters2[pending.sourceIndex];
        std::get<ParticlePayload>(node.payload).system.id = emitter.textureId;
        break;
    }
    case Origin::CornEmitter: {
        const mdx::CornEmitter& emitter = source.cornEmitters[pending.sourceIndex];
        std::get<ParticlePayload>(node.payload).system.path = emitter.path;
        break;
    }
    case Origin::RibbonEmitter: {
        const mdx::RibbonEmitter& emitter = source.ribbonEmitters[pending.sourceIndex];
        std::get<RibbonPayload>(node.payload).system.id = emitter.materialId;
        node.native.set("textureSlot", static_cast<i64>(emitter.textureSlot));
        break;
    }
    case Origin::Event: {
        const mdx::EventObject& event = source.eventObjects[pending.sourceIndex];
        std::get<EventPayload>(node.payload).id = event.globalSequenceId;
        break;
    }
    case Origin::Collision: {
        const mdx::CollisionShape& shape = source.collisionShapes[pending.sourceIndex];
        auto& payload = std::get<CollisionPayload>(node.payload);
        switch (shape.type) {
        case mdx::CollisionShape::ShapeType::Sphere:
            payload.shape.kind = CollisionShapeKind::Sphere;
            payload.shape.sphere.radius = shape.radius;
            if (!shape.vertices.empty()) {
                payload.shape.sphere.center = shape.vertices[0];
            }
            break;
        case mdx::CollisionShape::ShapeType::Plane:
            payload.shape.kind = CollisionShapeKind::Plane;
            break;
        case mdx::CollisionShape::ShapeType::Cylinder:
            payload.shape.kind = CollisionShapeKind::Cylinder;
            payload.shape.sphere.radius = shape.radius;
            break;
        case mdx::CollisionShape::ShapeType::Box:
        default:
            payload.shape.kind = CollisionShapeKind::Box;
            break;
        }
        if (shape.vertices.size() >= 2) {
            payload.shape.box.minimum = shape.vertices[0];
            payload.shape.box.maximum = shape.vertices[1];
        }
        break;
    }
    case Origin::Helper:
        break;
    }
}

/// The node tree, plus the `objectId` -> node index map every other importer
/// step joins through.
struct NodeImport {
    NodeTree tree;
    std::unordered_map<u32, u32> byObjectId;

    u32 resolve(u32 objectId) const {
        const auto it = byObjectId.find(objectId);
        return it == byObjectId.end() ? kInvalidNode : it->second;
    }
};

NodeImport ImportNodes(const mdx::Model& source) {
    NodeImport out;
    const std::vector<PendingNode> pending = CollectNodes(source);

    out.tree.poseSchema.push_back(PoseSchema{});
    out.tree.authoritativePose = 0;

    for (std::size_t i = 0; i < pending.size(); ++i) {
        out.byObjectId.emplace(pending[i].source->objectId, static_cast<u32>(i));
    }

    for (const PendingNode& item : pending) {
        const mdx::Node& mdxNode = *item.source;
        Node node;
        node.name = mdxNode.name;
        node.flags = ToNodeFlags(mdxNode.flags);
        if (mdxNode.objectId < source.pivotPoints.size()) {
            node.pivot = source.pivotPoints[mdxNode.objectId];
        }
        node.parent =
            mdxNode.parentId == mdx::Node::NO_PARENT ? kInvalidNode : out.resolve(mdxNode.parentId);
        node.native.set("objectId", static_cast<i64>(mdxNode.objectId));
        node.native.set("nodeFamilyId", static_cast<i64>(mdxNode.nodeFamilyId));
        node.native.set("flagBits", static_cast<i64>(static_cast<u32>(mdxNode.flags)));
        FillPayload(source, item, node);

        // See the file comment: the rest pose is the pivot, so the local
        // translation is the pivot difference and the rotation and scale are
        // identity until P7 imports the tracks.
        Vector3f parentPivot{0, 0, 0};
        if (node.parent != kInvalidNode && node.parent < out.tree.size()) {
            parentPivot = out.tree.nodes[node.parent].pivot;
        }
        node.local.translation =
            Vector3f{node.pivot.x - parentPivot.x, node.pivot.y - parentPivot.y,
                     node.pivot.z - parentPivot.z};
        node.poses.push_back(node.local);
        out.tree.add(std::move(node));
    }

    // Cameras are not node chunks in MDX -- they carry a position and no
    // `objectId` -- so they become parentless Camera nodes after the numbered
    // ones, where they cannot disturb the `objectId` join.
    for (const mdx::Camera& camera : source.cameras) {
        Node node;
        node.name = camera.name;
        node.kind = NodeKind::Camera;
        node.resetPayloadForKind();
        auto& payload = std::get<CameraPayload>(node.payload);
        payload.fov = camera.fieldOfView;
        payload.nearClip = camera.nearClippingPlane;
        payload.farClip = camera.farClippingPlane;
        node.local.translation = camera.position;
        node.poses.push_back(node.local);
        out.tree.add(std::move(node));
    }

    return out;
}

// ============================================================================
// Geometry
// ============================================================================

/// Feeds one geoset's skinning into @p builder.
///
/// Both MDX conventions land here. Reforged's `skinData` is four
/// (index, weight/255) pairs per vertex, where the index goes through
/// `matrixIndices` when that array is present. Classic's is a per-vertex group
/// id naming a run of `matrixIndices`, every member weighted equally — a
/// matrix *group*, not a weight list. WEM stores the run at 1/N rather than the
/// renderer's averaged pseudo-bone, because `SkinBinding` is variable width and
/// `maxBoneInfluences` is a limit export checks, not one import enforces.
void ImportSkin(const mdx::Geoset& geoset, const NodeImport& nodes, geom::MeshBuilder& builder,
                std::size_t vertexCount, Diagnostics& out, std::size_t geosetIndex) {
    const auto resolveMatrix = [&](u32 raw) -> u32 {
        const u32 objectId = raw < geoset.matrixIndices.size() ? geoset.matrixIndices[raw] : raw;
        const u32 node = nodes.resolve(objectId);
        if (node == kInvalidNode) {
            out.warn(DiagCode::DanglingNodeReference,
                     "geoset matrix names object id " + std::to_string(objectId) +
                         ", which is not a node",
                     ElementRef(ElementKind::Mesh, geosetIndex));
        }
        return node;
    };

    if (!geoset.skinData.empty()) {
        for (std::size_t v = 0; v < vertexCount; ++v) {
            const std::size_t base = v * 8;
            if (base + 7 >= geoset.skinData.size()) {
                break;
            }
            for (std::size_t k = 0; k < 4; ++k) {
                const f32 weight = static_cast<f32>(geoset.skinData[base + 4 + k]) / 255.0f;
                if (weight <= 0.0f) {
                    continue;
                }
                const u32 node = resolveMatrix(geoset.skinData[base + k]);
                if (node != kInvalidNode) {
                    builder.addInfluence(geom::VertexId(static_cast<u32>(v)), node, weight);
                }
            }
        }
        return;
    }

    if (geoset.vertexGroups.empty() || geoset.matrixGroups.empty()) {
        return;
    }

    std::vector<u32> groupStart(geoset.matrixGroups.size() + 1, 0);
    for (std::size_t g = 0; g < geoset.matrixGroups.size(); ++g) {
        groupStart[g + 1] = groupStart[g] + geoset.matrixGroups[g];
    }

    for (std::size_t v = 0; v < vertexCount && v < geoset.vertexGroups.size(); ++v) {
        const u32 group = geoset.vertexGroups[v];
        if (group >= geoset.matrixGroups.size()) {
            continue;
        }
        const u32 count = geoset.matrixGroups[group];
        if (count == 0) {
            continue;
        }
        const f32 weight = 1.0f / static_cast<f32>(count);
        for (u32 k = 0; k < count && (groupStart[group] + k) < geoset.matrixIndices.size(); ++k) {
            const u32 node = resolveMatrix(groupStart[group] + k);
            if (node != kInvalidNode) {
                builder.addInfluence(geom::VertexId(static_cast<u32>(v)), node, weight);
            }
        }
    }
}

} // namespace

// ============================================================================
// fromMdx
// ============================================================================

Result<Document> MdxConverter::fromMdx(const mdx::Model& source) const {
    Result<Document> result;
    Document document;
    Diagnostics& diagnostics = result.diagnostics;

    document.name = source.modelName;
    document.bounds = ToExtent(source.modelExtent);
    document.space = CoordSpace::Blizzard;

    // --- textures: document-wide, shared by both profiles (§6.3) ------------
    mdx_core::Context context;
    context.modelVersion = source.version;
    document.textures.reserve(source.textures.size());
    for (const mdx::Texture& texture : source.textures) {
        TextureRef ref;
        ref.path = texture.fileName;
        ref.flags = static_cast<u32>(texture.flags);
        ref.replaceableId = texture.replaceableId;
        context.textureIndexMap.push_back(static_cast<u32>(document.textures.size()));
        document.textures.push_back(std::move(ref));
    }

    Model model;
    model.name = source.modelName;
    model.bounds = document.bounds;

    const NodeImport nodes = ImportNodes(source);
    model.nodes = nodes.tree;

    // --- slots: one per source material, whatever any profile makes of it ---
    model.materialSlots.reserve(source.materials.size());
    for (std::size_t m = 0; m < source.materials.size(); ++m) {
        model.materialSlots.push_back(SlotName(m));
    }

    // --- which profiles this file actually serves ---------------------------
    std::vector<ProfileMask> slotProfiles(source.materials.size(), kNoProfiles);
    ProfileMask documentMask = kNoProfiles;
    for (std::size_t m = 0; m < source.materials.size(); ++m) {
        for (ProfileId profile : kMdxProfiles) {
            if (mdx_core::HasLayersFor(source.materials[m], profile, context)) {
                slotProfiles[m] |= ProfileBit(profile);
                documentMask |= ProfileBit(profile);
            }
        }
    }
    // A file whose every material is empty still has geometry, and dropping it
    // would be worse than drawing it untextured -- so it imports as classic.
    if (documentMask == kNoProfiles) {
        documentMask = ProfileBit(ProfileId::Wc3Classic);
    }
    for (ProfileId profile : kMdxProfiles) {
        if (HasProfile(documentMask, profile)) {
            document.declare(profile);
        }
    }
    document.defaultProfile = document.profiles.front();

    // --- geosets -> meshes --------------------------------------------------
    model.meshes.reserve(source.geosets.size());
    for (std::size_t g = 0; g < source.geosets.size(); ++g) {
        const mdx::Geoset& geoset = source.geosets[g];
        geom::MeshBuilder builder;

        MeshSection section;
        section.name = geoset.lodName.empty() ? "geoset_" + std::to_string(g) : geoset.lodName;
        section.selectionGroup = static_cast<u16>(geoset.selectionGroup);
        section.bounds = ToExtent(geoset.extent);
        if (geoset.materialId < model.materialSlots.size()) {
            section.materialSlot = geoset.materialId;
            section.profiles = slotProfiles[geoset.materialId];
            if (section.profiles == kNoProfiles) {
                section.profiles = documentMask;
            }
        } else {
            section.profiles = documentMask;
            diagnostics.warn(DiagCode::IndexOutOfRange,
                             "geoset names material " + std::to_string(geoset.materialId) +
                                 ", past the end of the material array",
                             ElementRef(ElementKind::Mesh, g));
        }
        section.native.set("selectionFlags", static_cast<i64>(geoset.selectionFlags));
        const u32 sectionIndex = builder.addSection(std::move(section));

        for (const Vector3f& position : geoset.vertexPositions) {
            builder.addVertex(position);
        }

        const std::size_t vertexCount = geoset.vertexPositions.size();
        const std::size_t triangles = geoset.faces.size() / 3;
        for (std::size_t t = 0; t < triangles; ++t) {
            const std::array<u32, 3> corners = {geoset.faces[t * 3 + 0], geoset.faces[t * 3 + 1],
                                                geoset.faces[t * 3 + 2]};
            if (corners[0] >= vertexCount || corners[1] >= vertexCount ||
                corners[2] >= vertexCount) {
                diagnostics.warn(DiagCode::IndexOutOfRange, "face corner past the vertex array",
                                 ElementRef(ElementKind::Mesh, g));
                continue;
            }
            const geom::FaceId face =
                builder.addTriangle(geom::VertexId(corners[0]), geom::VertexId(corners[1]),
                                    geom::VertexId(corners[2]), sectionIndex);
            for (u32 c = 0; c < 3; ++c) {
                const u32 vertex = corners[c];
                if (vertex < geoset.vertexNormals.size()) {
                    builder.setCornerAttr(face, c, geom::names::kNormal,
                                          geoset.vertexNormals[vertex]);
                }
                if (vertex < geoset.tangents.size()) {
                    builder.setCornerAttr(face, c, geom::names::kTangent, geoset.tangents[vertex]);
                }
                for (std::size_t uv = 0; uv < geoset.textureCoordinateSets.size(); ++uv) {
                    if (vertex < geoset.textureCoordinateSets[uv].size()) {
                        builder.setCornerAttr(face, c, geom::names::uv(static_cast<u32>(uv)),
                                              geoset.textureCoordinateSets[uv][vertex]);
                    }
                }
            }
        }

        ImportSkin(geoset, nodes, builder, vertexCount, diagnostics, g);

        geom::MeshBuilder::BuildOutcome outcome = builder.build();
        outcome.mesh.name = outcome.mesh.sections.empty() ? "geoset_" + std::to_string(g)
                                                          : outcome.mesh.sections[0].name;
        outcome.mesh.lodLevel = geoset.lod;
        outcome.mesh.bounds = ToExtent(geoset.extent);
        model.meshes.push_back(std::move(outcome.mesh));
    }

    // --- one material set per served profile --------------------------------
    for (ProfileId profile : document.profiles) {
        ProfileMaterialSet set;
        set.profile = profile;
        set.looks.looks.push_back(Look{});
        set.resizeBindings(model.materialSlots.size());

        for (std::size_t m = 0; m < source.materials.size(); ++m) {
            if (!HasProfile(slotProfiles[m], profile)) {
                continue;
            }
            const u32 index = static_cast<u32>(set.materials.size());
            set.materials.push_back(
                mdx_core::ImportMaterial(source.materials[m], profile, context, diagnostics));
            set.materials.back().name = SlotName(m);
            set.slotBindings[m].byLook[0] = index;
        }
        model.profileSets.push_back(std::move(set));
    }

    document.models.push_back(std::move(model));
    result.value = std::move(document);
    return result;
}

// ============================================================================
// toMdx
// ============================================================================

Result<mdx::Model> MdxConverter::toMdx(const Document& document, ProfileId profile,
                                       u32 targetVersion) const {
    Result<mdx::Model> result;
    if (!checkExportProfile(document, profile, result.diagnostics)) {
        return result;
    }
    if (document.models.empty()) {
        result.value = mdx::Model{};
        result.value->version = targetVersion;
        return result;
    }

    Diagnostics& diagnostics = result.diagnostics;
    const Model& model = document.models.front();
    const ProfileMaterialSet* set = model.setFor(profile);

    mdx::Model out;
    out.version = targetVersion;
    out.modelName = document.name.empty() ? model.name : document.name;
    out.modelExtent = FromExtent(model.bounds);

    // --- textures -----------------------------------------------------------
    mdx_core::Context context;
    context.modelVersion = targetVersion;
    out.textures.reserve(document.textures.size());
    for (const TextureRef& ref : document.textures) {
        mdx::Texture texture;
        texture.fileName = ref.path;
        texture.flags = static_cast<mdx::Texture::Flag>(ref.flags);
        texture.replaceableId = ref.replaceableId;
        context.textureIndexMap.push_back(static_cast<u32>(out.textures.size()));
        out.textures.push_back(std::move(texture));
    }

    // --- nodes --------------------------------------------------------------
    //
    // The node array is written back in WEM order and renumbered, because a
    // document that has been edited has no `objectId` left to trust. `PIVT` is
    // parallel to it, which is what makes the renumbering safe.
    out.pivotPoints.reserve(model.nodes.size());
    std::vector<u32> objectIdOf(model.nodes.size(), mdx::Node::NO_PARENT);
    u32 nextObjectId = 0;
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        if (model.nodes.nodes[i].kind == NodeKind::Camera) {
            continue; // Cameras are not node chunks; they are written below.
        }
        objectIdOf[i] = nextObjectId++;
    }

    const auto buildNode = [&](std::size_t index) {
        const Node& node = model.nodes.nodes[index];
        mdx::Node out_node;
        out_node.name = node.name;
        out_node.objectId = objectIdOf[index];
        out_node.parentId = node.parent == kInvalidNode || node.parent >= objectIdOf.size()
                                ? mdx::Node::NO_PARENT
                                : objectIdOf[node.parent];
        const NodeNative::Entry* raw = node.native.find("flagBits");
        out_node.flags = FromNodeFlags(node.flags, raw ? static_cast<u32>(raw->value) : 0u);
        const NodeNative::Entry* family = node.native.find("nodeFamilyId");
        out_node.nodeFamilyId = family ? static_cast<u32>(family->value) : 0u;
        return out_node;
    };

    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const Node& node = model.nodes.nodes[i];
        if (node.kind == NodeKind::Camera) {
            mdx::Camera camera;
            camera.name = node.name;
            camera.position = node.local.translation;
            if (const auto* payload = std::get_if<CameraPayload>(&node.payload)) {
                camera.fieldOfView = payload->fov;
                camera.nearClippingPlane = payload->nearClip;
                camera.farClippingPlane = payload->farClip;
            }
            out.cameras.push_back(std::move(camera));
            continue;
        }

        out.pivotPoints.push_back(node.pivot);
        switch (node.kind) {
        case NodeKind::Bone: {
            mdx::Bone bone;
            bone.node = buildNode(i);
            if (const auto* geosetId = node.native.find("geosetId")) {
                bone.geosetId = static_cast<u32>(geosetId->value);
            }
            if (const auto* animId = node.native.find("geosetAnimationId")) {
                bone.geosetAnimationId = static_cast<u32>(animId->value);
            }
            out.bones.push_back(std::move(bone));
            break;
        }
        case NodeKind::Light: {
            mdx::Light light;
            light.node = buildNode(i);
            if (const auto* payload = std::get_if<LightPayload>(&node.payload)) {
                switch (payload->kind) {
                case LightKind::Directional:
                    light.type = mdx::Light::LightType::Directional;
                    break;
                case LightKind::Ambient:
                    light.type = mdx::Light::LightType::Ambient;
                    break;
                case LightKind::Omni:
                case LightKind::Spot:
                default:
                    light.type = mdx::Light::LightType::Omni;
                    break;
                }
                light.color = payload->color;
                light.intensity = payload->intensity;
                light.attenuationStart = payload->attenuationStart;
                light.attenuationEnd = payload->attenuationEnd;
                if (payload->kind == LightKind::Spot) {
                    diagnostics.warn(DiagCode::FeatureDropped,
                                     "WC3 has no spot light; written as omni",
                                     ElementRef(ElementKind::Node, i));
                }
            }
            out.lights.push_back(std::move(light));
            break;
        }
        case NodeKind::Attachment: {
            mdx::Attachment attachment;
            attachment.node = buildNode(i);
            if (const auto* id = node.native.find("attachmentId")) {
                attachment.attachmentId = static_cast<u32>(id->value);
            }
            out.attachments.push_back(std::move(attachment));
            break;
        }
        case NodeKind::ParticleEmitter: {
            mdx::ParticleEmitter2 emitter;
            emitter.node = buildNode(i);
            if (const auto* payload = std::get_if<ParticlePayload>(&node.payload)) {
                if (payload->system.id != AssetKey::kNoId) {
                    emitter.textureId = payload->system.id;
                }
            }
            out.particleEmitters2.push_back(std::move(emitter));
            break;
        }
        case NodeKind::RibbonEmitter: {
            mdx::RibbonEmitter emitter;
            emitter.node = buildNode(i);
            if (const auto* payload = std::get_if<RibbonPayload>(&node.payload)) {
                if (payload->system.id != AssetKey::kNoId) {
                    emitter.materialId = payload->system.id;
                }
            }
            if (const auto* slot = node.native.find("textureSlot")) {
                emitter.textureSlot = static_cast<u32>(slot->value);
            }
            out.ribbonEmitters.push_back(std::move(emitter));
            break;
        }
        case NodeKind::Event: {
            mdx::EventObject event;
            event.node = buildNode(i);
            if (const auto* payload = std::get_if<EventPayload>(&node.payload)) {
                event.globalSequenceId = payload->id;
            }
            out.eventObjects.push_back(std::move(event));
            break;
        }
        case NodeKind::CollisionShape: {
            mdx::CollisionShape shape;
            shape.node = buildNode(i);
            if (const auto* payload = std::get_if<CollisionPayload>(&node.payload)) {
                switch (payload->shape.kind) {
                case CollisionShapeKind::Sphere:
                    shape.type = mdx::CollisionShape::ShapeType::Sphere;
                    shape.radius = payload->shape.sphere.radius;
                    shape.vertices.push_back(payload->shape.sphere.center);
                    break;
                case CollisionShapeKind::Plane:
                    shape.type = mdx::CollisionShape::ShapeType::Plane;
                    break;
                case CollisionShapeKind::Cylinder:
                    shape.type = mdx::CollisionShape::ShapeType::Cylinder;
                    shape.radius = payload->shape.sphere.radius;
                    break;
                case CollisionShapeKind::Capsule:
                case CollisionShapeKind::Hull:
                    diagnostics.warn(DiagCode::FeatureDropped,
                                     "WC3 has no capsule or hull collision shape; written as box",
                                     ElementRef(ElementKind::Node, i));
                    [[fallthrough]];
                case CollisionShapeKind::Box:
                default:
                    shape.type = mdx::CollisionShape::ShapeType::Box;
                    shape.vertices.push_back(payload->shape.box.minimum);
                    shape.vertices.push_back(payload->shape.box.maximum);
                    break;
                }
            }
            out.collisionShapes.push_back(std::move(shape));
            break;
        }
        case NodeKind::Helper:
        default: {
            mdx::Helper helper;
            helper.node = buildNode(i);
            out.helpers.push_back(std::move(helper));
            break;
        }
        }
    }

    // --- materials ----------------------------------------------------------
    //
    // One MDX material per WEM slot, so `geo.materialId` is the slot index and
    // no fix-up table is needed. A slot this profile does not bind writes an
    // empty material rather than shifting every later index.
    out.materials.reserve(model.materialSlots.size());
    for (std::size_t slot = 0; slot < model.materialSlots.size(); ++slot) {
        const Material* material = set ? Resolve(model, static_cast<u32>(slot), profile) : nullptr;
        if (material == nullptr) {
            out.materials.push_back(mdx::Material{});
            continue;
        }
        out.materials.push_back(mdx_core::ExportMaterial(*material, profile, context, diagnostics));
    }

    // --- meshes -> geosets --------------------------------------------------
    geom::RenderMeshDesc desc;
    desc.attributes = {
        {geom::names::kPosition, utils::AttributeClass::Position, utils::AttributeEncoding::Float32,
         3, 0},
        {geom::names::kNormal, utils::AttributeClass::Normal, utils::AttributeEncoding::Float32, 3,
         0},
        {geom::names::uv(0), utils::AttributeClass::UV, utils::AttributeEncoding::Float32, 2, 0},
    };
    desc.includeSkin = true;
    desc.maxInfluences = Profile(profile).maxBoneInfluences;
    desc.splitBySection = true;

    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        const geom::RenderMesh render = geom::BuildRenderMesh(mesh, desc);
        diagnostics.append(render.diagnostics);

        mdx::Geoset geoset;
        geoset.lodName = mesh.name;
        geoset.lod = mesh.lodLevel;
        geoset.extent = FromExtent(mesh.bounds);
        geoset.vertexPositions = render.vertices.getPositions();
        geoset.vertexNormals = render.vertices.getNormals();
        const std::vector<Vector2f> uv0 = render.vertices.getUVs(0);
        if (!uv0.empty()) {
            geoset.textureCoordinateSets.push_back(uv0);
        }

        geoset.faces.reserve(render.indices.size());
        for (u32 index : render.indices) {
            if (index > 0xFFFFu) {
                diagnostics.warn(DiagCode::IndexWidthExceeded,
                                 "mesh needs more than 65535 vertices for one geoset",
                                 ElementRef(ElementKind::Mesh, m));
                geoset.faces.push_back(0xFFFFu);
            } else {
                geoset.faces.push_back(static_cast<u16>(index));
            }
        }
        geoset.faceTypeGroups.push_back(4);
        geoset.faceGroups.push_back(static_cast<u32>(geoset.faces.size()));

        // The skin palette is per geoset: `matrixIndices` holds the object ids
        // this geoset binds to, and `skinData` addresses that array.
        const std::vector<std::array<u32, 4>> boneIndices = render.vertices.getBoneIndices();
        const std::vector<std::array<f32, 4>> boneWeights = render.vertices.getBoneWeights();
        if (!boneIndices.empty() && !boneWeights.empty()) {
            std::unordered_map<u32, u32> paletteOf;
            geoset.skinData.resize(boneIndices.size() * 8, 0);
            for (std::size_t v = 0; v < boneIndices.size(); ++v) {
                for (std::size_t k = 0; k < 4; ++k) {
                    const f32 weight = k < boneWeights[v].size() ? boneWeights[v][k] : 0.0f;
                    if (weight <= 0.0f) {
                        continue;
                    }
                    const u32 node = boneIndices[v][k];
                    const u32 objectId =
                        node < objectIdOf.size() ? objectIdOf[node] : mdx::Node::NO_PARENT;
                    if (objectId == mdx::Node::NO_PARENT) {
                        continue;
                    }
                    auto [entry, inserted] = paletteOf.try_emplace(
                        objectId, static_cast<u32>(geoset.matrixIndices.size()));
                    if (inserted) {
                        geoset.matrixIndices.push_back(objectId);
                    }
                    if (entry->second > 0xFFu) {
                        diagnostics.warn(DiagCode::BonePaletteLimit,
                                         "geoset needs more than 256 bones",
                                         ElementRef(ElementKind::Mesh, m));
                        continue;
                    }
                    geoset.skinData[v * 8 + k] = static_cast<u8>(entry->second);
                    geoset.skinData[v * 8 + 4 + k] =
                        static_cast<u8>(std::clamp(weight, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
            geoset.matrixGroups.push_back(static_cast<u32>(geoset.matrixIndices.size()));
        }

        if (!mesh.sections.empty()) {
            const MeshSection& section = mesh.sections.front();
            geoset.materialId = section.materialSlot;
            geoset.selectionGroup = section.selectionGroup;
            if (const auto* flags = section.native.find("selectionFlags")) {
                geoset.selectionFlags = static_cast<u32>(flags->value);
            }
        }

        out.geosets.push_back(std::move(geoset));
    }

    result.value = std::move(out);
    return result;
}

// ============================================================================
// FormatConverter
// ============================================================================

std::string MdxConverter::formatId() const {
    return "mdx";
}

std::string MdxConverter::formatName() const {
    return "Warcraft III MDX";
}

std::span<const ProfileId> MdxConverter::profiles() const {
    return kMdxProfiles;
}

bool MdxConverter::supportsImport() const {
    return true;
}

bool MdxConverter::supportsExport() const {
    return true;
}

u32 MdxConverter::defaultExportVersion() const {
    return 800;
}

Result<Document> MdxConverter::importFromBytes(std::span<const u8> data) const {
    mdx::Parser parser;
    const mdx::Model source = parser.parse(data);
    Result<Document> result = fromMdx(source);
    for (const std::string& issue : parser.getIssues()) {
        result.diagnostics.warn(DiagCode::Unspecified, issue);
    }
    return result;
}

Result<std::vector<u8>> MdxConverter::exportToBytes(const Document& document, ProfileId profile,
                                                    u32 version) const {
    Result<mdx::Model> converted =
        toMdx(document, profile, version == 0 ? defaultExportVersion() : version);
    Result<std::vector<u8>> result;
    result.diagnostics = std::move(converted.diagnostics);
    if (!converted.ok()) {
        return result;
    }
    mdx::Writer writer;
    result.value = writer.write(*converted);
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
