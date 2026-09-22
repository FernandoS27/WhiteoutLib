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
 *
 * Except where `worldBind` composes nothing of the parent's position: a node
 * flagged `DontInheritTranslation`, and one flagged `ModelSpace` (the particle
 * space bit, which WEM reads as "local is world"). Their local translation is
 * the pivot itself. Warcraft III places every node on its pivot at rest,
 * whatever its flags, and the difference would put these at `pivot -
 * parentPivot` instead.
 */

#include "whiteout/models/mdx/parser.h"
#include "whiteout/models/mdx/writer.h"
#include "whiteout/models/wem/converters.h"
#include "whiteout/models/wem/geometry/builder.h"
#include "whiteout/models/wem/geometry/render_view.h"
#include "whiteout/models/wem/skinning/quantize.h"

#include "../materials/mdx_core.h"
#include "../native/mdx_copy.h"
#include "mdx_anim.h"
#include "skin_skeleton.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <bit>
#include <map>
#include <string>
#include <unordered_map>

namespace whiteout {
namespace models {
namespace wem {

namespace {

constexpr ProfileId kMdxProfiles[] = {ProfileId::Wc3Classic, ProfileId::Wc3Reforged};

// A section's geoset-animation leftovers in its native bag, spelled here and
// nowhere else (`geosetTint` / `setGeosetTint`). Floats are their bit patterns.
constexpr const char* kGeosetColorR = "geosetColorR";
constexpr const char* kGeosetColorG = "geosetColorG";
constexpr const char* kGeosetColorB = "geosetColorB";
constexpr const char* kGeosetAlpha = "geosetAlpha";
/// The record's flags word without `DropShadow`, when it is not plain `Color`.
constexpr const char* kGeosetAnimFlags = "geosetAnimFlags";

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
    // Warcraft III's own four systems import whole (§10.9). A PopcornFX
    // emitter's effect is a file WEM does not hold, but its record is the
    // model's, and a kind of its own keeps it.
    case Origin::ParticleEmitter:
        return NodeKind::Wc3ParticleEmitter1;
    case Origin::ParticleEmitter2:
        return NodeKind::Wc3ParticleEmitter2;
    case Origin::CornEmitter:
        return NodeKind::Wc3CornEmitter;
    case Origin::RibbonEmitter:
        return NodeKind::Wc3RibbonEmitter;
    case Origin::Event:
        return NodeKind::Event;
    case Origin::Collision:
        return NodeKind::CollisionShape;
    case Origin::Helper:
        break;
    }
    return NodeKind::Helper;
}

/// The node flag bits an emitter kind gives a meaning of its own, which its
/// payload types (§10.9). The rest of the per-kind aliases stay in the raw word.
constexpr u32 kFlagUsesMdl = static_cast<u32>(mdx::Node::NodeFlag::EmitterUsesMdl);
constexpr u32 kFlagUsesTga = static_cast<u32>(mdx::Node::NodeFlag::EmitterUsesTga);
constexpr u32 kFlagUnshaded = static_cast<u32>(mdx::Node::NodeFlag::Unshaded);
constexpr u32 kFlagSortPrims = static_cast<u32>(mdx::Node::NodeFlag::SortPrimitives);
constexpr u32 kFlagLineEmitter = static_cast<u32>(mdx::Node::NodeFlag::LineEmitter);
constexpr u32 kFlagUnfogged = static_cast<u32>(mdx::Node::NodeFlag::Unfogged);
constexpr u32 kFlagXyQuad = static_cast<u32>(mdx::Node::NodeFlag::XYQuad);
// CORN's two bits past the shared pair, which PRE2 reads as `LineEmitter` and
// `Unfogged`.
constexpr u32 kFlagCornUnfogged = static_cast<u32>(mdx::Node::NodeFlag::PopcornUnfogged);
constexpr u32 kFlagCornScaling = static_cast<u32>(mdx::Node::NodeFlag::PopcornScaling);

u32 WithBit(u32 bits, u32 bit, bool on) {
    return on ? (bits | bit) : (bits & ~bit);
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
        // The gate is resolved as the renderer and the game resolve it: through
        // the record, with the bone's own `geosetId` read only against the
        // sentinel. A geoset is a mesh here, so its index is the mesh's.
        const mdx::Bone& bone = source.bones[pending.sourceIndex];
        auto& payload = std::get<BonePayload>(node.payload);
        if (bone.geosetId != mdx::Bone::MULTIPLE_GEOSETS &&
            bone.geosetAnimationId < source.geosetAnimations.size() &&
            source.geosetAnimations[bone.geosetAnimationId].geosetId < source.geosets.size()) {
            payload.gateMesh = source.geosetAnimations[bone.geosetAnimationId].geosetId;
        }
        // The file's `geosetId`, where the export would not write it back by
        // itself: a gated bone naming another geoset than its record does, and
        // an ungated one naming a geoset at all. Nothing reads either.
        const u32 written = payload.gateMesh != kInvalidIndex ? payload.gateMesh
                                                              : mdx::Bone::MULTIPLE_GEOSETS;
        if (bone.geosetId != written) {
            node.native.set("geosetId", static_cast<i64>(bone.geosetId));
        }
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
        // The parser fills a pre-1300/1600 light with the game's own
        // substitutes, so these are the values it plays at any version.
        payload.shadowCasting = light.shadowCasting;
        payload.shadowCastingStart = light.shadowCastingStart;
        payload.shadowCastingEnd = light.shadowCastingEnd;
        payload.quadraticFalloff = light.quadraticFalloff;
        payload.linearFalloff = light.linearFalloff;
        payload.damping = light.damping;
        // Rounded, not truncated: 0.7f is 0.69999999, and a truncation kept 0.699.
        SetMilli(node.native, "ambientIntensity", light.ambientIntensity);
        SetMilli(node.native, "shadowIntensity", light.shadowIntensity);
        // The ambient term's colour has no payload field. Kept bit for bit, red
        // first as the static colour is stored (the keys are blue first,
        // `mdx_anim.cpp`), and only when it is not the record's black.
        {
            const auto bits = [](f32 value) { return static_cast<i64>(std::bit_cast<u32>(value)); };
            const Vector3f& c = light.ambientColor;
            if (c.x != 0.0f || c.y != 0.0f || c.z != 0.0f) {
                node.native.set("ambientColorR", bits(c.x));
                node.native.set("ambientColorG", bits(c.y));
                node.native.set("ambientColorB", bits(c.z));
            }
        }
        break;
    }
    case Origin::Attachment: {
        const mdx::Attachment& attachment = source.attachments[pending.sourceIndex];
        node.native.set("mdxAttachmentId", static_cast<i64>(attachment.attachmentId));
        // The model the point spawns (`NEBirth`, `UBirth`), by name: nothing in
        // this file resolves it, so it stays a key with no model behind it.
        std::get<AttachmentPayload>(node.payload).asset.path = attachment.path;
        break;
    }
    case Origin::ParticleEmitter: {
        const mdx::ParticleEmitter& emitter = source.particleEmitters[pending.sourceIndex];
        auto& payload = std::get<Wc3ParticleEmitter1Payload>(node.payload);
        payload.emissionRate = emitter.emissionRate;
        payload.gravity = emitter.gravity;
        payload.longitude = emitter.longitude;
        payload.latitude = emitter.latitude;
        payload.lifespan = emitter.lifespan;
        payload.speed = emitter.initialVelocity;
        payload.spawnModel.path = emitter.spawnModelFileName;
        const u32 bits = static_cast<u32>(emitter.node.flags);
        payload.usesMdl = (bits & kFlagUsesMdl) != 0;
        payload.usesTga = (bits & kFlagUsesTga) != 0;
        break;
    }
    case Origin::ParticleEmitter2: {
        const mdx::ParticleEmitter2& emitter = source.particleEmitters2[pending.sourceIndex];
        auto& payload = std::get<Wc3ParticleEmitter2Payload>(node.payload);
        payload.speed = emitter.speed;
        payload.variation = emitter.variation;
        payload.latitude = emitter.latitude;
        payload.gravity = emitter.gravity;
        payload.lifespan = emitter.lifespan;
        payload.emissionRate = emitter.emissionRate;
        payload.width = emitter.width;
        payload.length = emitter.length;
        payload.filter = static_cast<Wc3ParticleFilter>(emitter.filterMode);
        payload.rows = emitter.rows;
        payload.columns = emitter.columns;
        payload.headOrTail = static_cast<Wc3ParticleHeadOrTail>(emitter.headOrTail);
        payload.tailLength = emitter.tailLength;
        payload.time = emitter.time;
        Wc3ParticleSegment* segments[3] = {&payload.start, &payload.middle, &payload.end};
        for (int s = 0; s < 3; ++s) {
            segments[s]->color = emitter.segmentColor[s];
            segments[s]->alpha = emitter.segmentAlpha[s];
            segments[s]->scaling = emitter.segmentScaling[s];
        }
        const auto interval = [](const std::array<u32, 3>& source) {
            return Wc3ParticleInterval{source[0], source[1], source[2]};
        };
        payload.headLife = interval(emitter.headInterval);
        payload.headDecay = interval(emitter.headDecayInterval);
        payload.tailLife = interval(emitter.tailInterval);
        payload.tailDecay = interval(emitter.tailDecayInterval);
        // `TEXS` imports one document texture per entry, in order.
        payload.texture = emitter.textureId < source.textures.size() ? emitter.textureId
                                                                      : kInvalidIndex;
        payload.replaceableId = emitter.replaceableId;
        payload.squirt = emitter.squirt != 0;
        payload.priorityPlane = emitter.priorityPlane;
        const u32 bits = static_cast<u32>(emitter.node.flags);
        payload.unshaded = (bits & kFlagUnshaded) != 0;
        payload.sortPrimsFarZ = (bits & kFlagSortPrims) != 0;
        payload.lineEmitter = (bits & kFlagLineEmitter) != 0;
        payload.unfogged = (bits & kFlagUnfogged) != 0;
        payload.xyQuad = (bits & kFlagXyQuad) != 0;
        break;
    }
    case Origin::CornEmitter: {
        const mdx::CornEmitter& emitter = source.cornEmitters[pending.sourceIndex];
        auto& payload = std::get<Wc3CornEmitterPayload>(node.payload);
        payload.lifespan = emitter.lifeSpan;
        payload.emissionRate = emitter.emissionRate;
        payload.speed = emitter.speed;
        payload.color = emitter.color;
        payload.alpha = emitter.alpha;
        payload.replaceableId = emitter.replaceableId;
        payload.effect.path = emitter.path;
        payload.animVisibilityGuide = emitter.animVisibilityGuide;
        const u32 bits = static_cast<u32>(emitter.node.flags);
        payload.unshaded = (bits & kFlagUnshaded) != 0;
        payload.sortPrimsFarZ = (bits & kFlagSortPrims) != 0;
        payload.unfogged = (bits & kFlagCornUnfogged) != 0;
        payload.popcornScaling = (bits & kFlagCornScaling) != 0;
        break;
    }
    case Origin::RibbonEmitter: {
        const mdx::RibbonEmitter& emitter = source.ribbonEmitters[pending.sourceIndex];
        auto& payload = std::get<Wc3RibbonEmitterPayload>(node.payload);
        payload.heightAbove = emitter.heightAbove;
        payload.heightBelow = emitter.heightBelow;
        payload.alpha = emitter.alpha;
        payload.color = emitter.color;
        payload.lifespan = emitter.lifespan;
        payload.textureSlot = emitter.textureSlot;
        payload.emissionRate = emitter.emissionRate;
        payload.rows = emitter.rows;
        payload.columns = emitter.columns;
        // One slot per `MTLS` entry, in order (`SlotName`).
        payload.materialSlot = emitter.materialId < source.materials.size() ? emitter.materialId
                                                                            : kInvalidIndex;
        payload.gravity = emitter.gravity;
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
        //
        // The parent's pivot comes from `pending`, which is complete, not from
        // the half-built tree: bones convert before helpers, so a bone whose
        // parent is a helper is a FORWARD reference — reading the growing tree
        // gave those nodes an absolute local (their parent pivot as zero),
        // which every consumer of `local` (glTF export, retargets) inherited
        // while the pivot-composing mdx evaluator hid it.
        //
        // A node `worldBind` does not compose onto its parent's position keeps
        // its pivot whole (see the file comment). At an MDX rest every rotation
        // and scale is the identity, so that is all the flag changes there.
        const bool absolute = hasFlag(node.flags, NodeFlags::DontInheritTranslation) ||
                              hasFlag(node.flags, NodeFlags::ModelSpace);
        Vector3f parentPivot{0, 0, 0};
        if (!absolute && node.parent != kInvalidNode && node.parent < pending.size()) {
            const mdx::Node& parentNode = *pending[node.parent].source;
            if (parentNode.objectId < source.pivotPoints.size()) {
                parentPivot = source.pivotPoints[parentNode.objectId];
            }
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
        payload.target = camera.targetPosition;
        // The position is the camera's pivot as much as its rest: KCTR keys
        // offset it, and a retarget rebuilds the rest from the pivot.
        node.pivot = camera.position;
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

/// Where a node kind's chunk sits in the order MDX assigns object ids in —
/// BONE, LITE, HELP, ATCH, PREM, PRE2, CORN, RIBB, EVTS, CLID. CORN is where
/// Reforged's own files number it, between PRE2 and RIBB, which is also where
/// 3.0's chunk table reads it. `Camera` has no node chunk and so no id, and
/// answers -1. @p kind is what the node is WRITTEN as (`WrittenKind`), so a
/// system the profile does not carry ranks as the helper it becomes.
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
    case NodeKind::Wc3ParticleEmitter1:
        return 4;
    case NodeKind::Wc3ParticleEmitter2:
    case NodeKind::ParticleEmitter: // written as a PRE2
        return 5;
    case NodeKind::Wc3CornEmitter:
        return 6;
    case NodeKind::Wc3RibbonEmitter:
    case NodeKind::RibbonEmitter:
        return 7;
    case NodeKind::Event:
        return 8;
    case NodeKind::CollisionShape:
        return 9;
    case NodeKind::Camera:
    case NodeKind::Sc2ParticleEmitter:
    case NodeKind::Sc2RibbonEmitter:
    case NodeKind::M2ParticleEmitter: // crossed to a PRE2 before export, or a helper
    case NodeKind::Count:
        break;
    }
    return -1;
}

constexpr int kLastChunkRank = 9;

/// What @p kind is written as under @p profile: itself when the profile carries
/// it, and otherwise its placement alone -- a helper (§10.9). A StarCraft II
/// particle system in a Warcraft III export keeps the node its children hang
/// off and loses the system Warcraft III cannot run.
NodeKind WrittenKind(NodeKind kind, ProfileId profile) {
    return CarriesNodeKind(profile, kind) ? kind : NodeKind::Helper;
}

/// Feeds one geoset's skinning into @p builder.
///
/// Both MDX conventions land here. Reforged's `skinData` is four
/// (index, weight/255) pairs per vertex, where the index goes through
/// `matrixIndices` when that array is present. Classic's is a per-vertex group
/// id naming a run of `matrixIndices`, every member weighted equally — a
/// matrix *group*, not a weight list. WEM stores the run at 1/N rather than the
/// renderer's averaged pseudo-bone, because `SkinBinding` is variable width and
/// `maxBoneInfluences` is a limit export checks, not one import enforces.
///
/// A bone named twice in one vertex is one influence of the summed weight: a
/// group `{A, A, B}` is `{A: ⅔, B: ⅓}`, which is what the game blends
/// (`BuildPrimBone` adds one matrix per entry). The weights are unchanged; what
/// changes is that every operation may assume one entry per bone.
void ImportSkin(const mdx::Geoset& geoset, const NodeImport& nodes, geom::MeshBuilder& builder,
                std::size_t vertexCount, Diagnostics& out, std::size_t geosetIndex) {
    const auto resolveMatrix = [&](u32 raw) -> u32 {
        const u32 objectId = raw < geoset.matrixIndices.size() ? geoset.matrixIndices[raw] : raw;
        const u32 node = nodes.resolve(objectId);
        if (node == kInvalidNode) {
            out.warn(DiagCode::DanglingNodeReference,
                     "geoset matrix names object id " + std::to_string(objectId) +
                         ", which is not a node",
                     ElementRef(ElementKind::Mesh, static_cast<u32>(geosetIndex)));
        }
        return node;
    };

    // One vertex's influences, a repeated bone summed, handed over in order.
    std::vector<geom::Influence> merged;
    const auto add = [&merged](u32 node, f32 weight) {
        for (geom::Influence& influence : merged) {
            if (influence.bone == node) {
                influence.weight += weight;
                return;
            }
        }
        merged.push_back({node, weight});
    };
    const auto flush = [&](std::size_t v) {
        for (const geom::Influence& influence : merged) {
            builder.addInfluence(geom::VertexId(static_cast<u32>(v)), influence.bone,
                                 influence.weight);
        }
        merged.clear();
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
                    add(node, weight);
                }
            }
            flush(v);
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
                add(node, weight);
            }
        }
        flush(v);
    }
}

