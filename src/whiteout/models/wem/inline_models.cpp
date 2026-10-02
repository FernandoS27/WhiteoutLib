// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/inline_models.h>

#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/nodes/remove.h>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// An `.mdx` name is 80 bytes with its terminator.
constexpr std::size_t kNameBytes = 79;

std::string Behind(const std::string& prefix, const std::string& name) {
    std::string out = prefix + " " + name;
    if (out.size() > kNameBytes) {
        out.resize(kNameBytes);
    }
    return out;
}

/// A leaf whose own axes say where something goes: an emitter's direction and
/// spawn area, a box's sides.
bool FrameMatters(NodeKind kind) {
    switch (kind) {
    case NodeKind::Helper:
    case NodeKind::Bone:
    case NodeKind::Attachment:
    case NodeKind::Light:
    case NodeKind::Camera:
    case NodeKind::Event:
        return false;
    default:
        return true;
    }
}

/// Row vectors, scale first: `ToMatrix`'s order.
struct Placement {
    Quaternion rotation{0, 0, 0, 1};
    f32 scale = 1.0f;
    Vector3f translation{0, 0, 0};

    Vector3f vector(const Vector3f& v) const {
        return rotation.rotate_vector(v * scale);
    }
    Vector3f point(const Vector3f& p) const {
        return vector(p) + translation;
    }
    /// A node turning by @p q inside the placed model turns by this outside it.
    Quaternion conjugated(const Quaternion& q) const {
        return rotation * q * rotation.conjugate();
    }
};

/// What a placement does to a key of @p channel: a translation is a vector, a
/// rotation conjugates, or turns the frame too where @p frame. Both are linear,
/// so a tangent goes the same way as its value.
void PlaceValues(Channel channel, geom::AttrType type, bool frame, const Placement& placement,
                 std::vector<u8>& bytes) {
    if (channel == Channel::Translation && type == geom::AttrType::F32x3) {
        std::vector<Vector3f> values(bytes.size() / sizeof(Vector3f));
        std::memcpy(values.data(), bytes.data(), values.size() * sizeof(Vector3f));
        for (Vector3f& v : values) {
            v = placement.vector(v);
        }
        std::memcpy(bytes.data(), values.data(), values.size() * sizeof(Vector3f));
    } else if (channel == Channel::Rotation && (type == geom::AttrType::Quat || type == geom::AttrType::F32x4)) {
        std::vector<Quaternion> values(bytes.size() / sizeof(Quaternion));
        std::memcpy(values.data(), bytes.data(), values.size() * sizeof(Quaternion));
        for (Quaternion& q : values) {
            q = frame ? placement.rotation * q : placement.conjugated(q);
        }
        std::memcpy(bytes.data(), values.data(), values.size() * sizeof(Quaternion));
    }
}

/// 12.1 spreads and speeds a doodad's particles by its placement's scale and
/// leaves their size; the bake left the scale off the emitter's frame, so these
/// fields take it.
bool Spreads(NodeKind kind, const char* field) {
    const std::string_view name = field;
    return (kind == NodeKind::Wc3ParticleEmitter2 && (name == "speed" || name == "width" || name == "length")) ||
           (kind == NodeKind::Wc3ParticleEmitter1 && name == "speed");
}

void ScaleFloats(std::vector<u8>& bytes, f32 factor) {
    std::vector<f32> values(bytes.size() / sizeof(f32));
    std::memcpy(values.data(), bytes.data(), values.size() * sizeof(f32));
    for (f32& v : values) {
        v *= factor;
    }
    std::memcpy(bytes.data(), values.data(), values.size() * sizeof(f32));
}

/// One attached model's share of the work: its slots and materials, added once,
/// and the global loops its clips became, which every placement keys.
struct Adopted {
    std::vector<u32> slots;                   ///< Its slot -> the host's.
    std::vector<std::pair<u32, u32>> loops;   ///< (its clip, the host's loop).
    u32 frameLoop = kInvalidIndex;            ///< Where a placed frame's one key goes.
};

