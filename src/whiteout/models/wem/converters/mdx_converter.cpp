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
#include "mdx_anim.h"

#include <algorithm>
#include <array>
#include <map>
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
        node.native.set("mdxAttachmentId", static_cast<i64>(attachment.attachmentId));
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

    /// `mdx::Model::cameras[i]` -> node index. Cameras carry no `objectId`, so
    /// they are the one kind the map above cannot reach.
    std::vector<u32> cameraNodes;

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
    out.tree.rig = RigConvention::PivotRelative;

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
        node.native.set("mdxFlagBits", static_cast<i64>(static_cast<u32>(mdxNode.flags)));
        FillPayload(source, item, node);

        // See the file comment: the rest pose is the pivot, so the local
        // translation is the pivot difference and the rotation and scale are
        // identity; the tracks are the channel table's (§10.8).
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
        out.cameraNodes.push_back(out.tree.size());
        out.tree.add(std::move(node));
    }

    return out;
}

// ============================================================================
// Geometry
// ============================================================================

/// Where a node kind's chunk sits in `mdx/writer.cpp`'s emission order — BONE,
/// LITE, HELP, ATCH, PRE2, RIBB, EVTS, CLID — which is the order MDX assigns
/// object ids in. `Camera` has no node chunk and so no id, and answers -1.
int ChunkRank(NodeKind kind) {
    switch (kind) {
    case NodeKind::Bone:
        return 0;
    case NodeKind::Light:
        return 1;
    case NodeKind::Helper:
        return 2;
    case NodeKind::Attachment:
        return 3;
    case NodeKind::ParticleEmitter:
        return 4;
    case NodeKind::RibbonEmitter:
        return 5;
    case NodeKind::Event:
        return 6;
    case NodeKind::CollisionShape:
        return 7;
    case NodeKind::Camera:
    case NodeKind::Count:
        break;
    }
    return -1;
}

constexpr int kLastChunkRank = 7;

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
        // A geoset Warcraft III hides carries a static alpha of zero -- the
        // only per-geoset visibility the format has -- and a static alpha is
        // not a track, so nothing else in this import would have seen it.
        for (const mdx::GeosetAnimation& animation : source.geosetAnimations) {
            if (animation.geosetId == g && !animation.alphaTracks.isUsed &&
                animation.alpha <= 0.0f) {
                section.flags |= SectionFlags::Hidden;
                break;
            }
        }
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
    //
    // The layer -> ordinal map falls out of this loop rather than being
    // recomputed: an animated layer's ordinal is its position in the *filtered*
    // stack, which only the material import knows (§10.8).
    mdx_anim::Context animContext;
    animContext.byObjectId = &nodes.byObjectId;
    animContext.cameraNodes = nodes.cameraNodes;

    for (ProfileId profile : document.profiles) {
        ProfileMaterialSet set;
        set.profile = profile;
        set.looks.looks.push_back(Look{});
        set.resizeBindings(model.materialSlots.size());

        mdx_anim::Context::ProfileLayers layers;
        layers.profile = profile;
        layers.byMaterial.resize(source.materials.size());

        for (std::size_t m = 0; m < source.materials.size(); ++m) {
            if (!HasProfile(slotProfiles[m], profile)) {
                continue;
            }
            const u32 index = static_cast<u32>(set.materials.size());
            set.materials.push_back(mdx_core::ImportMaterial(source.materials[m], profile, context,
                                                             diagnostics, &layers.byMaterial[m]));
            set.materials.back().name = SlotName(m);
            set.slotBindings[m].byLook[0] = index;
        }
        model.profileSets.push_back(std::move(set));
        animContext.layerOrdinals.push_back(std::move(layers));
    }

    const u32 modelIndex = static_cast<u32>(document.models.size());
    document.models.push_back(std::move(model));
    mdx_anim::Import(source, animContext, document, modelIndex, diagnostics);

    result.value = std::move(document);
    return result;
}