/// The address mode each document texture is sampled with, as MDX's own two
/// bits: 0x1 wrap U, 0x2 wrap V.
///
/// MDX states wrapping on the TEXTURE and every other source states it on the
/// layer, so `TextureRef::flags` is empty on everything that did not come from
/// an `.mdx` -- and an empty flag word is not "no opinion", it is clamp. A
/// clamped layer whose coordinates leave [0,1] samples one edge column across
/// the whole surface: half of a Murky is a grey smear down the u=1 seam, which
/// is the same defect the `.m3` adapter's `MaddWrapLayers` was written for and
/// with the same cause, reached from the export side.
///
/// The `.m3` layer's own wrap bit is not the answer either. It is carried in a
/// per-layer UV transform most layers do not have, so 82% of restored MADD
/// layers come back with no bit at all, while shipped fixed-function content
/// wraps on 14464 of 14534 StarCraft II layers and 27664 of 28267 Heroes ones.
/// `TextureInput` defaults to `Repeat` for exactly that reason, and this reads
/// the default rather than the source's silence.
///
/// A texture two layers disagree about is wrapped: clamping something that
/// needs to tile loses the whole surface, and tiling something that wanted
/// clamp shows at one seam.
std::vector<u32> TextureWrapBits(const Document& document, const ProfileMaterialSet* set) {
    std::vector<u32> bits(document.textures.size(), 0u);
    if (set == nullptr) {
        return bits;
    }
    const auto note = [&bits](const TextureInput& input) {
        if (!input.hasTexture() || input.texture >= bits.size()) {
            return;
        }
        bits[input.texture] |= (input.wrapU == WrapMode::Repeat ? 0x1u : 0u) |
                               (input.wrapV == WrapMode::Repeat ? 0x2u : 0u);
    };
    for (const Material& material : set->materials) {
        const CommonMaterial& common = material.Common();
        if (const CompositeBody* composite = common.composite()) {
            for (const CompositeLayer& layer : composite->layers) {
                note(layer.input);
            }
        } else if (const CombinersBody* combiners = common.combiners()) {
            for (const CombinerStage& stage : combiners->stages) {
                note(stage.input);
            }
        } else if (const PbrDeferredBody* pbr = common.pbr()) {
            for (const auto& [slot, input] : pbr->slots) {
                note(input);
            }
        } else if (const LegacyDeferredBody* legacy = common.legacy()) {
            for (const auto& [slot, input] : legacy->slots) {
                note(input);
            }
        }
    }
    return bits;
}

/// The geoset animations' flags words and the bones' links to them, written
/// once every record exists (EDIT_MODE_MESH_DESIGN.md §7.6). A bone's link is
/// derived from its `gateMesh` rather than carried: the table the file's raw
/// indices named is rebuilt by this export, so they would name other records.
void LinkGeosetAnimations(const Model& model, const mdx_anim::ExportContext& context,
                          mdx::Model& out, Diagnostics& diagnostics) {
    using Flag = mdx::GeosetAnimation::Flag;
    const auto recordOf = [&out](u32 geoset, bool create) -> u32 {
        for (std::size_t r = 0; r < out.geosetAnimations.size(); ++r) {
            if (out.geosetAnimations[r].geosetId == geoset) {
                return static_cast<u32>(r);
            }
        }
        if (!create) {
            return kInvalidIndex;
        }
        mdx::GeosetAnimation created;
        created.geosetId = geoset;
        created.flags = Flag::Color;
        out.geosetAnimations.push_back(std::move(created));
        return static_cast<u32>(out.geosetAnimations.size() - 1);
    };
    const auto gateOf = [&model](std::size_t node) {
        const auto* bone = std::get_if<BonePayload>(&model.nodes.nodes[node].payload);
        return bone != nullptr ? bone->gateMesh : kInvalidIndex;
    };

    // A mesh a bone gates gets a record whatever else it carries: the game
    // reads that record's flags through the bone.
    std::vector<u8> gated(model.meshes.size(), 0);
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        if (gateOf(i) < gated.size()) {
            gated[gateOf(i)] = 1;
        }
    }

    const u32 dropShadow = static_cast<u32>(Flag::DropShadow);
    for (std::size_t m = 0; m < context.geosetsOfMesh.size() && m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        for (std::size_t k = 0; k < context.geosetsOfMesh[m].size(); ++k) {
            const u32 s = k < context.sectionOfGeoset[m].size() ? context.sectionOfGeoset[m][k]
                                                                : kInvalidIndex;
            const MeshSection* section = s < mesh.sections.size() ? &mesh.sections[s] : nullptr;
            const NativeBag::Entry* word =
                section != nullptr ? section->native.find(kGeosetAnimFlags) : nullptr;
            const bool shadow =
                section != nullptr && hasFlag(section->flags, SectionFlags::ProjectedShadow);
            const bool needed = word != nullptr || shadow || (gated[m] != 0 && k == 0);
            const u32 r = recordOf(context.geosetsOfMesh[m][k], needed);
            if (r == kInvalidIndex) {
                continue;
            }
            u32 flags = word != nullptr ? static_cast<u32>(word->value) & ~dropShadow
                                        : static_cast<u32>(Flag::Color);
            if (shadow) {
                flags |= dropShadow;
            }
            out.geosetAnimations[r].flags = static_cast<Flag>(flags);
        }
    }

    for (std::size_t i = 0; i < model.nodes.size() && i < context.nodeSlots.size(); ++i) {
        const mdx_anim::ExportContext::NodeSlot& slot = context.nodeSlots[i];
        if (slot.slot != mdx_anim::ExportContext::Slot::Bone || slot.index >= out.bones.size()) {
            continue;
        }
        mdx::Bone& bone = out.bones[slot.index];
        const u32 mesh = gateOf(i);
        const NativeBag::Entry* kept = model.nodes.nodes[i].native.find("geosetId");
        if (mesh < context.geosetsOfMesh.size() && !context.geosetsOfMesh[mesh].empty()) {
            const u32 first = context.geosetsOfMesh[mesh].front();
            bone.geosetId = kept != nullptr ? static_cast<u32>(kept->value) : first;
            bone.geosetAnimationId = recordOf(first, true);
            continue;
        }
        if (mesh != kInvalidIndex) {
            diagnostics.warn(DiagCode::IndexOutOfRange,
                             "bone is gated by mesh " + std::to_string(mesh) +
                                 ", which does not exist; written with no gate",
                             ElementRef(ElementKind::Node, static_cast<u32>(i)));
        }
        bone.geosetId = kept != nullptr ? static_cast<u32>(kept->value) : mdx::Bone::MULTIPLE_GEOSETS;
        bone.geosetAnimationId = mdx::Bone::MULTIPLE_GEOSETS;
    }
}

} // namespace