Adopted Adopt(Document& document, u32 into, u32 from, u32& nextChannel, InlineReport& report) {
    Model& host = document.models[into];
    const Model& model = document.models[from];
    Adopted adopted;

    // Its slots, under names of their own: a slot name is the join key.
    const std::string owner = model.name.empty() ? "model " + std::to_string(from) : model.name;
    for (const std::string& slot : model.materialSlots) {
        std::string name = owner + "|" + slot;
        while (host.slotIndex(name) != kInvalidIndex) {
            name += "'";
        }
        adopted.slots.push_back(host.addSlot(name));
    }
    // Its materials, in every set the host has, as its own default look draws them.
    for (ProfileMaterialSet& set : host.profileSets) {
        set.resizeBindings(host.materialSlots.size());
        const ProfileMaterialSet* own = model.setFor(set.profile);
        if (own == nullptr) {
            continue;
        }
        const u32 base = static_cast<u32>(set.materials.size());
        set.materials.insert(set.materials.end(), own->materials.begin(), own->materials.end());
        for (u32 s = 0; s < adopted.slots.size() && s < own->slotBindings.size(); ++s) {
            if (!own->slotBindings[s].bound(own->defaultLook)) {
                continue;
            }
            for (u32& material : set.slotBindings[adopted.slots[s]].byLook) {
                material = base + own->slotBindings[s].byLook[own->defaultLook];
            }
        }
    }

    // What it plays in place: `Stand`, else its first clip; and its global loops.
    u32 played = kInvalidIndex;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model != from || IsGlobalLoop(clip)) {
            continue;
        }
        played = played == kInvalidIndex ? c : played;
        if (clip.native.value("animationId", -1) == 0) {
            played = c;
            break;
        }
    }
    const u32 clips = static_cast<u32>(document.clips.size());
    for (u32 c = 0; c < clips; ++c) {
        if (document.clips[c].model != from || (c != played && !IsGlobalLoop(document.clips[c]))) {
            continue;
        }
        Clip loop = document.clips[c];
        loop.model = into;
        loop.flags |= ClipFlags::AutoPlay | ClipFlags::WorldClocked;
        loop.looping = true;
        loop.events.clear();
        loop.trackSets.clear();
        loop.physics.reset();
        for (SubTrackContainer& container : loop.containers) {
            container.subTracks.clear();
        }
        adopted.loops.emplace_back(c, static_cast<u32>(document.clips.size()));
        if (c == played || adopted.frameLoop == kInvalidIndex) {
            adopted.frameLoop = static_cast<u32>(document.clips.size());
        }
        document.clips.push_back(std::move(loop));
        ++report.loops;
    }

    // Its material channels drive the materials it shares between placements,
    // so they come in once.
    std::unordered_map<u32, u32> ids;
    for (AnimChannel channel : model.animChannels.channels) {
        if (!IsMaterialTarget(channel.target.kind) || channel.target.material.slot >= adopted.slots.size()) {
            continue;
        }
        channel.target.material.slot = adopted.slots[channel.target.material.slot];
        channel.target.material.look = 0;
        ids[channel.id] = nextChannel;
        channel.id = nextChannel++;
        host.animChannels.add(channel);
    }
    for (const auto& [source, loop] : adopted.loops) {
        for (std::size_t k = 0; k < document.clips[source].containers.size(); ++k) {
            for (SubTrack track : document.clips[source].containers[k].subTracks) {
                if (const auto it = ids.find(track.channel); it != ids.end()) {
                    track.channel = it->second;
                    document.clips[loop].containers[k].subTracks.push_back(std::move(track));
                }
            }
        }
    }
    return adopted;
}