/**
 * @brief Writes one geoset's `GNDX` / `MTGC` / `MATS` and, above version 800,
 *        its `SKIN`.
 *
 * Two encodings of the same fact, and both go in every file because the writer
 * emits all three group chunks unconditionally.
 *
 * - **`GNDX`/`MTGC`/`MATS`** is what Warcraft III classic skins with, and it
 *   has no weights: `MTGC` gives the size of each group, `MATS` is the groups
 *   concatenated, and a vertex names a group whose bones are averaged
 *   **uniformly**. So a group IS the set of bones a vertex binds, and identical
 *   sets share one. `Ace.mdx` needs 100 of them for 6,148 vertices.
 * - **`SKIN`** carries four (bone, weight) pairs per vertex and indexes `MATS`
 *   directly, which is why a Reforged file writes the degenerate form of the
 *   groups instead: `LadyAlexstraszaReforged.mdx` has `MATS` = the identity
 *   over all 242 bones, `MTGC` = 242 ones, and `GNDX` pointing each vertex at
 *   one of them. This reproduces exactly that shape.
 *
 * `GNDX` is a byte, so no more than 256 groups can be named. Past that the
 * set encoding falls back to the dominant-bone one, which is a real loss of
 * blending for a classic file and none at all for a Reforged one -- where
 * `SKIN` is what the renderer reads.
 */