// ============================================================================
// MdxExportMapOf
// ============================================================================

MdxExportMap MdxExportMapOf(const Document& document, u32 model, ProfileId profile) {
    MdxExportMap map;
    if (model >= document.models.size()) {
        return map;
    }

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
    const auto& nodes = document.models[model].nodes;
    map.nodeObjectId.assign(nodes.size(), kInvalidIndex);
    u32 next = 0;
    for (int rank = 0; rank <= kLastChunkRank; ++rank) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            // A camera is not a node chunk and carries no id.
            if (ChunkRank(WrittenKind(nodes.nodes[i].kind, profile)) == rank) {
                map.nodeObjectId[i] = next++;
            }
        }
    }
    map.clipSequence = mdx_anim::ClipSequences(document, model);

    // One geoset per section, in mesh order, and one for a mesh with none: the
    // render view splits by section and always yields that many ranges.
    // `toMdx` numbers from this, so the two cannot disagree.
    const auto& meshes = document.models[model].meshes;
    map.geosetsOfMesh.resize(meshes.size());
    u32 geoset = 0;
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        const std::size_t count = std::max<std::size_t>(1, meshes[m].sections.size());
        for (std::size_t k = 0; k < count; ++k) {
            map.geosetsOfMesh[m].push_back(geoset++);
        }
    }
    return map;
}

// ============================================================================
// A geoset's static colour and alpha
// ============================================================================

GeosetTint MdxConverter::geosetTint(const MeshSection& section) const {
    const auto value = [&](const char* key) -> const NativeBag::Entry* {
        return section.native.find(key);
    };
    const auto real = [](const NativeBag::Entry* entry) {
        return std::bit_cast<f32>(static_cast<u32>(entry->value));
    };
    GeosetTint tint;
    const NativeBag::Entry* r = value(kGeosetColorR);
    const NativeBag::Entry* g = value(kGeosetColorG);
    const NativeBag::Entry* b = value(kGeosetColorB);
    if (r != nullptr && g != nullptr && b != nullptr) {
        tint.color = Vector3f{real(r), real(g), real(b)};
    }
    if (const NativeBag::Entry* a = value(kGeosetAlpha)) {
        tint.alpha = real(a);
    }
    tint.hidden = hasFlag(section.flags, SectionFlags::Hidden);
    return tint;
}

void MdxConverter::setGeosetTint(MeshSection& section, const GeosetTint& tint) const {
    const bool hidden = tint.hidden || tint.alpha <= 0.0f;
    const f32 alpha = tint.alpha > 0.0f ? tint.alpha : geosetTint(section).alpha;
    const auto bits = [](f32 v) { return static_cast<i64>(std::bit_cast<u32>(v)); };

    // Written back where the import puts them -- after `selectionFlags` -- so
    // that a bag edited back to its imported value compares equal to it: the
    // bag keeps insertion order and two orders are two bags.
    std::vector<NativeBag::Entry>& entries = section.native.entries;
    std::size_t at = entries.size();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string& name = entries[i].name;
        if (name == kGeosetColorR || name == kGeosetColorG || name == kGeosetColorB ||
            name == kGeosetAlpha) {
            at = std::min(at, i);
        }
    }
    std::erase_if(entries, [](const NativeBag::Entry& entry) {
        return entry.name == kGeosetColorR || entry.name == kGeosetColorG ||
               entry.name == kGeosetColorB || entry.name == kGeosetAlpha;
    });
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == "selectionFlags") {
            at = i + 1;
        }
    }
    at = std::min(at, entries.size());

    std::vector<NativeBag::Entry> added;
    const Vector3f& c = tint.color;
    if (c.x != 1.0f || c.y != 1.0f || c.z != 1.0f) {
        added.push_back({kGeosetColorR, bits(c.x), {}});
        added.push_back({kGeosetColorG, bits(c.y), {}});
        added.push_back({kGeosetColorB, bits(c.z), {}});
    }
    if (alpha != 1.0f) {
        added.push_back({kGeosetAlpha, bits(alpha), {}});
    }
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(at), added.begin(), added.end());

    const u32 without = static_cast<u32>(section.flags) & ~static_cast<u32>(SectionFlags::Hidden);
    section.flags = static_cast<SectionFlags>(without);
    if (hidden) {
        section.flags |= SectionFlags::Hidden;
    }
}

GeosetFlags MdxConverter::geosetFlags(const MeshSection& section) const {
    GeosetFlags flags;
    if (const NativeBag::Entry* selection = section.native.find("selectionFlags")) {
        flags.selection = static_cast<u32>(selection->value);
    }
    if (const NativeBag::Entry* word = section.native.find(kGeosetAnimFlags)) {
        flags.animation = static_cast<u32>(word->value) & ~GeosetFlags::kDropShadow;
    }
    if (hasFlag(section.flags, SectionFlags::ProjectedShadow)) {
        flags.animation |= GeosetFlags::kDropShadow;
    }
    return flags;
}