void Place(Document& document, u32 into, u32 attachment, u32 from, Adopted& adopted, u32& nextChannel,
           Diagnostics& diagnostics) {
    const Model& model = document.models[from];
    NodeTree nodes = model.nodes;
    std::vector<Mesh> meshes = model.meshes;
    AnimChannelTable channels = model.animChannels;

    Model& host = document.models[into];
    const u32 base = host.nodes.size();
    const u32 meshBase = static_cast<u32>(host.meshes.size());
    std::vector<u32> remap(nodes.size());
    std::iota(remap.begin(), remap.end(), base);
    NodeReferencers referencers;
    referencers.meshes = meshes;
    referencers.channels = &channels;
    RemapNodeReferencers(nodes, remap, referencers, diagnostics);

    const Node& at = host.nodes.nodes[attachment];
    const std::string prefix = at.name;
    const u32 parent = at.parent;
    const Vector3f parentPivot = parent < base ? host.nodes.nodes[parent].pivot : Vector3f{0, 0, 0};
    const Transform world = host.nodes.worldBind(attachment);
    Placement placement;
    placement.rotation = world.rotation.normalized();
    placement.scale = world.scale.x;
    placement.translation = world.translation;
    const bool turned = std::abs(placement.rotation.w) < 1.0f - 1e-6f;

    // A frame turns with the placement only on a leaf: a child would turn twice.
    std::vector<bool> leaf(nodes.size(), true);
    for (const Node& node : nodes.nodes) {
        if (node.parent < nodes.size()) {
            leaf[node.parent] = false;
        }
    }
    std::vector<bool> frame(nodes.size(), false);
    std::vector<bool> demoted(nodes.size(), false);
    for (u32 k = 0; k < nodes.size(); ++k) {
        Node node = std::move(nodes.nodes[k]);
        const bool root = node.parent >= nodes.size();
        frame[k] = turned && leaf[k] && FrameMatters(node.kind);
        node.parent = root ? parent : base + node.parent;
        node.name = Behind(prefix, node.name);
        node.pivot = placement.point(node.pivot);
        const auto rest = [&](Transform& t) {
            t.translation = root ? placement.point(t.translation) - parentPivot : placement.vector(t.translation);
            t.rotation = frame[k] ? placement.rotation * t.rotation : placement.conjugated(t.rotation);
        };
        rest(node.local);
        for (Transform& pose : node.poses) {
            rest(pose);
        }
        if (node.kind == NodeKind::Camera) {
            // A doodad's portrait camera is not the model's.
            node.kind = NodeKind::Helper;
            node.resetPayloadForKind();
            demoted[k] = true;
        } else if (auto* attached = std::get_if<AttachmentPayload>(&node.payload)) {
            // Its id would break the climb `toMdx` gives the host's.
            attached->model = kInvalidIndex;
            std::erase_if(node.native.entries,
                          [](const NativeBag::Entry& entry) { return entry.name == "mdxAttachmentId"; });
        } else if (auto* ribbon = std::get_if<Wc3RibbonEmitterPayload>(&node.payload)) {
            ribbon->materialSlot =
                ribbon->materialSlot < adopted.slots.size() ? adopted.slots[ribbon->materialSlot] : 0u;
        } else if (auto* pe2 = std::get_if<Wc3ParticleEmitter2Payload>(&node.payload)) {
            pe2->speed *= placement.scale;
            pe2->width *= placement.scale;
            pe2->length *= placement.scale;
        } else if (auto* pe1 = std::get_if<Wc3ParticleEmitter1Payload>(&node.payload)) {
            pe1->speed *= placement.scale;
        }
        host.nodes.add(std::move(node));
    }

    for (Mesh& mesh : meshes) {
        mesh.name = Behind(prefix, mesh.name);
        for (Vector3f& p : mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex)) {
            p = placement.point(p);
        }
        for (const geom::Domain domain : {geom::Domain::Vertex, geom::Domain::Halfedge}) {
            for (Vector3f& n : mesh.attributes.get<Vector3f>(geom::names::kNormal, domain)) {
                n = placement.rotation.rotate_vector(n);
            }
            for (std::array<f32, 4>& t : mesh.attributes.get<std::array<f32, 4>>(geom::names::kTangent, domain)) {
                const Vector3f axis = placement.rotation.rotate_vector(Vector3f{t[0], t[1], t[2]});
                t = {axis.x, axis.y, axis.z, t[3]};
            }
        }
        for (MeshSection& section : mesh.sections) {
            section.materialSlot = section.materialSlot < adopted.slots.size() ? adopted.slots[section.materialSlot] : 0u;
            if (!section.name.empty()) {
                section.name = Behind(prefix, section.name);
            }
        }
        mesh.recomputeBounds();
        host.meshes.push_back(std::move(mesh));
    }

    // Its node and section channels, placed; the material ones came in once.
    struct Placed {
        u32 id = 0;
        Channel what = Channel::Translation;
        geom::AttrType type = geom::AttrType::F32x3;
        bool node = false;
        bool frame = false;
        f32 spread = 1.0f;
    };
    std::unordered_map<u32, Placed> ids;
    std::vector<bool> keyedRotation(nodes.size(), false);
    for (AnimChannel channel : channels.channels) {
        if (IsMaterialTarget(channel.target.kind) || IsPhysicsChannel(channel.target) ||
            IsStageChannel(channel.target)) {
            continue;
        }
        Placed placed{nextChannel, channel.target.channel, channel.valueType, false, false, 1.0f};
        if (channel.target.kind == TrackTarget::Kind::Section) {
            channel.target.mesh += meshBase;
        } else if (channel.target.kind == TrackTarget::Kind::Node) {
            const u32 k = channel.target.node - base;
            if (channel.target.node < base || k >= nodes.size()) {
                continue;
            }
            if (demoted[k] && placed.what != Channel::Translation && placed.what != Channel::Rotation &&
                placed.what != Channel::Scale) {
                continue;
            }
            placed.node = true;
            placed.frame = frame[k];
            keyedRotation[k] = keyedRotation[k] || placed.what == Channel::Rotation;
            const EmitterPropertyDesc* property =
                placed.what == Channel::EmitterProperty
                    ? FindEmitterProperty(host.nodes.nodes[channel.target.node].kind, channel.target.sub)
                    : nullptr;
            if (property != nullptr && Spreads(host.nodes.nodes[channel.target.node].kind, property->name)) {
                placed.spread = placement.scale;
                ScaleFloats(channel.initValue, placed.spread);
            }
            PlaceValues(placed.what, placed.type, placed.frame, placement, channel.initValue);
        }
        ids[channel.id] = placed;
        channel.id = nextChannel++;
        host.animChannels.add(channel);
    }
    for (const auto& [source, loop] : adopted.loops) {
        for (std::size_t c = 0; c < document.clips[source].containers.size(); ++c) {
            for (SubTrack track : document.clips[source].containers[c].subTracks) {
                const auto it = ids.find(track.channel);
                if (it == ids.end()) {
                    continue;
                }
                if (it->second.node) {
                    PlaceValues(it->second.what, it->second.type, it->second.frame, placement, track.values);
                }
                if (it->second.spread != 1.0f) {
                    ScaleFloats(track.values, it->second.spread);
                }
                track.channel = it->second.id;
                document.clips[loop].containers[c].subTracks.push_back(std::move(track));
            }
        }
    }

    // A turned frame no key turns says so with a key of its own.
    for (u32 k = 0; k < nodes.size(); ++k) {
        if (!frame[k] || keyedRotation[k]) {
            continue;
        }
        if (adopted.frameLoop == kInvalidIndex) {
            Clip loop;
            loop.name = "frames";
            loop.model = into;
            loop.duration = 1.0f;
            loop.looping = true;
            loop.flags = ClipFlags::AutoPlay | ClipFlags::WorldClocked;
            loop.containers.emplace_back();
            adopted.frameLoop = static_cast<u32>(document.clips.size());
            document.clips.push_back(std::move(loop));
        }
        AnimChannel channel;
        channel.id = nextChannel++;
        channel.target.kind = TrackTarget::Kind::Node;
        channel.target.node = base + k;
        channel.target.channel = Channel::Rotation;
        channel.valueType = geom::AttrType::Quat;
        channel.initValue.resize(sizeof(Quaternion));
        std::memcpy(channel.initValue.data(), &placement.rotation, sizeof(Quaternion));
        host.animChannels.add(channel);
        SubTrack track;
        track.channel = channel.id;
        track.interp = Interpolation::Slerp;
        track.times = {0.0f};
        track.values = channel.initValue;
        document.clips[adopted.frameLoop].containers.front().subTracks.push_back(std::move(track));
    }

    for (u32 m = meshBase; m < host.meshes.size(); ++m) {
        const Extent& bounds = host.meshes[m].bounds;
        if (bounds.valid()) {
            GrowExtent(host.bounds, bounds.minimum);
            GrowExtent(host.bounds, bounds.maximum);
        }
    }
    FinishExtent(host.bounds);
}

} // namespace