void WriteGeosetSkin(mdx::Geoset& geoset, const std::vector<u32>& sourceOf,
                     const std::vector<std::array<u32, 4>>& boneIndices,
                     const std::vector<std::array<f32, 4>>& boneWeights,
                     const std::vector<u32>& objectIdOf, bool skinChunk, u32 mesh,
                     Diagnostics& diagnostics) {
    if (boneIndices.empty() || boneWeights.empty()) {
        return;
    }

    // Per geoset vertex, the object ids it binds and their weights, already
    // resolved through the node -> object renumbering and with the unbound
    // influences (weight 0, or a node this export dropped) removed.
    struct Bound {
        std::vector<u32> ids;
        std::vector<f32> weights;
        u32 dominant = 0; ///< Index into `ids` of the heaviest influence.
    };
    std::vector<Bound> bound(sourceOf.size());
    std::vector<u32> palette;
    std::unordered_map<u32, u32> paletteOf;

    for (std::size_t v = 0; v < sourceOf.size(); ++v) {
        const u32 source = sourceOf[v];
        if (source >= boneIndices.size() || source >= boneWeights.size()) {
            continue;
        }
        Bound& entry = bound[v];
        f32 best = -1.0f;
        for (std::size_t k = 0; k < boneIndices[source].size(); ++k) {
            const f32 weight = k < boneWeights[source].size() ? boneWeights[source][k] : 0.0f;
            if (weight <= 0.0f) {
                continue;
            }
            const u32 node = boneIndices[source][k];
            const u32 objectId = node < objectIdOf.size() ? objectIdOf[node] : mdx::Node::NO_PARENT;
            if (objectId == mdx::Node::NO_PARENT) {
                continue;
            }
            if (weight > best) {
                best = weight;
                entry.dominant = static_cast<u32>(entry.ids.size());
            }
            entry.ids.push_back(objectId);
            entry.weights.push_back(weight);
            if (paletteOf.try_emplace(objectId, static_cast<u32>(palette.size())).second) {
                palette.push_back(objectId);
            }
        }
    }
    if (palette.empty()) {
        return;
    }

    // The set encoding, attempted first: a group is a sorted set of object ids,
    // deduplicated across the geoset.
    std::vector<u32> groupOf(sourceOf.size(), 0);
    std::vector<std::vector<u32>> groups;
    std::map<std::vector<u32>, u32> groupIndex;
    bool sets = true;
    for (std::size_t v = 0; v < bound.size() && sets; ++v) {
        std::vector<u32> key = bound[v].ids;
        std::sort(key.begin(), key.end());
        key.erase(std::unique(key.begin(), key.end()), key.end());
        if (key.empty()) {
            key.push_back(palette.front());
        }
        const auto [entry, inserted] = groupIndex.try_emplace(key, static_cast<u32>(groups.size()));
        if (inserted) {
            if (groups.size() >= 256) {
                sets = false;
                break;
            }
            groups.push_back(key);
        }
        groupOf[v] = entry->second;
    }

    if (!sets) {
        diagnostics.warn(DiagCode::BonePaletteLimit,
                         "section binds more than 256 distinct bone sets; the group encoding "
                         "keeps only the heaviest bone per vertex",
                         ElementRef(ElementKind::Mesh, mesh));
    }

    if (sets && !skinChunk) {
        geoset.matrixGroups.reserve(groups.size());
        for (const std::vector<u32>& group : groups) {
            geoset.matrixGroups.push_back(static_cast<u32>(group.size()));
            geoset.matrixIndices.insert(geoset.matrixIndices.end(), group.begin(), group.end());
        }
        geoset.vertexGroups.reserve(sourceOf.size());
        for (const u32 group : groupOf) {
            geoset.vertexGroups.push_back(static_cast<u8>(group));
        }
        return;
    }

    // `MATS` is the palette itself from here on, because `SKIN` addresses it --
    // which leaves `MTGC` no choice but one group per bone, and `GNDX` no
    // choice but the heaviest.
    if (palette.size() > 256) {
        diagnostics.warn(DiagCode::BonePaletteLimit,
                         "section binds " + std::to_string(palette.size()) +
                             " bones; a geoset palette holds 256",
                         ElementRef(ElementKind::Mesh, mesh));
    }
    geoset.matrixIndices = palette;
    geoset.matrixGroups.assign(palette.size(), 1u);
    geoset.vertexGroups.reserve(sourceOf.size());
    for (const Bound& entry : bound) {
        const u32 slot =
            entry.dominant < entry.ids.size() ? paletteOf[entry.ids[entry.dominant]] : 0u;
        geoset.vertexGroups.push_back(static_cast<u8>(std::min<u32>(slot, 0xFFu)));
    }

    if (!skinChunk) {
        return;
    }
    geoset.skinData.assign(sourceOf.size() * 8, 0);
    for (std::size_t v = 0; v < bound.size(); ++v) {
        const Bound& entry = bound[v];
        for (std::size_t k = 0; k < entry.ids.size() && k < 4; ++k) {
            const u32 slot = paletteOf[entry.ids[k]];
            if (slot > 0xFFu) {
                continue;
            }
            geoset.skinData[v * 8 + k] = static_cast<u8>(slot);
            geoset.skinData[v * 8 + 4 + k] =
                static_cast<u8>(std::clamp(entry.weights[k], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
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
    checkRigConvention(document, profile, result.diagnostics);
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
    // `TextureRef::replaceableId` is two vocabularies in one field — its own
    // header says so — and only one of them is MDX's. Warcraft III numbers team
    // colour 1, team glow 2 and the tilesets from 11; World of Warcraft numbers
    // *texture types*, where 11 is a monster's first skin. Copying a `.m2`'s 11
    // across told the adapter to ask `ReplaceableTextureManager` for a tileset,
    // and the replaceable branch is taken before the file name is ever read —
    // so a felstalker opened as Warcraft III drew white with a resolvable
    // `fileDataID` sitting unused beside the slot.
    //
    // `defaultProfile` is the authoring profile and a derive does not move it,
    // so a two-profile `.mdx` document keeps its ids and an imported one drops
    // them back to the texture's own key.
    const bool authoredAsMdx =
        Profile(document.defaultProfile).nativeMaterialKind == NativeKind::Mdx;
    for (const TextureRef& ref : document.textures) {
        mdx::Texture texture;
        texture.fileName = ref.path;
        texture.flags = static_cast<mdx::Texture::Flag>(ref.flags);
        texture.replaceableId = authoredAsMdx ? ref.replaceableId : 0u;
        context.textureIndexMap.push_back(static_cast<u32>(out.textures.size()));
        out.textures.push_back(std::move(texture));
    }

    // --- nodes --------------------------------------------------------------
    //
    // The node array is written back in WEM order and renumbered, because a
    // document that has been edited has no `objectId` left to trust. `PIVT` is
    // parallel to it, which is what makes the renumbering safe.
    // Where each node lands, for the animation export below: MDX keeps a
    // record's tracks ON the record, so writing them back needs the map only
    // this loop has.
    mdx_anim::ExportContext animContext;
    animContext.nodeSlots.assign(model.nodes.size(), mdx_anim::ExportContext::NodeSlot{});
    const auto claim = [&animContext](std::size_t node, mdx_anim::ExportContext::Slot slot,
                                      std::size_t index) {
        animContext.nodeSlots[node] = {slot, static_cast<u32>(index)};
    };

    // MDX numbers a node by the chunk it lands in, so every bone comes first,
    // then the lights, then the helpers, in `mdx/writer.cpp`'s emission order.
    // That is not cosmetic: `MATS` names an object id, and a reader that takes
    // one for an index into the bone array gets the right node only while the
    // bones are 0..n-1. Ours does exactly that (`resolveBoneIdx` asks
    // `BoneIndexToNodeIndex` first), and so does every other tool, because no
    // Blizzard file has ever been numbered any other way.
    //
    // Numbering in node order interleaved the kinds, which cost nothing until
    // `RetargetSkeleton` began inserting a shear helper immediately before the
    // bone it stretches — three of them, in the middle of the bone list. From
    // then on a StarCraft II model skinned nearly every vertex to a bone three
    // places off, which is a torn skeleton, not a wrong pose.
    std::vector<u32> objectIdOf(model.nodes.size(), mdx::Node::NO_PARENT);
    u32 nextObjectId = 0;
    for (int rank = 0; rank <= kLastChunkRank; ++rank) {
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            // A camera is not a node chunk and carries no id; it is written below.
            if (ChunkRank(model.nodes.nodes[i].kind) == rank) {
                objectIdOf[i] = nextObjectId++;
            }
        }
    }
    out.pivotPoints.assign(nextObjectId, Vector3f{0, 0, 0});

    const auto buildNode = [&](std::size_t index) {
        const Node& node = model.nodes.nodes[index];
        mdx::Node out_node;
        out_node.name = node.name;
        out_node.objectId = objectIdOf[index];
        out_node.parentId = node.parent == kInvalidNode || node.parent >= objectIdOf.size()
                                ? mdx::Node::NO_PARENT
                                : objectIdOf[node.parent];
        const NodeNative::Entry* raw = node.native.find("mdxFlagBits");
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
            claim(i, mdx_anim::ExportContext::Slot::Camera, out.cameras.size());
            out.cameras.push_back(std::move(camera));
            continue;
        }

        // The pivot IS the rest pose here (see the file comment), and a
        // document from a format that states its bind another way carries none
        // — `.m3` leaves every `pivot` zero. Composing the rest chain is the
        // best a bare export can do; `RetargetSkeleton` is what makes the
        // tracks agree with it.
        // `PIVT` is indexed by object id, so it is filled by id rather than
        // appended -- the loop below runs in node order, which is no longer it.
        out.pivotPoints[objectIdOf[i]] =
            model.nodes.rig == RigConvention::PivotRelative
                ? node.pivot
                : model.nodes.worldBind(static_cast<u32>(i)).translation;
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
            claim(i, mdx_anim::ExportContext::Slot::Bone, out.bones.size());
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
            claim(i, mdx_anim::ExportContext::Slot::Light, out.lights.size());
            out.lights.push_back(std::move(light));
            break;
        }
        case NodeKind::Attachment: {
            mdx::Attachment attachment;
            attachment.node = buildNode(i);
            if (const auto* id = node.native.find("mdxAttachmentId")) {
                attachment.attachmentId = static_cast<u32>(id->value);
            }
            claim(i, mdx_anim::ExportContext::Slot::Attachment, out.attachments.size());
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
            claim(i, mdx_anim::ExportContext::Slot::ParticleEmitter2, out.particleEmitters2.size());
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
            claim(i, mdx_anim::ExportContext::Slot::RibbonEmitter, out.ribbonEmitters.size());
            out.ribbonEmitters.push_back(std::move(emitter));
            break;
        }
        case NodeKind::Event: {
            mdx::EventObject event;
            event.node = buildNode(i);
            if (const auto* payload = std::get_if<EventPayload>(&node.payload)) {
                event.globalSequenceId = payload->id;
            }
            claim(i, mdx_anim::ExportContext::Slot::EventObject, out.eventObjects.size());
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
            claim(i, mdx_anim::ExportContext::Slot::CollisionShape, out.collisionShapes.size());
            out.collisionShapes.push_back(std::move(shape));
            break;
        }
        case NodeKind::Helper:
        default: {
            mdx::Helper helper;
            helper.node = buildNode(i);
            claim(i, mdx_anim::ExportContext::Slot::Helper, out.helpers.size());
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
    animContext.layerOfOrdinal.resize(model.materialSlots.size());
    for (std::size_t slot = 0; slot < model.materialSlots.size(); ++slot) {
        const Material* material = set ? Resolve(model, static_cast<u32>(slot), profile) : nullptr;
        if (material == nullptr) {
            out.materials.push_back(mdx::Material{});
            continue;
        }
        out.materials.push_back(mdx_core::ExportMaterial(*material, profile, context, diagnostics));
        // The ordinal is a position in the filtered stack and the export writes
        // exactly that stack, so the map is the identity — recorded rather than
        // assumed, because the two are only equal until something reorders a
        // layer between them.
        std::vector<u32>& ordinals = animContext.layerOfOrdinal[slot];
        ordinals.resize(out.materials.back().layers.size());
        for (std::size_t l = 0; l < ordinals.size(); ++l) {
            ordinals[l] = static_cast<u32>(l);
        }
    }

    // --- meshes -> geosets --------------------------------------------------
    //
    // **One geoset per SECTION, not per mesh.** A geoset carries a single
    // `materialId`, and a mesh carries as many sections as its source drew
    // batches: an `.m2` skin is one per batch, an `.m3` division one per
    // region, a Diablo III appearance thirty over one mesh. A geoset per mesh
    // merged every one of them into a single draw wearing section 0's material
    // -- for a Barbarian, all thirty armour variants at once in the look of a
    // hidden one.
    //
    // Each geoset takes a DISJOINT vertex slice in first-use order, so its
    // faces index its own array (the rule `toM3` already follows for a region),
    // and its own bone palette, because in MDX both are per geoset.
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

    // `SKIN` and `TANG` are written only above 800 (mdx/writer.cpp), so at 800
    // the group encoding is the only skinning the file carries.
    const bool skinChunk = targetVersion > 800;
    animContext.geosetsOfMesh.assign(model.meshes.size(), {});
    // The geosets a hidden section produced, turned into geoset animations once
    // every geoset exists.
    std::vector<u32> hiddenGeosets;

    constexpr u32 kUnmapped = ~0u;
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        const geom::RenderMesh render = geom::BuildRenderMesh(mesh, desc);
        diagnostics.append(render.diagnostics);

        const std::vector<Vector3f> positions = render.vertices.getPositions();
        const std::vector<Vector3f> normals = render.vertices.getNormals();
        const std::vector<Vector2f> uv0 = render.vertices.getUVs(0);
        const std::vector<std::array<u32, 4>> boneIndices = render.vertices.getBoneIndices();
        const std::vector<std::array<f32, 4>> boneWeights = render.vertices.getBoneWeights();

        // One slot per GPU vertex, cleared after each range rather than
        // reallocated: two sections can share a vertex when their corner
        // attributes agree, so the map is not a simple offset.
        std::vector<u32> localOf(render.vertexCount(), kUnmapped);

        for (const geom::RenderRange& range : render.ranges) {
            const MeshSection* section =
                range.section < mesh.sections.size() ? &mesh.sections[range.section] : nullptr;

            mdx::Geoset geoset;
            geoset.lod = mesh.lodLevel;
            geoset.lodName =
                section != nullptr && !section->name.empty() ? section->name : mesh.name;

            std::vector<u32> sourceOf;
            sourceOf.reserve(range.indexCount);
            geoset.faces.reserve(range.indexCount);
            bool wide = false;
            const u32 end = range.firstIndex + range.indexCount;
            for (u32 i = range.firstIndex; i < end && i < render.indices.size(); ++i) {
                const u32 source = render.indices[i];
                if (source >= localOf.size()) {
                    geoset.faces.push_back(0);
                    continue;
                }
                if (localOf[source] == kUnmapped) {
                    localOf[source] = static_cast<u32>(sourceOf.size());
                    sourceOf.push_back(source);
                }
                const u32 local = localOf[source];
                wide = wide || local > 0xFFFFu;
                geoset.faces.push_back(static_cast<u16>(local & 0xFFFFu));
            }
            for (const u32 source : sourceOf) {
                localOf[source] = kUnmapped;
            }
            if (wide) {
                diagnostics.warn(DiagCode::IndexWidthExceeded,
                                 "section needs more than 65535 vertices for one geoset",
                                 ElementRef(ElementKind::Mesh, m));
            }
            geoset.faceTypeGroups.push_back(4);
            geoset.faceGroups.push_back(static_cast<u32>(geoset.faces.size()));

            geoset.vertexPositions.reserve(sourceOf.size());
            geoset.vertexNormals.reserve(sourceOf.size());
            std::vector<Vector2f> uvs;
            if (!uv0.empty()) {
                uvs.reserve(sourceOf.size());
            }
            Extent bounds;
            ResetExtent(bounds);
            for (const u32 source : sourceOf) {
                const Vector3f position =
                    source < positions.size() ? positions[source] : Vector3f(0, 0, 0);
                geoset.vertexPositions.push_back(position);
                GrowExtent(bounds, position);
                geoset.vertexNormals.push_back(source < normals.size() ? normals[source]
                                                                       : Vector3f(0, 0, 1));
                if (!uv0.empty()) {
                    uvs.push_back(source < uv0.size() ? uv0[source] : Vector2f(0, 0));
                }
            }
            if (!uvs.empty()) {
                geoset.textureCoordinateSets.push_back(std::move(uvs));
            }
            // Derived, like every other bound in WEM. It is also the one thing
            // the merged geoset could not state: a section's own volume.
            if (!sourceOf.empty()) {
                FinishExtent(bounds);
                geoset.extent = FromExtent(bounds);
            } else {
                geoset.extent = FromExtent(mesh.bounds);
            }

            WriteGeosetSkin(geoset, sourceOf, boneIndices, boneWeights, objectIdOf, skinChunk,
                            static_cast<u32>(m), diagnostics);

            if (section != nullptr) {
                geoset.materialId = section->materialSlot;
                geoset.selectionGroup = section->selectionGroup;
                if (const auto* flags = section->native.find("selectionFlags")) {
                    geoset.selectionFlags = static_cast<u32>(flags->value);
                }
                if (hasFlag(section->flags, SectionFlags::Hidden)) {
                    hiddenGeosets.push_back(static_cast<u32>(out.geosets.size()));
                }
            }

            animContext.geosetsOfMesh[m].push_back(static_cast<u32>(out.geosets.size()));
            out.geosets.push_back(std::move(geoset));
        }
    }

    // A hidden section becomes a static alpha of zero, which is the only
    // per-geoset visibility MDX has and what Warcraft III itself uses to keep
    // an alternate body part out of the frame. Written before the animation
    // export so that a mesh which also keys alpha lands on the same record.
    for (const u32 geoset : hiddenGeosets) {
        mdx::GeosetAnimation animation;
        animation.geosetId = geoset;
        animation.alpha = 0.0f;
        animation.flags = mdx::GeosetAnimation::Flag::Color;
        out.geosetAnimations.push_back(std::move(animation));
    }

    // Last, because a geoset animation names a geoset and an event object has
    // to exist before its times can be written onto it.
    mdx_anim::Export(document, 0, profile, animContext, out, diagnostics);

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