void MdxConverter::setGeosetFlags(MeshSection& section, const GeosetFlags& flags) const {
    section.native.set("selectionFlags", static_cast<i64>(flags.selection));
    const u32 rest = flags.animation & ~GeosetFlags::kDropShadow;
    std::erase_if(section.native.entries,
                  [](const NativeBag::Entry& entry) { return entry.name == kGeosetAnimFlags; });
    if (rest != static_cast<u32>(mdx::GeosetAnimation::Flag::Color)) {
        section.native.set(kGeosetAnimFlags, static_cast<i64>(rest));
    }
    const u32 without =
        static_cast<u32>(section.flags) & ~static_cast<u32>(SectionFlags::ProjectedShadow);
    section.flags = static_cast<SectionFlags>(without);
    if ((flags.animation & GeosetFlags::kDropShadow) != 0) {
        section.flags |= SectionFlags::ProjectedShadow;
    }
}

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
    context.textureRefs = &document.textures;

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
                             ElementRef(ElementKind::Mesh, static_cast<u32>(g)));
        }
        section.native.set("selectionFlags", static_cast<i64>(geoset.selectionFlags));
        // A geoset Warcraft III hides carries a static alpha of zero -- the
        // only per-geoset visibility the format has -- and a static alpha is
        // not a track, so nothing else in this import would have seen it. The
        // rest of the geoset's first record is no track either: a static tint
        // and a static partial alpha, which Warcraft III multiplies into the
        // geoset and a keyed one answers outside its keys. Kept bit for bit;
        // the colour is red first, as the renderer and Blizzard's conversions
        // read it (`mdx_anim.cpp`, `SwapRedBlue`).
        const mdx::GeosetAnimation* record = nullptr;
        GeosetTint tint;
        for (const mdx::GeosetAnimation& animation : source.geosetAnimations) {
            if (animation.geosetId != g) {
                continue;
            }
            if (record == nullptr) {
                record = &animation;
                tint.color = animation.color;
                if (animation.alpha > 0.0f) {
                    tint.alpha = animation.alpha;
                }
            }
            if (!animation.alphaTracks.isUsed && animation.alpha <= 0.0f) {
                tint.hidden = true;
            }
        }
        setGeosetTint(section, tint);
        // The record's flags word: `DropShadow` is the section's flag, and the
        // rest rides the bag when it is anything but the `Color` every record
        // the export makes carries -- 251 shipped records set bits 0x18.
        if (record != nullptr) {
            const u32 word = static_cast<u32>(record->flags);
            const u32 dropShadow = static_cast<u32>(mdx::GeosetAnimation::Flag::DropShadow);
            if ((word & dropShadow) != 0) {
                section.flags |= SectionFlags::ProjectedShadow;
            }
            if ((word & ~dropShadow) != static_cast<u32>(mdx::GeosetAnimation::Flag::Color)) {
                section.native.set(kGeosetAnimFlags, static_cast<i64>(word & ~dropShadow));
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
                                 ElementRef(ElementKind::Mesh, static_cast<u32>(g)));
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

namespace {

// ============================================================================
// What a geoset binds (EDIT_MODE_SKIN_DESIGN.md §12)
// ============================================================================

/// The render view every geoset is cut from: split by section, with the four
/// streams a geoset holds.
///
/// It carries no skin. What a vertex binds is `GeosetSkin`'s, read from
/// `Mesh::skin` through `vertexToWemVertex`, so no byte lane and no four-wide
/// getter stands between the document and the file: a classic group of eight
/// bones reaches the quantizer whole, and node 300 stays node 300.
geom::RenderMeshDesc GeosetRenderDesc(bool secondUvSet = false) {
    geom::RenderMeshDesc desc;
    desc.attributes = {
        {geom::names::kPosition, utils::AttributeClass::Position, utils::AttributeEncoding::Float32,
         3, 0},
        {geom::names::kNormal, utils::AttributeClass::Normal, utils::AttributeEncoding::Float32, 3,
         0},
        {geom::names::uv(0), utils::AttributeClass::UV, utils::AttributeEncoding::Float32, 2, 0},
        // `TANG` exists from v900 and an HD material's normal map is meaningless
        // without it: the shader reads the tangent frame off this chunk, and a
        // Reforged model that carried a normal map and no tangents shaded as
        // though every face were flat. Import has always read them (§5.4's
        // `kTangent` corner layer); export dropped them on the floor.
        {geom::names::kTangent, utils::AttributeClass::Tangent, utils::AttributeEncoding::Float32,
         4, 0},
    };
    if (secondUvSet) {
        // After uv0, so `getUVs(1)` is this layer: the getter counts the UV
        // attributes the view holds, not the names.
        desc.attributes.insert(desc.attributes.begin() + 3,
                               decltype(desc.attributes)::value_type{
                                   geom::names::uv(1), utils::AttributeClass::UV,
                                   utils::AttributeEncoding::Float32, 2, 0});
    }
    desc.splitBySection = true;
    return desc;
}

/// Whether @p mesh's geosets carry a second UV set (a layer's `coordId 1`).
/// Reforged reads two; a set that never varies can neither seam a vertex nor
/// tell a layer anything, and every M2 body mesh ships an all-zero `uv1`.
bool WritesSecondUvSet(const Mesh& mesh, ProfileId profile) {
    if (Profile(profile).maxUvSets < 2 ||
        !mesh.attributes.has(geom::names::uv(0), geom::Domain::Halfedge)) {
        return false;
    }
    const auto uv1 = mesh.attributes.get<Vector2f>(geom::names::uv(1), geom::Domain::Halfedge);
    return std::any_of(uv1.begin(), uv1.end(), [&](const Vector2f& uv) {
        return uv.x != uv1.front().x || uv.y != uv1.front().y;
    });
}

/// One mesh's render view, unpacked once for every range in it.
struct GeosetStreams {
    std::vector<Vector3f> positions;
    std::vector<Vector3f> normals;
    std::vector<Vector2f> uv0;
    std::vector<Vector2f> uv1; ///< empty unless the view asked for it
    std::vector<Vector4f> tangents;

    GeosetStreams(const geom::RenderMesh& render, const Mesh& mesh, u32 targetVersion,
                  bool secondUvSet = false)
        : positions(render.vertices.getPositions()), normals(render.vertices.getNormals()),
          uv0(render.vertices.getUVs(0)) {
        if (secondUvSet) {
            uv1 = render.vertices.getUVs(1);
        }
        // Only when the source actually authored them. A mesh with no tangent
        // layer would otherwise get a chunk of zeroes, which is worse than the
        // absence the reader already handles.
        if (targetVersion > 800 &&
            mesh.attributes.has(geom::names::kTangent, geom::Domain::Halfedge)) {
            tangents = render.vertices.getTangents();
        }
    }
};

constexpr u32 kUnmappedVertex = ~0u;

/// One render range's vertex slice: each geoset vertex's GPU vertex, in
/// first-use order, and the faces over them.
struct GeosetSlice {
    std::vector<u32> sourceOf;
    /// Local indices; `kUnmappedVertex` for a corner past the view.
    std::vector<u32> faces;
};

/// @p localOf is one slot per GPU vertex, all `kUnmappedVertex`, and left that
/// way: two sections can share a vertex when their corner attributes agree, so
/// the map is not a simple offset.
GeosetSlice SliceRange(const geom::RenderMesh& render, const geom::RenderRange& range,
                       std::vector<u32>& localOf) {
    GeosetSlice slice;
    slice.sourceOf.reserve(range.indexCount);
    slice.faces.reserve(range.indexCount);
    const u32 end = range.firstIndex + range.indexCount;
    for (u32 i = range.firstIndex; i < end && i < render.indices.size(); ++i) {
        const u32 source = render.indices[i];
        if (source >= localOf.size()) {
            slice.faces.push_back(kUnmappedVertex);
            continue;
        }
        if (localOf[source] == kUnmappedVertex) {
            localOf[source] = static_cast<u32>(slice.sourceOf.size());
            slice.sourceOf.push_back(source);
        }
        slice.faces.push_back(localOf[source]);
    }
    for (const u32 source : slice.sourceOf) {
        localOf[source] = kUnmappedVertex;
    }
    return slice;
}

/// The WEM vertex of each vertex of @p slice.
std::vector<u32> WemVerticesOf(const geom::RenderMesh& render, const GeosetSlice& slice) {
    std::vector<u32> vertices;
    vertices.reserve(slice.sourceOf.size());
    for (const u32 source : slice.sourceOf) {
        vertices.push_back(source < render.vertexToWemVertex.size()
                               ? render.vertexToWemVertex[source]
                               : kUnmappedVertex);
    }
    return vertices;
}

/// What one mesh's geosets need to say what they bind.
struct SkinContext {
    const Mesh& mesh;
    const std::vector<u32>& objectIdOf;
    const SkinSkeleton& skeleton;
    std::span<const Vector3f> positions; ///< The mesh's, per WEM vertex.
    bool classic = false;                ///< Groups (the Skin Quantizer), not `SKIN`.
    std::span<const u16> pins;           ///< `classicBones`, per WEM vertex; empty for none.
};

/// A vertex's document influences on the nodes the file writes: a zero,
/// negative or non-finite weight and a node with no object id dropped, a
/// duplicate bone merged, heaviest first with ties by node.
std::vector<geom::Influence> WritableInfluences(std::span<const geom::Influence> influences,
                                                const std::vector<u32>& objectIdOf) {
    std::vector<geom::Influence> out;
    for (const geom::Influence& influence : influences) {
        if (!(influence.weight > 0.0f) || !std::isfinite(influence.weight) ||
            influence.bone >= objectIdOf.size() ||
            objectIdOf[influence.bone] == mdx::Node::NO_PARENT) {
            continue;
        }
        const auto same = std::find_if(out.begin(), out.end(), [&](const geom::Influence& kept) {
            return kept.bone == influence.bone;
        });
        if (same != out.end()) {
            same->weight += influence.weight;
        } else {
            out.push_back(influence);
        }
    }
    std::sort(out.begin(), out.end(), [](const geom::Influence& a, const geom::Influence& b) {
        if (a.weight != b.weight) {
            return a.weight > b.weight;
        }
        return a.bone < b.bone;
    });
    return out;
}

/// The skin one geoset holds: @p vertices are its vertices' WEM vertices, in
/// the geoset's order, and @p overLimit counts those past the encoding's width.
WrittenGeosetSkin GeosetSkin(const SkinContext& context, const MeshSection* section,
                             std::span<const u32> vertices, u32& overLimit) {
    WrittenGeosetSkin out;
    out.vertices.assign(vertices.begin(), vertices.end());
    out.influences.resize(vertices.size());

    // A rigid section binds every vertex to one node and never reads the skin
    // (§5.6), exactly as the render view did.
    const std::optional<u32> rigid = section != nullptr ? section->rigidNode : std::nullopt;
    std::vector<std::vector<geom::Influence>> gathered(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (rigid.has_value()) {
            const geom::Influence one{*rigid, 1.0f};
            gathered[i] = WritableInfluences(std::span<const geom::Influence>(&one, 1),
                                             context.objectIdOf);
        } else {
            gathered[i] = WritableInfluences(context.mesh.skin.forVertex(vertices[i]),
                                             context.objectIdOf);
        }
    }

    if (context.classic) {
        skinning::ClassicLimits limits;
        limits.maxBones = Profile(ProfileId::Wc3Classic).maxBoneInfluences;
        std::vector<u16> pins;
        if (!context.pins.empty()) {
            pins.reserve(vertices.size());
            for (const u32 vertex : vertices) {
                pins.push_back(vertex < context.pins.size() ? context.pins[vertex] : u16{0});
            }
        }
        // Each geoset vertex's WEM vertex as its source: a uv1 seam that splits
        // a vertex in one slicing and not in another must not change which
        // group the 256 limit merges away.
        out.classic = skinning::QuantizeClassic(gathered, pins, limits, vertices);
        out.unbound = out.classic.unboundVertices;
        // What the group could not hold, counted after the prune: a bleed
        // share is never in a group whatever the vertex has.
        overLimit += out.classic.wide;
        for (std::size_t i = 0; i < out.classic.groupOf.size() && i < vertices.size(); ++i) {
            const std::vector<u32>& group = out.classic.groups[out.classic.groupOf[i]];
            const f32 share = 1.0f / static_cast<f32>(group.size());
            for (const u32 node : group) {
                out.influences[i].push_back({node, share, 0});
            }
        }
        return out;
    }

    // A vertex that binds nothing the file writes, in a geoset where others
    // bind: four zero bytes would weigh it by nothing at all, so it is bound
    // wholly to the node the first bound vertex leans on most -- the classic
    // quantizer's rule, and one bone for the vertex where the game would
    // otherwise collapse it. It is listed, so the invented binding still reads
    // as unskinned.
    std::optional<u32> firstBound;
    for (const std::vector<geom::Influence>& one : gathered) {
        if (!one.empty()) {
            firstBound = one.front().bone;
            break;
        }
    }

    // `SKIN`: four lanes, folded over the skeleton past them, then one rounding
    // for the four so the bytes sum to 255.
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        std::vector<geom::Influence> kept = std::move(gathered[i]);
        if (kept.empty() && firstBound.has_value()) {
            kept.push_back({*firstBound, 1.0f});
            out.unbound.push_back(static_cast<u32>(i));
        }
        if (kept.size() > 4) {
            ++overLimit;
            const Vector3f position = vertices[i] < context.positions.size()
                                          ? context.positions[vertices[i]]
                                          : Vector3f{0, 0, 0};
            kept = geom::FoldInfluences(kept, 4, position, context.skeleton.parents,
                                        context.skeleton.pivots);
        }
        std::array<f32, 4> shares{};
        for (std::size_t k = 0; k < kept.size() && k < 4; ++k) {
            shares[k] = kept[k].weight;
        }
        const std::array<u8, 4> bytes = skinning::QuantizeWeights(shares, kept.size());
        std::vector<WrittenInfluence>& written = out.influences[i];
        for (std::size_t k = 0; k < kept.size() && k < 4; ++k) {
            if (bytes[k] != 0) {
                written.push_back({kept[k].bone, static_cast<f32>(bytes[k]) / 255.0f, bytes[k]});
            }
        }
        std::stable_sort(written.begin(), written.end(),
                         [](const WrittenInfluence& a, const WrittenInfluence& b) {
                             if (a.byte != b.byte) {
                                 return a.byte > b.byte;
                             }
                             return a.node < b.node;
                         });
    }
    return out;
}

/// What the quantizer did to one geoset, said once: the 256-group merge, or the
/// pins' refusal.
void ReportClassic(const skinning::ClassicSkin& classic, u32 mesh, Diagnostics& diagnostics) {
    if (classic.pinsOverflow) {
        diagnostics.error(DiagCode::BonePaletteLimit,
                          "the classic pins alone need more than 256 groups in one geoset",
                          ElementRef(ElementKind::Mesh, mesh));
    } else if (classic.mergedGroups != 0) {
        diagnostics.warn(DiagCode::BonePaletteLimit,
                         "section needs " +
                             std::to_string(classic.groups.size() + classic.mergedGroups) +
                             " bone groups; " + std::to_string(classic.mergedGroups) +
                             " merged into their nearest, moving " +
                             std::to_string(classic.merged) + " vertices",
                         ElementRef(ElementKind::Mesh, mesh));
    }
}

/// Once per mesh: the vertices whose influences the encoding could not hold.
void ReportInfluenceLimit(u32 overLimit, bool classic, u32 mesh, Diagnostics& diagnostics) {
    if (overLimit == 0) {
        return;
    }
    const u32 width = classic ? Profile(ProfileId::Wc3Classic).maxBoneInfluences : 4u;
    diagnostics.warn(DiagCode::BoneInfluenceLimit,
                     std::to_string(overLimit) + " vertex/vertices carry more than " +
                         std::to_string(width) + " influences; " +
                         (classic ? "their group keeps the heaviest"
                                  : "the surplus folded into the nearest joints"),
                     ElementRef(ElementKind::Mesh, mesh));
}

/**
 * @brief Writes one geoset's `GNDX` / `MTGC` / `MATS` and, for Reforged, its
 *        `SKIN`, from `GeosetSkin`'s answer.
 *
 * Two encodings of the same fact, and all three group chunks go in every file
 * because the writer emits them unconditionally.
 *
 * - **Classic** (@p classic): `GNDX`/`MTGC`/`MATS` is what Warcraft III classic
 *   skins with, and it has no weights: `MTGC` gives the size of each group,
 *   `MATS` is the groups concatenated, and a vertex names a group whose bones
 *   are averaged **uniformly**. The groups are the Skin Quantizer's, never a
 *   repeat (Q2), at most 256 because `GNDX` is a byte.
 * - **`SKIN`** carries four (bone, weight) pairs per vertex and indexes `MATS`
 *   directly, which is why a Reforged file writes the degenerate form of the
 *   groups instead: `LadyAlexstraszaReforged.mdx` has `MATS` = the identity
 *   over all 242 bones, `MTGC` = 242 ones, and `GNDX` pointing each vertex at
 *   its heaviest. Below v1400 a `SKIN` slot is a byte, so a palette past 256
 *   is refused, naming the mesh, rather than dropping the weight.
 *
 * @return false when the geoset cannot be written as asked; the error is said.
 */
bool WriteGeosetSkin(mdx::Geoset& geoset, const WrittenGeosetSkin& skin,
                     const std::vector<u32>& objectIdOf, bool classic, u32 targetVersion,
                     u32 mesh, Diagnostics& diagnostics) {
    if (classic) {
        const skinning::ClassicSkin& groups = skin.classic;
        ReportClassic(groups, mesh, diagnostics);
        if (groups.pinsOverflow) {
            return false;
        }
        if (groups.groups.empty()) {
            return true;
        }
        geoset.matrixGroups.reserve(groups.groups.size());
        for (const std::vector<u32>& group : groups.groups) {
            geoset.matrixGroups.push_back(static_cast<u32>(group.size()));
            for (const u32 node : group) {
                geoset.matrixIndices.push_back(objectIdOf[node]);
            }
        }
        geoset.vertexGroups.reserve(groups.groupOf.size());
        for (const u32 group : groups.groupOf) {
            geoset.vertexGroups.push_back(static_cast<u8>(group));
        }
        return true;
    }

    std::vector<u32> palette;
    std::unordered_map<u32, u32> paletteOf;
    for (const std::vector<WrittenInfluence>& influences : skin.influences) {
        for (const WrittenInfluence& influence : influences) {
            const u32 objectId = objectIdOf[influence.node];
            if (paletteOf.try_emplace(objectId, static_cast<u32>(palette.size())).second) {
                palette.push_back(objectId);
            }
        }
    }
    if (palette.empty()) {
        return true;
    }
    if (palette.size() > 256 && targetVersion < 1400) {
        diagnostics.error(DiagCode::BonePaletteLimit,
                          "section binds " + std::to_string(palette.size()) +
                              " bones; a geoset palette holds 256 below v1400",
                          ElementRef(ElementKind::Mesh, mesh));
        return false;
    }
    geoset.matrixIndices = palette;
    geoset.matrixGroups.assign(palette.size(), 1u);
    geoset.vertexGroups.reserve(skin.influences.size());
    geoset.skinData.assign(skin.influences.size() * 8, 0);
    for (std::size_t v = 0; v < skin.influences.size(); ++v) {
        const std::vector<WrittenInfluence>& influences = skin.influences[v];
        // `GNDX` names the heaviest; `SKIN` is what the game reads, so a slot a
        // byte cannot name (v1400 and later only) is clamped here alone.
        const u32 heaviest =
            influences.empty() ? 0u : paletteOf[objectIdOf[influences.front().node]];
        geoset.vertexGroups.push_back(static_cast<u8>(std::min<u32>(heaviest, 0xFFu)));
        for (std::size_t k = 0; k < influences.size() && k < 4; ++k) {
            geoset.skinData[v * 8 + k] =
                static_cast<u16>(paletteOf[objectIdOf[influences[k].node]]);
            geoset.skinData[v * 8 + 4 + k] = influences[k].byte;
        }
    }
    return true;
}

/// The `classicBones` pins of @p mesh, per vertex; empty when it has none
/// (EDIT_MODE_SKIN_DESIGN.md §12.6). The one part of the skin setup a file sees,
/// and only ever through the classic groups.
std::span<const u16> ClassicPinsOf(const Mesh& mesh) {
    return mesh.attributes.get<u16>(geom::names::kClassicBones, geom::Domain::Vertex);
}

/**
 * @brief One render range as the geoset `toMdx` writes: its own vertex slice in
 *        first-use order, its faces, its bounds and its skin.
 *
 * Shared by the export and by `checkGeoset`, so what the check refuses is what
 * the export would have said. @p refused is set when the skin cannot be
 * written as asked.
 */
mdx::Geoset BuildGeosetFromRange(const Mesh& mesh, u32 meshIndex, const geom::RenderMesh& render,
                                 const GeosetStreams& streams, const geom::RenderRange& range,
                                 std::vector<u32>& localOf, const SkinContext& skin,
                                 u32 targetVersion, u32& overLimit, bool& refused,
                                 Diagnostics& diagnostics) {
    const MeshSection* section =
        range.section < mesh.sections.size() ? &mesh.sections[range.section] : nullptr;

    mdx::Geoset geoset;
    geoset.lod = mesh.lodLevel;
    geoset.lodName = section != nullptr && !section->name.empty() ? section->name : mesh.name;

    const GeosetSlice slice = SliceRange(render, range, localOf);
    const std::vector<u32>& sourceOf = slice.sourceOf;
    geoset.faces.reserve(slice.faces.size());
    bool wide = false;
    for (const u32 local : slice.faces) {
        const u32 index = local == kUnmappedVertex ? 0u : local;
        wide = wide || index > 0xFFFFu;
        geoset.faces.push_back(static_cast<u16>(index & 0xFFFFu));
    }
    if (wide) {
        diagnostics.warn(DiagCode::IndexWidthExceeded,
                         "section needs more than 65535 vertices for one geoset",
                         ElementRef(ElementKind::Mesh, meshIndex));
    }
    geoset.faceTypeGroups.push_back(4);
    geoset.faceGroups.push_back(static_cast<u32>(geoset.faces.size()));

    const auto& positions = streams.positions;
    const auto& normals = streams.normals;
    const auto& uv0 = streams.uv0;
    const auto& tangents = streams.tangents;
    geoset.vertexPositions.reserve(sourceOf.size());
    geoset.vertexNormals.reserve(sourceOf.size());
    if (!tangents.empty()) {
        geoset.tangents.reserve(sourceOf.size());
    }
    std::vector<Vector2f> uvs;
    if (!uv0.empty()) {
        uvs.reserve(sourceOf.size());
    }
    std::vector<Vector2f> uvs1;
    if (!streams.uv1.empty()) {
        uvs1.reserve(sourceOf.size());
    }
    Extent bounds;
    ResetExtent(bounds);
    for (const u32 source : sourceOf) {
        const Vector3f position = source < positions.size() ? positions[source] : Vector3f(0, 0, 0);
        geoset.vertexPositions.push_back(position);
        GrowExtent(bounds, position);
        geoset.vertexNormals.push_back(source < normals.size() ? normals[source]
                                                               : Vector3f(0, 0, 1));
        if (!tangents.empty()) {
            geoset.tangents.push_back(source < tangents.size() ? tangents[source]
                                                               : Vector4f(1, 0, 0, 1));
        }
        if (!uv0.empty()) {
            uvs.push_back(source < uv0.size() ? uv0[source] : Vector2f(0, 0));
        }
        if (!streams.uv1.empty()) {
            uvs1.push_back(source < streams.uv1.size() ? streams.uv1[source] : Vector2f(0, 0));
        }
    }
    if (!uvs.empty()) {
        geoset.textureCoordinateSets.push_back(std::move(uvs));
        if (!uvs1.empty()) {
            geoset.textureCoordinateSets.push_back(std::move(uvs1));
        }
    }
    // Derived, like every other bound in WEM. It is also the one thing the
    // merged geoset could not state: a section's own volume.
    if (!sourceOf.empty()) {
        FinishExtent(bounds);
        geoset.extent = FromExtent(bounds);
    } else {
        geoset.extent = FromExtent(mesh.bounds);
    }

    const WrittenGeosetSkin written =
        GeosetSkin(skin, section, WemVerticesOf(render, slice), overLimit);
    if (!WriteGeosetSkin(geoset, written, skin.objectIdOf, skin.classic, targetVersion, meshIndex,
                         diagnostics)) {
        refused = true;
    }
    return geoset;
}

} // namespace