InlineReport InlineAttachedModels(Document& document, u32 into) {
    InlineReport report;
    if (into >= document.models.size()) {
        return report;
    }
    // Gathered first: a placement grows the host's nodes.
    std::vector<std::pair<u32, u32>> placements;
    const NodeTree& tree = document.models[into].nodes;
    for (u32 n = 0; n < tree.size(); ++n) {
        const auto* attachment = std::get_if<AttachmentPayload>(&tree.nodes[n].payload);
        if (attachment != nullptr && !tree.nodes[n].removed && attachment->model < document.models.size() &&
            attachment->model != into) {
            placements.emplace_back(n, attachment->model);
        }
    }
    if (placements.empty()) {
        return report;
    }
    if (tree.rig != RigConvention::PivotRelative) {
        report.diagnostics.warn(DiagCode::OperationUnsupported,
                                "the attached models stay out: the model is not a pivot rig",
                                ElementRef(ElementKind::Document, 0));
        return report;
    }

    u32 nextChannel = document.models[into].animChannels.nextFreeId();
    std::map<u32, Adopted> adopted;
    u32 skipped = 0;
    u32 unplayed = 0;
    for (const auto& [attachment, from] : placements) {
        const Model& model = document.models[from];
        if (model.nodes.rig != RigConvention::PivotRelative) {
            ++skipped;
            continue;
        }
        auto it = adopted.find(from);
        if (it == adopted.end()) {
            it = adopted.emplace(from, Adopt(document, into, from, nextChannel, report)).first;
            ++report.models;
            unplayed += model.physics.empty() && model.poseStages.empty() ? 0u : 1u;
        }
        Place(document, into, attachment, from, it->second, nextChannel, report.diagnostics);
        std::get<AttachmentPayload>(document.models[into].nodes.nodes[attachment].payload).model = kInvalidIndex;
        ++report.placements;
    }

    if (report.placements != 0) {
        report.diagnostics.info(DiagCode::LossyKindConversion,
                                std::to_string(report.placements) + " placement(s) of " +
                                    std::to_string(report.models) +
                                    " attached model(s) written into the model where they sit, playing on " +
                                    std::to_string(report.loops) + " global loop(s)",
                                ElementRef(ElementKind::Document, 0));
    }
    if (skipped != 0) {
        report.diagnostics.warn(DiagCode::OperationUnsupported,
                                std::to_string(skipped) + " placement(s) stay out: their model is not a pivot rig",
                                ElementRef(ElementKind::Document, 0));
    }
    if (unplayed != 0) {
        report.diagnostics.info(DiagCode::FeatureDropped,
                                std::to_string(unplayed) +
                                    " attached model(s) leave their physics and pose stages behind",
                                ElementRef(ElementKind::Document, 0));
    }
    return report;
}

} // namespace wem
} // namespace models
} // namespace whiteout