// ============================================================================
// What the file holds for each vertex (EDIT_MODE_SKIN_DESIGN.md §12.3-12.4)
// ============================================================================

std::vector<std::vector<u32>> MdxGeosetVertices(const Document& document, u32 model,
                                                ProfileId profile) {
    std::vector<std::vector<u32>> out;
    if (model >= document.models.size()) {
        return out;
    }
    // Every section is written whatever the profile draws; the profile decides
    // only whether a second UV set can split vertices.
    for (const Mesh& mesh : document.models[model].meshes) {
        const geom::RenderMesh render =
            geom::BuildRenderMesh(mesh, GeosetRenderDesc(WritesSecondUvSet(mesh, profile)));
        if (render.ranges.empty()) {
            out.resize(out.size() + std::max<std::size_t>(1, mesh.sections.size()));
            continue;
        }
        std::vector<u32> localOf(render.vertexCount(), kUnmappedVertex);
        for (const geom::RenderRange& range : render.ranges) {
            out.push_back(WemVerticesOf(render, SliceRange(render, range, localOf)));
        }
    }
    return out;
}

WrittenSkin MdxConverter::writtenSkin(const Document& document, u32 model, ProfileId profile,
                                      u32 mesh, std::optional<ProfileId> skinAs) const {
    WrittenSkin result;
    result.classic = skinAs.value_or(profile) == ProfileId::Wc3Classic;
    if (model >= document.models.size() || mesh >= document.models[model].meshes.size()) {
        result.diagnostics.error(DiagCode::IndexOutOfRange, "no such model or mesh");
        return result;
    }
    const Model& owner = document.models[model];
    const Mesh& target = owner.meshes[mesh];
    const std::vector<u32> objectIdOf = MdxExportMapOf(document, model, profile).nodeObjectId;
    const geom::RenderMesh render =
        geom::BuildRenderMesh(target, GeosetRenderDesc(WritesSecondUvSet(target, profile)));
    if (render.ranges.empty()) {
        result.geosets.resize(std::max<std::size_t>(1, target.sections.size()));
        return result;
    }
    const SkinSkeleton skeleton(owner.nodes);
    const SkinContext context{target,
                              objectIdOf,
                              skeleton,
                              target.attributes.get<Vector3f>(geom::names::kPosition,
                                                              geom::Domain::Vertex),
                              result.classic,
                              ClassicPinsOf(target)};
    std::vector<u32> localOf(render.vertexCount(), kUnmappedVertex);
    for (const geom::RenderRange& range : render.ranges) {
        const MeshSection* section =
            range.section < target.sections.size() ? &target.sections[range.section] : nullptr;
        result.geosets.push_back(GeosetSkin(context, section,
                                            WemVerticesOf(render, SliceRange(render, range, localOf)),
                                            result.overLimit));
        if (result.classic) {
            ReportClassic(result.geosets.back().classic, mesh, result.diagnostics);
        }
    }
    ReportInfluenceLimit(result.overLimit, result.classic, mesh, result.diagnostics);
    return result;
}

Diagnostics MdxConverter::checkGeoset(const Document& document, u32 model, const Mesh& mesh,
                                      ProfileId profile, u32 targetVersion,
                                      u32* writtenVertices) const {
    Diagnostics out;
    if (writtenVertices != nullptr) {
        *writtenVertices = 0;
    }
    if (model >= document.models.size()) {
        out.error(DiagCode::IndexOutOfRange, "the document has no model " + std::to_string(model));
        return out;
    }
    const Model& owner = document.models[model];
    const std::vector<u32> objectIdOf = MdxExportMapOf(document, model, profile).nodeObjectId;
    const bool secondUvSet = WritesSecondUvSet(mesh, profile);
    const geom::RenderMesh render = geom::BuildRenderMesh(mesh, GeosetRenderDesc(secondUvSet));
    out.append(render.diagnostics);
    const GeosetStreams streams(render, mesh, targetVersion, secondUvSet);
    const SkinSkeleton skeleton(owner.nodes);
    const SkinContext skin{mesh,
                           objectIdOf,
                           skeleton,
                           mesh.attributes.get<Vector3f>(geom::names::kPosition,
                                                         geom::Domain::Vertex),
                           targetVersion <= 800,
                           ClassicPinsOf(mesh)};
    std::vector<u32> localOf(render.vertexCount(), kUnmappedVertex);
    u32 overLimit = 0;
    bool refused = false;
    for (const geom::RenderRange& range : render.ranges) {
        const mdx::Geoset geoset =
            BuildGeosetFromRange(mesh, kInvalidIndex, render, streams, range, localOf, skin,
                                 targetVersion, overLimit, refused, out);
        if (writtenVertices != nullptr) {
            *writtenVertices += static_cast<u32>(geoset.vertexPositions.size());
        }
    }
    ReportInfluenceLimit(overLimit, skin.classic, kInvalidIndex, out);
    return out;
}

// ============================================================================
// toMdx
// ============================================================================

u32 MdxFileVersion(ProfileId profile) {
    switch (profile) {
    case ProfileId::Wc3Classic:
        return 800;
    case ProfileId::Wc3Reforged:
        return 1800;
    default:
        return 0;
    }
}

Result<mdx::Model> MdxConverter::toMdx(const Document& document, ProfileId profile,
                                       u32 targetVersion, std::optional<ProfileId> skinAs) const {
    if (targetVersion == 0) {
        targetVersion = MdxFileVersion(profile);
    }
    Result<mdx::Model> result;
    if (!checkExportProfile(document, profile, result.diagnostics)) {
        return result;
    }
    checkRigConvention(document, profile, result.diagnostics);
    checkNodeKinds(document, profile, result.diagnostics);
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
    const std::vector<u32> wrapBits = TextureWrapBits(document, set);
    for (std::size_t t = 0; t < document.textures.size(); ++t) {
        const TextureRef& ref = document.textures[t];
        mdx::Texture texture;
        texture.fileName = ref.path;
        // `TextureRef::flags` is MDX's own wrap word only when an `.mdx` put it
        // there; for every other source it is empty and means nothing, so the
        // materials say what the texture is sampled with (`TextureWrapBits`).
        texture.flags = static_cast<mdx::Texture::Flag>(authoredAsMdx ? ref.flags : wrapBits[t]);
        texture.replaceableId = authoredAsMdx ? ref.replaceableId : 0u;
        context.textureIndexMap.push_back(static_cast<u32>(out.textures.size()));
        out.textures.push_back(std::move(texture));
    }

    // A Reforged material names files no document holds: Warcraft III's own
    // neutral maps, one per HD slot it has nothing of its own for
    // (`mdx_core::StockTextureFor`). They are interned while the materials are
    // written and appended below, so the document's own textures keep the
    // indices every other part of the export assumes are theirs -- the host's
    // baked ORM among them, which is keyed by document index.
    std::vector<mdx::Texture> stockTextures;
    context.stockTextures = &stockTextures;
    context.stockBase = static_cast<u32>(out.textures.size());

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

    // The numbering, and the sequence each clip becomes, are `MdxExportMap`'s,
    // so a caller that asks where a node or clip went gets this export's answer.
    // A camera has no id (`kInvalidIndex`, which is also `NO_PARENT`).
    const MdxExportMap exportMap = MdxExportMapOf(document, 0, profile);
    const std::vector<u32>& objectIdOf = exportMap.nodeObjectId;
    animContext.clipSequence = exportMap.clipSequence;
    u32 nextObjectId = 0;
    for (const u32 id : objectIdOf) {
        if (id != kInvalidIndex) {
            nextObjectId = std::max(nextObjectId, id + 1u);
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
                camera.targetPosition = payload->target;
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
        switch (WrittenKind(node.kind, profile)) {
        case NodeKind::Bone: {
            mdx::Bone bone;
            bone.node = buildNode(i);
            // `geosetId` and `geosetAnimationId` are written once every
            // geoset animation exists (`LinkGeosetAnimations`).
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
                // Written from v1300/v1600 only (`mdx/writer.cpp`); below, the
                // game substitutes its own, which are the payload's defaults.
                light.shadowCasting = payload->shadowCasting;
                light.shadowCastingStart = payload->shadowCastingStart;
                light.shadowCastingEnd = payload->shadowCastingEnd;
                light.quadraticFalloff = payload->quadraticFalloff;
                light.linearFalloff = payload->linearFalloff;
                light.damping = payload->damping;
                if (payload->kind == LightKind::Spot) {
                    diagnostics.warn(DiagCode::FeatureDropped,
                                     "WC3 has no spot light; written as omni",
                                     ElementRef(ElementKind::Node, static_cast<u32>(i)));
                }
            }
            // The import's ambient and shadow keys; an absent key leaves the
            // record's default.
            light.ambientIntensity = Milli(node.native, "ambientIntensity", light.ambientIntensity);
            light.shadowIntensity = Milli(node.native, "shadowIntensity", light.shadowIntensity);
            {
                const auto* r = node.native.find("ambientColorR");
                const auto* g = node.native.find("ambientColorG");
                const auto* b = node.native.find("ambientColorB");
                if (r != nullptr && g != nullptr && b != nullptr) {
                    const auto value = [](const auto* entry) {
                        return std::bit_cast<f32>(static_cast<u32>(entry->value));
                    };
                    light.ambientColor = Vector3f(value(r), value(g), value(b));
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
            if (const auto* payload = std::get_if<AttachmentPayload>(&node.payload)) {
                attachment.path = payload->asset.path;
            }
            claim(i, mdx_anim::ExportContext::Slot::Attachment, out.attachments.size());
            out.attachments.push_back(std::move(attachment));
            break;
        }
        case NodeKind::Wc3ParticleEmitter1: {
            const auto& payload = std::get<Wc3ParticleEmitter1Payload>(node.payload);
            mdx::ParticleEmitter emitter;
            emitter.node = buildNode(i);
            u32 bits = static_cast<u32>(emitter.node.flags);
            bits = WithBit(bits, kFlagUsesMdl, payload.usesMdl);
            bits = WithBit(bits, kFlagUsesTga, payload.usesTga);
            emitter.node.flags = static_cast<mdx::Node::NodeFlag>(bits);
            emitter.emissionRate = payload.emissionRate;
            emitter.gravity = payload.gravity;
            emitter.longitude = payload.longitude;
            emitter.latitude = payload.latitude;
            emitter.spawnModelFileName = payload.spawnModel.path;
            emitter.lifespan = payload.lifespan;
            emitter.initialVelocity = payload.speed;
            claim(i, mdx_anim::ExportContext::Slot::ParticleEmitter, out.particleEmitters.size());
            out.particleEmitters.push_back(std::move(emitter));
            break;
        }
        case NodeKind::Wc3ParticleEmitter2: {
            const auto& payload = std::get<Wc3ParticleEmitter2Payload>(node.payload);
            mdx::ParticleEmitter2 emitter;
            emitter.node = buildNode(i);
            u32 bits = static_cast<u32>(emitter.node.flags);
            bits = WithBit(bits, kFlagUnshaded, payload.unshaded);
            bits = WithBit(bits, kFlagSortPrims, payload.sortPrimsFarZ);
            bits = WithBit(bits, kFlagLineEmitter, payload.lineEmitter);
            bits = WithBit(bits, kFlagUnfogged, payload.unfogged);
            bits = WithBit(bits, kFlagXyQuad, payload.xyQuad);
            emitter.node.flags = static_cast<mdx::Node::NodeFlag>(bits);
            emitter.speed = payload.speed;
            emitter.variation = payload.variation;
            emitter.latitude = payload.latitude;
            emitter.gravity = payload.gravity;
            emitter.lifespan = payload.lifespan;
            emitter.emissionRate = payload.emissionRate;
            emitter.length = payload.length;
            emitter.width = payload.width;
            emitter.filterMode = static_cast<u32>(payload.filter);
            emitter.rows = payload.rows;
            emitter.columns = payload.columns;
            emitter.headOrTail = static_cast<u32>(payload.headOrTail);
            emitter.tailLength = payload.tailLength;
            emitter.time = payload.time;
            const Wc3ParticleSegment* segments[3] = {&payload.start, &payload.middle,
                                                     &payload.end};
            for (int s = 0; s < 3; ++s) {
                emitter.segmentColor[s] = segments[s]->color;
                emitter.segmentAlpha[s] = segments[s]->alpha;
                emitter.segmentScaling[s] = segments[s]->scaling;
            }
            const auto interval = [](const Wc3ParticleInterval& source) {
                return std::array<u32, 3>{source.start, source.end, source.repeat};
            };
            emitter.headInterval = interval(payload.headLife);
            emitter.headDecayInterval = interval(payload.headDecay);
            emitter.tailInterval = interval(payload.tailLife);
            emitter.tailDecayInterval = interval(payload.tailDecay);
            // `TEXS` is the document's textures in order, so the index crosses
            // as it is. A PRE2 names a texture whatever else it does; an
            // emitter an editor made without one draws the first.
            if (payload.texture == kInvalidIndex || payload.texture >= document.textures.size()) {
                diagnostics.warn(DiagCode::TextureUnresolved,
                                 "particle emitter '" + node.name +
                                     "' names no texture; written with the first",
                                 ElementRef(ElementKind::Node, static_cast<u32>(i)), profile);
                emitter.textureId = 0;
            } else {
                emitter.textureId = payload.texture;
            }
            emitter.replaceableId = payload.replaceableId;
            emitter.squirt = payload.squirt ? 1u : 0u;
            emitter.priorityPlane = payload.priorityPlane;
            claim(i, mdx_anim::ExportContext::Slot::ParticleEmitter2, out.particleEmitters2.size());
            out.particleEmitters2.push_back(std::move(emitter));
            break;
        }
        case NodeKind::Wc3RibbonEmitter: {
            const auto& payload = std::get<Wc3RibbonEmitterPayload>(node.payload);
            mdx::RibbonEmitter emitter;
            emitter.node = buildNode(i);
            emitter.heightAbove = payload.heightAbove;
            emitter.heightBelow = payload.heightBelow;
            emitter.alpha = payload.alpha;
            emitter.color = payload.color;
            emitter.lifespan = payload.lifespan;
            emitter.textureSlot = payload.textureSlot;
            emitter.emissionRate = payload.emissionRate;
            emitter.rows = payload.rows;
            emitter.columns = payload.columns;
            // One MDX material per slot (below), so the slot is the material.
            emitter.materialId = payload.materialSlot < model.materialSlots.size()
                                     ? payload.materialSlot
                                     : 0u;
            emitter.gravity = payload.gravity;
            claim(i, mdx_anim::ExportContext::Slot::RibbonEmitter, out.ribbonEmitters.size());
            out.ribbonEmitters.push_back(std::move(emitter));
            break;
        }
        case NodeKind::Wc3CornEmitter: {
            const auto& payload = std::get<Wc3CornEmitterPayload>(node.payload);
            mdx::CornEmitter emitter;
            emitter.node = buildNode(i);
            u32 bits = static_cast<u32>(emitter.node.flags);
            bits = WithBit(bits, kFlagUnshaded, payload.unshaded);
            bits = WithBit(bits, kFlagSortPrims, payload.sortPrimsFarZ);
            bits = WithBit(bits, kFlagCornUnfogged, payload.unfogged);
            bits = WithBit(bits, kFlagCornScaling, payload.popcornScaling);
            emitter.node.flags = static_cast<mdx::Node::NodeFlag>(bits);
            emitter.lifeSpan = payload.lifespan;
            emitter.emissionRate = payload.emissionRate;
            emitter.speed = payload.speed;
            emitter.color = payload.color;
            emitter.alpha = payload.alpha;
            emitter.replaceableId = payload.replaceableId;
            emitter.path = payload.effect.path;
            emitter.animVisibilityGuide = payload.animVisibilityGuide;
            claim(i, mdx_anim::ExportContext::Slot::CornEmitter, out.cornEmitters.size());
            out.cornEmitters.push_back(std::move(emitter));
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
                // Both carry two vertices, which import reads into the box's
                // corners; a chunk written without them is one every reader
                // misparses from there on.
                case CollisionShapeKind::Plane:
                    shape.type = mdx::CollisionShape::ShapeType::Plane;
                    shape.vertices.push_back(payload->shape.box.minimum);
                    shape.vertices.push_back(payload->shape.box.maximum);
                    break;
                case CollisionShapeKind::Cylinder:
                    shape.type = mdx::CollisionShape::ShapeType::Cylinder;
                    shape.vertices.push_back(payload->shape.box.minimum);
                    shape.vertices.push_back(payload->shape.box.maximum);
                    shape.radius = payload->shape.sphere.radius;
                    break;
                case CollisionShapeKind::Capsule:
                case CollisionShapeKind::Hull:
                    diagnostics.warn(DiagCode::FeatureDropped,
                                     "WC3 has no capsule or hull collision shape; written as box",
                                     ElementRef(ElementKind::Node, static_cast<u32>(i)));
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
        // The export says which layer each ordinal became; it is not the
        // identity, because a chain drops a stage that draws nothing and a
        // replacing stage clears everything written before it.
        out.materials.push_back(mdx_core::ExportMaterial(*material, profile, context, diagnostics,
                                                         &animContext.layerOfOrdinal[slot]));
    }
    // A profile that holds one UV set writes one (`WritesSecondUvSet`), so a
    // layer naming set 1 would read past every geoset. It takes set 0, which is
    // also what the viewer draws for a geoset without the set it names.
    if (Profile(profile).maxUvSets < 2) {
        u32 moved = 0;
        for (mdx::Material& material : out.materials) {
            for (mdx::Layer& layer : material.layers) {
                if (layer.coordId != 0) {
                    layer.coordId = 0;
                    ++moved;
                }
            }
        }
        if (moved != 0) {
            diagnostics.warn(DiagCode::UvSetLimit,
                             std::to_string(moved) + " layer(s) read a second UV set " +
                                 Profile(profile).name + " does not hold; they read the first");
        }
    }
    // Whatever the materials asked for, in the order they asked. `stockBase`
    // promised these ids and nothing has pushed a texture since.
    for (mdx::Texture& texture : stockTextures) {
        out.textures.push_back(std::move(texture));
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
    // `SKIN` and `TANG` are written only above 800 (mdx/writer.cpp), so at 800
    // the group encoding is the only skinning the file carries. Above it, a
    // caller previewing the classic file asks for the groups alone (§12.3).
    const bool classic = targetVersion <= 800 || skinAs == ProfileId::Wc3Classic;
    const SkinSkeleton skinSkeleton(model.nodes);
    bool refused = false;
    // Numbered by the export map, which is what a host marks geosets through.
    animContext.geosetsOfMesh = exportMap.geosetsOfMesh;
    animContext.sectionOfGeoset.assign(model.meshes.size(), {});
    // The geosets a hidden or tinted section produced, turned into geoset
    // animations once every geoset exists.
    std::vector<u32> hiddenGeosets;
    std::vector<std::pair<u32, GeosetTint>> tintedGeosets;
    // What a geoset takes from its section, called just before it is pushed.
    const auto takeSection = [&](mdx::Geoset& geoset, const MeshSection* section) {
        if (section == nullptr) {
            return;
        }
        geoset.materialId = section->materialSlot;
        geoset.selectionGroup = section->selectionGroup;
        if (const auto* flags = section->native.find("selectionFlags")) {
            geoset.selectionFlags = static_cast<u32>(flags->value);
        }
        const GeosetTint tint = geosetTint(*section);
        const u32 index = static_cast<u32>(out.geosets.size());
        if (tint.hidden) {
            hiddenGeosets.push_back(index);
        }
        if (tint.color != Vector3f(1.0f, 1.0f, 1.0f) || tint.alpha != 1.0f) {
            tintedGeosets.emplace_back(index, tint);
        }
    };

    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        const bool secondUvSet = WritesSecondUvSet(mesh, profile);
        const geom::RenderMesh render = geom::BuildRenderMesh(mesh, GeosetRenderDesc(secondUvSet));
        diagnostics.append(render.diagnostics);
        if (render.ranges.empty()) {
            // The render view failed and said why. The map promised this mesh
            // its geosets, so they are written empty rather than renumbering
            // every geoset after them.
            for (std::size_t k = 0; k < animContext.geosetsOfMesh[m].size(); ++k) {
                const MeshSection* section = k < mesh.sections.size() ? &mesh.sections[k] : nullptr;
                mdx::Geoset geoset;
                geoset.lod = mesh.lodLevel;
                geoset.lodName =
                    section != nullptr && !section->name.empty() ? section->name : mesh.name;
                geoset.extent = FromExtent(mesh.bounds);
                takeSection(geoset, section);
                animContext.sectionOfGeoset[m].push_back(static_cast<u32>(k));
                out.geosets.push_back(std::move(geoset));
            }
            continue;
        }

        const GeosetStreams streams(render, mesh, targetVersion, secondUvSet);
        const SkinContext skin{mesh,
                               objectIdOf,
                               skinSkeleton,
                               mesh.attributes.get<Vector3f>(geom::names::kPosition,
                                                             geom::Domain::Vertex),
                               classic,
                               ClassicPinsOf(mesh)};
        std::vector<u32> localOf(render.vertexCount(), kUnmappedVertex);
        u32 overLimit = 0;
        for (const geom::RenderRange& range : render.ranges) {
            const MeshSection* section =
                range.section < mesh.sections.size() ? &mesh.sections[range.section] : nullptr;
            mdx::Geoset geoset =
                BuildGeosetFromRange(mesh, static_cast<u32>(m), render, streams, range, localOf,
                                     skin, targetVersion, overLimit, refused, diagnostics);
            takeSection(geoset, section);
            animContext.sectionOfGeoset[m].push_back(range.section);
            out.geosets.push_back(std::move(geoset));
        }
        ReportInfluenceLimit(overLimit, classic, static_cast<u32>(m), diagnostics);
    }
    // A geoset whose skin cannot be written as asked fails the export, named;
    // a file that binds the wrong bones is worse than none.
    if (refused) {
        return result;
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
    // A static tint or partial alpha goes back onto the geoset's record.
    for (const auto& [geoset, tint] : tintedGeosets) {
        mdx::GeosetAnimation* animation = nullptr;
        for (mdx::GeosetAnimation& existing : out.geosetAnimations) {
            if (existing.geosetId == geoset) {
                animation = &existing;
            }
        }
        if (animation == nullptr) {
            mdx::GeosetAnimation created;
            created.geosetId = geoset;
            created.flags = mdx::GeosetAnimation::Flag::Color;
            out.geosetAnimations.push_back(std::move(created));
            animation = &out.geosetAnimations.back();
        }
        animation->color = tint.color;
        // A hidden geoset writes 0 and keeps the document's alpha for later.
        if (!tint.hidden) {
            animation->alpha = tint.alpha;
        }
    }

    // Last, because a geoset animation names a geoset and an event object has
    // to exist before its times can be written onto it.
    mdx_anim::Export(document, 0, profile, animContext, out, diagnostics);
    // And after it, because the keyed records are only now all made.
    LinkGeosetAnimations(model, animContext, out, diagnostics);

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

// Classic's. The version a file is written at depends on the profile, which
// this cannot see: `exportToBytes` and `toMdx` resolve 0 through
// `MdxFileVersion`, so a Reforged file is never written at this one.
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
    Result<mdx::Model> converted = toMdx(document, profile, version);
    Result<std::vector<u8>> result;
    result.diagnostics = std::move(converted.diagnostics);
    if (!converted.ok()) {
        return result;
    }
    mdx::Writer writer;
    result.value = writer.write(*converted);
    return result;
}

// ============================================================================
// Editing a Warcraft III material (EDIT_MODE_MATERIALS_DESIGN.md §6)
// ============================================================================

namespace {

/// The context `fromMdx` builds, over a document that already exists: every
/// texture by identity (the index a block names IS the document's), the wrap
/// words read off the table, and the block's own version.
mdx_core::Context EditContext(const Document& document, u32 version) {
    mdx_core::Context context;
    context.modelVersion = version;
    context.textureIndexMap.resize(document.textures.size());
    for (std::size_t i = 0; i < document.textures.size(); ++i) {
        context.textureIndexMap[i] = static_cast<u32>(i);
    }
    context.textureRefs = &document.textures;
    return context;
}

/// @p block as the import reads it: its layers' HD-ness recomputed from what
/// says it in a file — the layer's shader from v1100, the material's name below
/// — because `isHd` is the import's normalisation of that, and an edit that
/// changed the shader leaves the flag behind it. `IsHdLayer` believes the flag
/// first, so a stale one would read an SD layer back as HD.
mdx::Material IncomingMaterial(const native::MdxMaterial& block) {
    mdx::Material material;
    CopyFromNative(block, material);
    for (mdx::Layer& layer : material.layers) {
        layer.is_hd = false;
        layer.is_hd = mdx_core::IsHdLayer(material, layer, block.sourceVersion);
    }
    return material;
}

/// @p block through the import, as a re-import of a saved file would see it.
/// @p ordinals receives the ordinal each block layer became.
Material ProjectBlock(const native::MdxMaterial& block, ProfileId profile,
                      const Document& document, Diagnostics& out, std::vector<u32>& ordinals) {
    return mdx_core::ImportMaterial(IncomingMaterial(block), profile,
                                    EditContext(document, block.sourceVersion), out, &ordinals);
}

/// Whether @p set binds @p material at (@p slot, @p look).
bool BindsMaterial(const ProfileMaterialSet& set, u32 slot, u32 look, u32 material) {
    return slot < set.slotBindings.size() && look < set.slotBindings[slot].byLook.size() &&
           set.slotBindings[slot].byLook[look] == material;
}

/// What a stock-texture lookup compares: the path as the game resolves it,
/// which ignores case and the slash direction.
std::string NormalisedTexturePath(const std::string& path) {
    std::string out = path;
    for (char& c : out) {
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

MaterialBlockResult MdxConverter::setMaterialBlock(Document& document,
                                                   const MaterialBlockEdit& edit) const {
    MaterialBlockResult result;
    Diagnostics& out = result.diagnostics;
    const auto number = [](u64 value) { return std::to_string(value); };

    if (edit.profile != ProfileId::Wc3Classic && edit.profile != ProfileId::Wc3Reforged) {
        out.error(DiagCode::OperationUnsupported,
                  std::string("an MDX block edits a Warcraft III set, not ") +
                      ToString(edit.profile),
                  ElementRef(), edit.profile);
        return result;
    }
    if (edit.model >= document.models.size()) {
        out.error(DiagCode::IndexOutOfRange,
                  "model " + number(edit.model) + " of " + number(document.models.size()));
        return result;
    }
    Model& model = document.models[edit.model];
    ProfileMaterialSet* set = model.setFor(edit.profile);
    if (set == nullptr) {
        out.error(DiagCode::ProfileNotCarried,
                  std::string("the model carries no ") + ToString(edit.profile) + " material set",
                  ElementRef(), edit.profile);
        return result;
    }
    if (edit.material >= set->materials.size()) {
        out.error(DiagCode::IndexOutOfRange,
                  "material " + number(edit.material) + " of " + number(set->materials.size()),
                  ElementRef(ElementKind::Material, edit.material), edit.profile);
        return result;
    }
    Material& target = set->materials[edit.material];
    const native::MdxMaterial* previous = std::get_if<native::MdxMaterial>(&target.Native());
    if (previous == nullptr) {
        out.error(DiagCode::OperationUnsupported, "the material has no MDX block to edit",
                  ElementRef(ElementKind::Material, edit.material), edit.profile);
        return result;
    }

    // --- 1. the shaders: the import would drop a layer the set may not hold --
    const ElementRef where(ElementKind::Material, edit.material);
    if (edit.block.layers.empty()) {
        // Not "draws nothing": an empty MDX stack draws untextured white. A
        // material that should not draw is a layer at alpha 0.
        out.error(DiagCode::MaterialBodyInvalid, "a material keeps at least one layer", where,
                  edit.profile);
        return result;
    }
    {
        // A Reforged model uses all four shaders and a classic one SD alone
        // (`mdx_core::LayerAllowedIn`), so only a classic set can refuse.
        const mdx::Material incoming = IncomingMaterial(edit.block);
        for (std::size_t i = 0; i < incoming.layers.size(); ++i) {
            if (mdx_core::LayerAllowedIn(incoming, incoming.layers[i], edit.block.sourceVersion,
                                         edit.profile)) {
                continue;
            }
            out.error(DiagCode::NativeKindProfileMismatch,
                      "layer " + number(i) + " is not an SD layer, and a classic model draws SD "
                      "layers only",
                      ElementRef(ElementKind::Layer, edit.material, static_cast<u32>(i)),
                      edit.profile);
            return result;
        }
    }
    if (!edit.layerRemap.empty()) {
        bool valid = edit.layerRemap.size() == previous->layers.size();
        for (const u32 to : edit.layerRemap) {
            valid = valid && (to == kInvalidIndex || to < edit.block.layers.size());
        }
        if (!valid) {
            out.error(DiagCode::IndexOutOfRange,
                      "the layer map names " + number(edit.layerRemap.size()) + " of " +
                          number(previous->layers.size()) + " old layers, or a new layer past " +
                          number(edit.block.layers.size()),
                      where, edit.profile);
            return result;
        }
    }

    // --- 2. project, both sides: the import is the one place that decides
    // which ordinal a layer is ------------------------------------------------
    Diagnostics before;
    std::vector<u32> oldOrdinals;
    ProjectBlock(*previous, edit.profile, document, before, oldOrdinals);
    std::vector<u32> newOrdinals;
    Material derived = ProjectBlock(edit.block, edit.profile, document, out, newOrdinals);
    // The import names a material after its `shader` string; this one keeps
    // the name it had, which is its slot's.
    derived.name = target.name;

    const auto newLayerOf = [&](u32 oldLayer) {
        if (edit.layerRemap.empty()) {
            return oldLayer < edit.block.layers.size() ? oldLayer : kInvalidIndex;
        }
        return oldLayer < edit.layerRemap.size() ? edit.layerRemap[oldLayer] : kInvalidIndex;
    };
    // Old ordinal -> old layer -> the map -> new layer -> new ordinal.
    const auto newOrdinalOf = [&](u32 oldOrdinal) -> u32 {
        if (oldOrdinal == kWholeMaterial) {
            return kWholeMaterial;
        }
        for (std::size_t layer = 0; layer < oldOrdinals.size(); ++layer) {
            if (oldOrdinals[layer] != oldOrdinal) {
                continue;
            }
            const u32 to = newLayerOf(static_cast<u32>(layer));
            return to < newOrdinals.size() ? newOrdinals[to] : kInvalidIndex;
        }
        return kInvalidIndex;
    };

    // --- 3. the features: sub-tracks join them by id -------------------------
    const std::vector<MaterialFeature>& oldFeatures = target.Common().features;
    std::vector<u32> joined;
    u32 nextId = NextFeatureId(oldFeatures);
    for (const AnimChannel& channel : model.animChannels.channels) {
        const TrackTarget& t = channel.target;
        if (t.kind != TrackTarget::Kind::MaterialFeature || t.material.profile != edit.profile ||
            !BindsMaterial(*set, t.material.slot, t.material.look, edit.material)) {
            continue;
        }
        joined.push_back(t.sub);
        // Never an id a channel still names, even one whose feature is gone.
        nextId = std::max(nextId, t.sub + 1);
    }
    const auto isJoined = [&](u32 id) {
        return std::find(joined.begin(), joined.end(), id) != joined.end();
    };

    CommonMaterial& common = derived.InitCommon();
    std::vector<MaterialFeature> features;
    std::vector<bool> carried(oldFeatures.size(), false);
    for (MaterialFeature feature : common.features) {
        bool matched = false;
        for (std::size_t o = 0; o < oldFeatures.size() && !matched; ++o) {
            if (carried[o] || oldFeatures[o].kind() != feature.kind() ||
                newOrdinalOf(oldFeatures[o].layer) != feature.layer) {
                continue;
            }
            feature.id = oldFeatures[o].id;
            carried[o] = true;
            matched = true;
        }
        if (!matched) {
            feature.id = nextId++;
        }
        features.push_back(std::move(feature));
    }
    std::vector<u32> droppedIds;
    for (std::size_t o = 0; o < oldFeatures.size(); ++o) {
        if (carried[o]) {
            continue;
        }
        const MaterialFeature& old = oldFeatures[o];
        // The import never makes a UV animation (the animation import does),
        // and a feature a channel joins on is the user's animation: a static
        // value typed to zero must not cut it.
        if (old.kind() != FeatureKind::UvAnimation && !isJoined(old.id)) {
            continue;
        }
        const u32 layer = newOrdinalOf(old.layer);
        if (layer == kInvalidIndex) {
            droppedIds.push_back(old.id);
            out.warn(DiagCode::FeatureDropped,
                     std::string(ToString(old.kind())) + " feature " + number(old.id) +
                         " was on a removed layer",
                     ElementRef(ElementKind::Feature, edit.material, old.id), edit.profile);
            continue;
        }
        MaterialFeature kept = old;
        kept.layer = layer;
        if (FresnelFeature* fresnel = kept.fresnel()) {
            for (std::size_t l = 0; l < newOrdinals.size(); ++l) {
                if (newOrdinals[l] != layer || l >= edit.block.layers.size()) {
                    continue;
                }
                const native::MdxLayer& source = edit.block.layers[l];
                fresnel->color = source.fresnelColor;
                fresnel->outMax = source.fresnelOpacity;
                fresnel->teamColor = source.fresnelTeamColor;
            }
        }
        features.push_back(std::move(kept));
    }
    common.features = std::move(features);

    // --- 4. the channels ------------------------------------------------------
    for (AnimChannel& channel : model.animChannels.channels) {
        TrackTarget& t = channel.target;
        if (!IsMaterialTarget(t.kind) || t.material.profile != edit.profile ||
            !BindsMaterial(*set, t.material.slot, t.material.look, edit.material)) {
            continue;
        }
        bool dead = false;
        if (t.kind == TrackTarget::Kind::MaterialLayer) {
            if (t.sub == kWholeMaterial) {
                continue;
            }
            const u32 to = newOrdinalOf(t.sub);
            dead = to == kInvalidIndex;
            if (!dead && to != t.sub) {
                t.sub = to;
                ++result.channelsRemapped;
            }
        } else {
            dead = std::find(droppedIds.begin(), droppedIds.end(), t.sub) != droppedIds.end();
        }
        if (!dead) {
            continue;
        }
        // Invalidated, never dropped: an id is never reused (§7.5).
        t.material.slot = kInvalidIndex;
        ++result.channelsInvalidated;
        out.warn(DiagCode::AnimChannelInvalidated,
                 "channel " + number(channel.id) + " animated a removed layer",
                 ElementRef(ElementKind::Channel, channel.id), edit.profile);
    }

    target = std::move(derived);
    result.ok = true;
    return result;
}

u32 MdxConverter::internStockTexture(Document& document, mdx::Layer::SlotType slot,
                                     bool* appended) const {
    if (appended != nullptr) {
        *appended = false;
    }
    const mdx_core::StockSlotTexture stock = mdx_core::StockTextureFor(slot);
    const std::string path = stock.path;
    if (path.empty() && stock.replaceableId == 0) {
        return kInvalidIndex;
    }
    const std::string wanted = NormalisedTexturePath(path);
    for (std::size_t i = 0; i < document.textures.size(); ++i) {
        const TextureRef& texture = document.textures[i];
        if (texture.replaceableId == stock.replaceableId &&
            NormalisedTexturePath(texture.path) == wanted) {
            return static_cast<u32>(i);
        }
    }
    TextureRef texture;
    texture.path = path;
    texture.replaceableId = stock.replaceableId;
    // Every shipped Reforged `TEXS` word wraps both ways; an empty word is
    // clamp, not "no opinion".
    texture.flags = static_cast<u32>(mdx::Texture::Flag::WrapWidth) |
                    static_cast<u32>(mdx::Texture::Flag::WrapHeight);
    document.textures.push_back(std::move(texture));
    if (appended != nullptr) {
        *appended = true;
    }
    return static_cast<u32>(document.textures.size() - 1);
}

MaterialBlockDraft MdxConverter::defaultMaterialBlock(Document& document, ProfileId profile,
                                                      u32 colourMap, u32 sourceVersion) const {
    MaterialBlockDraft draft;
    // From the parser's own defaults — alpha 1, emissive gain 1, a white
    // fresnel colour at no strength — so a default layer is what a file with
    // nothing to say about a field would have read as.
    mdx::Material material;
    mdx::Layer layer;
    layer.filterMode = mdx::Layer::FilterMode::None;
    layer.textureAnimationId = mdx_core::kNoTextureAnimation;
    layer.coordId = 0;
    if (profile == ProfileId::Wc3Reforged) {
        layer.shader = mdx::Layer::ShaderType::HD;
        layer.is_hd = true;
        for (u32 s = 0; s < mdx_core::kHdPositionalSlotCount; ++s) {
            const auto slot = static_cast<mdx::Layer::SlotType>(s);
            mdx::Layer::SubTexture sub;
            sub.slot = slot;
            sub.textureId = colourMap;
            if (slot != mdx::Layer::SlotType::DiffuseMap) {
                bool added = false;
                const u32 stock = internStockTexture(document, slot, &added);
                draft.texturesAppended += added ? 1u : 0u;
                if (stock != kInvalidIndex) {
                    sub.textureId = stock;
                }
            }
            layer.subTextures.push_back(sub);
        }
        // Below v1100 a layer carries no shader id, and this string on the
        // material is the whole signal that its stack is HD.
        if (sourceVersion < 1100) {
            material.shader = "Shader_HD_DefaultUnit";
        }
    } else {
        layer.shader = mdx::Layer::ShaderType::SD;
        // Where a block of this version keeps its colour map: the parser moves
        // every v900+ layer's texture into the first sub-texture and zeroes
        // `textureId`, and the renderer reads `subAt(0)` whenever there is one.
        if (sourceVersion >= 900) {
            mdx::Layer::SubTexture sub;
            sub.textureId = colourMap;
            sub.slot = mdx::Layer::SlotType::DiffuseMap;
            layer.subTextures.push_back(sub);
            layer.textureId = 0;
        } else {
            layer.textureId = colourMap;
        }
    }
    material.layers.push_back(layer);
    CopyToNative(material, draft.block);
    draft.block.sourceVersion = sourceVersion;
    draft.block.layers[0].isHd = profile == ProfileId::Wc3Reforged;
    return draft;
}

} // namespace wem
} // namespace models
} // namespace whiteout
