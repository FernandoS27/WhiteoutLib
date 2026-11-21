// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/inline_models.h>

#include <whiteout/models/wem/anim/pose.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/physics/references.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <optional>
#include <set>
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
    case NodeKind::Wc3FaceFx:
    case NodeKind::Projector: // its own frame is in its record
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

/// Where a clip of the placed model plays in the host: the whole of it as a
/// loop of its own, or, on a span, from `span.played` through `span.clip`'s
/// stretch.
struct Route {
    u32 source = 0;
    u32 target = 0;
    bool stretch = false;
    ShownSpan span;
};

/// One attached model's share of the work: its slots and materials, added once,
/// and where its clips play, which every placement keys.
struct Adopted {
    std::vector<u32> slots;        ///< Its slot -> the host's.
    std::vector<Route> routes;
    u32 frameLoop = kInvalidIndex; ///< Where a placed frame's one key goes.
};

/// Two keys this close are one.
constexpr f32 kSameTime = 1e-4f;

/// @p track's value at @p time, one element's bytes: sampled where it is
/// floats, the key held otherwise.
std::vector<u8> ValueAt(const SubTrack& track, geom::AttrType type, f32 time) {
    const std::size_t size = geom::AttrTypeSize(type);
    std::vector<u8> value(size, 0);
    switch (type) {
    case geom::AttrType::F32:
    case geom::AttrType::F32x2:
    case geom::AttrType::F32x3:
    case geom::AttrType::F32x4:
    case geom::AttrType::Quat:
    case geom::AttrType::Extent: {
        std::vector<f32> floats(size / sizeof(f32));
        SampleTrack(track, type, time, floats.data(), static_cast<u32>(floats.size()));
        std::memcpy(value.data(), floats.data(), size);
        break;
    }
    default: {
        std::size_t k = 0;
        while (k + 1 < track.times.size() && track.times[k + 1] <= time) {
            ++k;
        }
        std::memcpy(value.data(), track.values.data() + k * ValuesPerKey(track.interp) * size, size);
        break;
    }
    }
    return value;
}

/// @p track from @p from for @p length, laid at @p at onto @p into, with a key
/// at its start, and at its end where @p close, where it has none: what it
/// plays there whichever keys fall outside. A wrap's next lap starts where an
/// end key would sit, so it leaves that one out.
void AppendStretch(SubTrack& into, const SubTrack& track, geom::AttrType type, f32 from, f32 length, f32 at,
                   bool close) {
    if (track.times.empty() || !track.wellSized(type)) {
        return;
    }
    const std::size_t size = geom::AttrTypeSize(type);
    const std::size_t stride = ValuesPerKey(track.interp) * size;
    const bool tcb = track.tcb.size() == track.times.size() * 3;
    if (into.times.empty()) {
        into.interp = track.interp;
    }
    const auto push = [&](f32 time, const u8* key, const f32* params) {
        // A lap's last key and the next lap's first meet: the first holds.
        if (!into.times.empty() && time <= into.times.back() + kSameTime) {
            return;
        }
        into.times.push_back(time);
        into.values.insert(into.values.end(), key, key + stride);
        if (tcb) {
            into.tcb.insert(into.tcb.end(), params, params + 3);
        }
    };
    const f32 flat[3] = {0.0f, 0.0f, 0.0f};
    // A key of its own: flat, a Bezier's controls on its value, a Hermite's
    // tangents zero.
    const auto pushSampled = [&](f32 time, f32 source) {
        std::vector<u8> key(stride, 0);
        const std::vector<u8> value = ValueAt(track, type, source);
        std::memcpy(key.data(), value.data(), size);
        if (track.interp == Interpolation::Bezier) {
            std::memcpy(key.data() + size, value.data(), size);
            std::memcpy(key.data() + 2 * size, value.data(), size);
        }
        push(time, key.data(), flat);
    };
    const f32 to = from + length;
    bool first = true;
    f32 last = from;
    for (std::size_t k = 0; k < track.times.size(); ++k) {
        const f32 t = track.times[k];
        if (t < from - kSameTime || t > to + kSameTime) {
            continue;
        }
        if (first && t > from + kSameTime) {
            pushSampled(at, from);
        }
        first = false;
        last = t;
        push(at + std::clamp(t - from, 0.0f, length), track.values.data() + k * stride,
             tcb ? track.tcb.data() + k * 3 : flat);
    }
    if (first) {
        pushSampled(at, from);
        last = from;
    }
    if (close && last < to - kSameTime) {
        pushSampled(at + length, to);
    }
}

/// The host's track for channel @p id in @p clip, made where there is none.
SubTrack& TrackIn(Clip& clip, std::size_t container, u32 id) {
    if (clip.containers.empty()) {
        clip.containers.emplace_back();
    }
    SubTrackContainer& box = clip.containers[std::min(container, clip.containers.size() - 1)];
    for (SubTrack& track : box.subTracks) {
        if (track.channel == id) {
            return track;
        }
    }
    SubTrack& made = box.subTracks.emplace_back();
    made.channel = id;
    return made;
}

/// Every track the routes carry whose channel @p rename names (the host's id
/// and its type), after @p touch, into the host's clips: a loop's as it is, a
/// span's as the stretch of it the span plays, wrapped where its clip loops.
template <class Rename, class Touch>
void CopyTracks(Document& document, const Adopted& adopted, Rename rename, Touch touch) {
    for (const Route& route : adopted.routes) {
        const Clip& source = document.clips[route.source];
        for (std::size_t c = 0; c < source.containers.size(); ++c) {
            for (SubTrack track : document.clips[route.source].containers[c].subTracks) {
                const auto renamed = rename(track.channel);
                if (!renamed) {
                    continue;
                }
                touch(track);
                track.channel = renamed->first;
                if (!route.stretch) {
                    document.clips[route.target].containers[c].subTracks.push_back(std::move(track));
                    continue;
                }
                SubTrack& into = TrackIn(document.clips[route.target], c, renamed->first);
                const f32 duration = document.clips[route.source].duration;
                const bool wraps = document.clips[route.source].looping && duration > 0.0f;
                f32 at = route.span.start;
                f32 from = wraps ? std::fmod(route.span.played, duration) : route.span.played;
                while (at < route.span.end - kSameTime) {
                    const f32 length = wraps ? std::min(route.span.end - at, duration - from)
                                             : route.span.end - at;
                    const bool more = at + length < route.span.end - kSameTime;
                    AppendStretch(into, track, renamed->second, from, length, at, !more);
                    at += length;
                    from = 0.0f;
                }
            }
        }
    }
}

Adopted Adopt(Document& document, u32 into, u32 from, u32& nextChannel, InlineReport& report,
              const std::vector<ShownSpan>* spans) {
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

    // What it plays in place: `Stand`, else its first clip -- on spans, its
    // first, as StarCraft II starts a model particle's; and its global loops.
    u32 played = kInvalidIndex;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model != from || IsGlobalLoop(clip)) {
            continue;
        }
        played = played == kInvalidIndex ? c : played;
        if (spans != nullptr || clip.native.value("animationId", -1) == 0) {
            played = c;
            break;
        }
    }
    if (spans != nullptr && played != kInvalidIndex) {
        for (const ShownSpan& span : *spans) {
            adopted.routes.push_back(Route{played, span.clip, true, span});
        }
    }
    const u32 clips = static_cast<u32>(document.clips.size());
    for (u32 c = 0; c < clips; ++c) {
        if (document.clips[c].model != from || (c != played && !IsGlobalLoop(document.clips[c])) ||
            (c == played && spans != nullptr)) {
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
        adopted.routes.push_back(Route{c, static_cast<u32>(document.clips.size()), false, ShownSpan{}});
        if (c == played || adopted.frameLoop == kInvalidIndex) {
            adopted.frameLoop = static_cast<u32>(document.clips.size());
        }
        document.clips.push_back(std::move(loop));
        ++report.loops;
    }

    // Its material channels drive the materials it shares between placements,
    // so they come in once.
    std::unordered_map<u32, std::pair<u32, geom::AttrType>> ids;
    for (AnimChannel channel : model.animChannels.channels) {
        if (!IsMaterialTarget(channel.target.kind) || channel.target.material.slot >= adopted.slots.size()) {
            continue;
        }
        channel.target.material.slot = adopted.slots[channel.target.material.slot];
        channel.target.material.look = 0;
        ids[channel.id] = {nextChannel, channel.valueType};
        channel.id = nextChannel++;
        host.animChannels.add(channel);
    }
    CopyTracks(
        document, adopted,
        [&](u32 channel) -> std::optional<std::pair<u32, geom::AttrType>> {
            const auto it = ids.find(channel);
            return it != ids.end() ? std::optional(it->second) : std::nullopt;
        },
        [](SubTrack&) {});
    return adopted;
}

/// The roots hang from @p hang, and a section with no gate of its own takes
/// @p gate, where it is a node.
void Place(Document& document, u32 into, u32 attachment, u32 from, Adopted& adopted, u32& nextChannel,
           u32 hang, u32 gate, Diagnostics& diagnostics) {
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
    const u32 parent = hang;
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
        // Its model's soft bodies stay behind with the rest of its physics.
        DropSoftLayers(mesh);
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
            const i64 own = section.native.value(kSectionVisibilityNode, kSectionAlwaysDrawn);
            if (gate != kInvalidNode && (own < 0 || own == kSectionAlwaysDrawn)) {
                section.native.set(kSectionVisibilityNode, static_cast<i64>(gate));
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
    CopyTracks(
        document, adopted,
        [&](u32 channel) -> std::optional<std::pair<u32, geom::AttrType>> {
            const auto it = ids.find(channel);
            return it != ids.end() ? std::optional(std::pair{it->second.id, it->second.type}) : std::nullopt;
        },
        [&](SubTrack& track) {
            const Placed& placed = ids.at(track.channel);
            if (placed.node) {
                PlaceValues(placed.what, placed.type, placed.frame, placement, track.values);
            }
            if (placed.spread != 1.0f) {
                ScaleFloats(track.values, placed.spread);
            }
        });

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

/// Two explicit-bind trees can hold each other's poses only by the same schema.
bool SameSchema(const NodeTree& a, const NodeTree& b) {
    if (a.poseSchema.size() != b.poseSchema.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.poseSchema.size(); ++i) {
        const PoseSchema& x = a.poseSchema[i];
        const PoseSchema& y = b.poseSchema[i];
        if (x.name != y.name || x.space != y.space || x.inverse != y.inverse || x.storage != y.storage) {
            return false;
        }
    }
    return true;
}

/// An explicit bind has rest rotations to parent under, so the model goes
/// under the attachment itself and follows it, keys untouched. What moves is
/// what is stated in model space — the points and every model-space pose — by
/// the attachment's world bind W, the shipped binds composed rather than
/// rederived: `p W` skinned by `W^-1 B^-1` and `world * A(t)` is `p B^-1 world
/// A(t)`, the model riding the attachment, whatever W the bind took.
void PlaceExplicit(Document& document, u32 into, u32 attachment, u32 from, Adopted& adopted, u32& nextChannel,
                   u32 hang, u32 gate, Diagnostics& diagnostics) {
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

    const std::string prefix = host.nodes.nodes[attachment].name;
    const Matrix44f placed = ToMatrix(host.nodes.worldBind(attachment));
    const Matrix44f unplaced = Matrix44f::inverse(placed);
    const Quaternion turn = FromMatrix(placed).rotation.normalized();
    const bool schema = SameSchema(host.nodes, nodes);

    for (u32 k = 0; k < nodes.size(); ++k) {
        Node node = std::move(nodes.nodes[k]);
        node.parent = node.parent >= nodes.size() ? hang : base + node.parent;
        node.name = Behind(prefix, node.name);
        if (!schema) {
            // Rebuilt from the rest chain under the attachment by `conformPoses`.
            node.poses.clear();
            node.poseMatrices.clear();
        }
        for (std::size_t i = 0; schema && i < host.nodes.poseSchema.size() && i < node.poses.size(); ++i) {
            const PoseSchema& entry = host.nodes.poseSchema[i];
            if (entry.space != PoseSpace::Model) {
                continue; // parent-relative: the attachment is the root's parent now
            }
            const bool stored = entry.storage == PoseStorage::Matrix && i < node.poseMatrices.size();
            const Matrix44f value = stored ? node.poseMatrices[i] : ToMatrix(node.poses[i]);
            const Matrix44f moved = entry.inverse ? unplaced * value : value * placed;
            if (stored) {
                node.poseMatrices[i] = moved;
            }
            node.poses[i] = FromMatrix(moved);
        }
        if (node.kind == NodeKind::Camera) {
            node.kind = NodeKind::Helper;
            node.resetPayloadForKind();
        } else if (auto* attached = std::get_if<AttachmentPayload>(&node.payload)) {
            attached->model = kInvalidIndex;
        } else if (auto* ribbon = std::get_if<Wc3RibbonEmitterPayload>(&node.payload)) {
            ribbon->materialSlot =
                ribbon->materialSlot < adopted.slots.size() ? adopted.slots[ribbon->materialSlot] : 0u;
        }
        host.nodes.add(std::move(node));
    }
    if (!schema) {
        host.nodes.conformPoses();
    }

    for (Mesh& mesh : meshes) {
        mesh.name = Behind(prefix, mesh.name);
        DropSoftLayers(mesh);
        for (Vector3f& p : mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex)) {
            p = whiteout::transform_point(p, placed);
        }
        for (const geom::Domain domain : {geom::Domain::Vertex, geom::Domain::Halfedge}) {
            for (Vector3f& n : mesh.attributes.get<Vector3f>(geom::names::kNormal, domain)) {
                n = turn.rotate_vector(n);
            }
            for (std::array<f32, 4>& t : mesh.attributes.get<std::array<f32, 4>>(geom::names::kTangent, domain)) {
                const Vector3f axis = turn.rotate_vector(Vector3f{t[0], t[1], t[2]});
                t = {axis.x, axis.y, axis.z, t[3]};
            }
        }
        for (MeshSection& section : mesh.sections) {
            section.materialSlot = section.materialSlot < adopted.slots.size() ? adopted.slots[section.materialSlot] : 0u;
            if (!section.name.empty()) {
                section.name = Behind(prefix, section.name);
            }
            const i64 own = section.native.value(kSectionVisibilityNode, kSectionAlwaysDrawn);
            if (gate != kInvalidNode && (own < 0 || own == kSectionAlwaysDrawn)) {
                section.native.set(kSectionVisibilityNode, static_cast<i64>(gate));
            }
        }
        mesh.recomputeBounds();
        host.meshes.push_back(std::move(mesh));
    }

    // Its node and section channels as they are: the keys are its parent's
    // frame, which the attachment now is. The material ones came in once.
    std::unordered_map<u32, std::pair<u32, geom::AttrType>> ids;
    for (AnimChannel channel : channels.channels) {
        if (IsMaterialTarget(channel.target.kind) || IsPhysicsChannel(channel.target) ||
            IsStageChannel(channel.target)) {
            continue;
        }
        if (channel.target.kind == TrackTarget::Kind::Section) {
            channel.target.mesh += meshBase;
        } else if (channel.target.kind == TrackTarget::Kind::Node &&
                   (channel.target.node < base || channel.target.node - base >= nodes.size())) {
            continue;
        }
        ids[channel.id] = {nextChannel, channel.valueType};
        channel.id = nextChannel++;
        host.animChannels.add(channel);
    }
    CopyTracks(
        document, adopted,
        [&](u32 channel) -> std::optional<std::pair<u32, geom::AttrType>> {
            const auto it = ids.find(channel);
            return it != ids.end() ? std::optional(it->second) : std::nullopt;
        },
        [](SubTrack&) {});

    for (u32 m = meshBase; m < host.meshes.size(); ++m) {
        const Extent& bounds = host.meshes[m].bounds;
        if (bounds.valid()) {
            GrowExtent(host.bounds, bounds.minimum);
            GrowExtent(host.bounds, bounds.maximum);
        }
    }
    FinishExtent(host.bounds);
}

/// The helper a placement on spans hangs from, under @p attachment at its
/// rest: shown through each span and hidden otherwise, rest included.
u32 AddSpanGate(Document& document, u32 into, u32 attachment, const std::vector<ShownSpan>& spans,
                u32& nextChannel) {
    Model& host = document.models[into];
    Node gate;
    gate.kind = NodeKind::Helper;
    gate.resetPayloadForKind();
    gate.name = host.nodes.nodes[attachment].name + "_Model";
    if (gate.name.size() > kNameBytes) {
        gate.name.resize(kNameBytes);
    }
    gate.parent = attachment;
    gate.pivot = host.nodes.nodes[attachment].pivot;
    gate.local = Transform::identity();
    const u32 index = host.nodes.size();
    host.nodes.add(std::move(gate));
    if (host.nodes.rig != RigConvention::PivotRelative) {
        host.nodes.conformPoses();
    }

    AnimChannel channel;
    channel.id = nextChannel++;
    channel.target.kind = TrackTarget::Kind::Node;
    channel.target.node = index;
    channel.target.channel = Channel::Visibility;
    channel.valueType = geom::AttrType::F32;
    channel.initValue.resize(sizeof(f32));
    const f32 hidden = 0.0f;
    std::memcpy(channel.initValue.data(), &hidden, sizeof(f32));
    host.animChannels.add(channel);

    std::map<u32, SubTrack> tracks;
    const auto key = [](SubTrack& track, f32 time, f32 value) {
        track.times.push_back(time);
        const u8* bytes = reinterpret_cast<const u8*>(&value);
        track.values.insert(track.values.end(), bytes, bytes + sizeof(f32));
    };
    for (const ShownSpan& span : spans) {
        SubTrack& track = tracks[span.clip];
        track.channel = channel.id;
        track.interp = Interpolation::Step;
        if (track.times.empty() && span.start > kSameTime) {
            key(track, 0.0f, 0.0f);
        }
        key(track, span.start, 1.0f);
        if (span.end < document.clips[span.clip].duration - kSameTime) {
            key(track, span.end, 0.0f);
        }
    }
    // Every other clip says hidden: a Warcraft III export fills a clip that
    // keys nothing from the timeline around it, which is a span.
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model == into && !IsGlobalLoop(clip) && tracks.find(c) == tracks.end()) {
            SubTrack& track = tracks[c];
            track.channel = channel.id;
            track.interp = Interpolation::Step;
            key(track, 0.0f, 0.0f);
        }
    }
    for (auto& [clip, track] : tracks) {
        TrackIn(document.clips[clip], 0, channel.id) = std::move(track);
    }
    return index;
}

} // namespace

std::vector<ShownSpan> ShownSpans(const Document& document, u32 model, u32 node) {
    std::vector<ShownSpan> out;
    if (model >= document.models.size()) {
        return out;
    }
    const Model& owner = document.models[model];
    const AnimChannel* channel = nullptr;
    for (const AnimChannel& entry : owner.animChannels.channels) {
        if (entry.target.kind == TrackTarget::Kind::Node && entry.target.node == node &&
            entry.target.channel == Channel::Visibility && entry.valueType == geom::AttrType::F32) {
            channel = &entry;
        }
    }
    f32 rest = 1.0f;
    if (channel != nullptr && channel->hasInitValue() && channel->initValue.size() >= sizeof(f32)) {
        std::memcpy(&rest, channel->initValue.data(), sizeof(f32));
    }

    constexpr f32 kShortestSpan = 1e-3f;
    std::vector<std::vector<ShownSpan>> spans(document.clips.size());
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model != model || clip.duration <= 0.0f || IsGlobalLoop(clip)) {
            continue;
        }
        const SubTrack* track = channel != nullptr ? FindSubTrack(clip, channel->id) : nullptr;
        // The clip's breakpoints: its ends and the keys it plays.
        std::vector<f32> times{0.0f, clip.duration};
        if (track != nullptr) {
            for (const f32 t : track->times) {
                if (t > 0.0f && t < clip.duration) {
                    times.push_back(t);
                }
            }
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        const auto shown = [&](f32 time) {
            f32 v = rest;
            if (track != nullptr && !track->times.empty()) {
                SampleTrack(*track, geom::AttrType::F32, time, &v, 1);
            }
            return v > 0.0f;
        };
        f32 start = -1.0f;
        const auto close = [&](f32 end) {
            if (end - start >= kShortestSpan) {
                spans[c].push_back(ShownSpan{c, start, end, 0.0f, end - start});
            }
            start = -1.0f;
        };
        for (std::size_t k = 0; k + 1 < times.size(); ++k) {
            // A segment shows where its middle does: a step holds its left
            // key, and a line above 0 at either end is above 0 inside.
            const bool on = shown(0.5f * (times[k] + times[k + 1]));
            if (on && start < 0.0f) {
                start = times[k];
            }
            if (!on && start >= 0.0f) {
                close(times[k]);
            }
        }
        if (start >= 0.0f) {
            close(clip.duration);
        }
    }

    // Death -> Decay, by the names both games give them, a number after them
    // aside ("Death 1" and "Decay", "Death 01" and "Decay 01"): what shows to
    // the Death's end plays on into the Decay that starts shown.
    const auto after = [](const std::string& name, std::string_view word) -> std::optional<std::string> {
        if (name.rfind(word, 0) != 0) {
            return std::nullopt;
        }
        std::string rest = name.substr(word.size());
        while (!rest.empty() && (rest.back() == ' ' || (rest.back() >= '0' && rest.back() <= '9'))) {
            rest.pop_back();
        }
        return rest;
    };
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const std::optional<std::string> death = after(document.clips[c].name, "Death");
        if (spans[c].empty() || !death || spans[c].back().end < document.clips[c].duration - kShortestSpan) {
            continue;
        }
        for (u32 d = 0; d < document.clips.size(); ++d) {
            if (d != c && after(document.clips[d].name, "Decay") == death && !spans[d].empty() &&
                spans[d].front().start <= 0.0f && spans[d].front().played == 0.0f) {
                ShownSpan& born = spans[c].back();
                spans[d].front().played = born.end - born.start;
                born.life += spans[d].front().life;
                spans[d].front().life = 0.0f;
                break;
            }
        }
    }
    for (std::vector<ShownSpan>& clip : spans) {
        out.insert(out.end(), clip.begin(), clip.end());
    }
    return out;
}

InlineReport InlineAttachedModels(Document& document, u32 into, const InlineOptions& options) {
    InlineReport report;
    if (into >= document.models.size()) {
        return report;
    }
    const bool onSpans = options.clock == InlineClock::ShownSpans;
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
    const RigConvention rig = tree.rig;

    u32 nextChannel = document.models[into].animChannels.nextFreeId();
    std::map<u32, Adopted> adopted;
    std::set<u32> models;
    u32 skipped = 0;
    u32 unplayed = 0;
    u32 unshown = 0;
    for (const auto& [attachment, from] : placements) {
        const Model& model = document.models[from];
        if (model.nodes.rig != rig) {
            ++skipped;
            continue;
        }
        AttachmentPayload& payload =
            std::get<AttachmentPayload>(document.models[into].nodes.nodes[attachment].payload);
        u32 hang = document.models[into].nodes.nodes[attachment].parent;
        u32 gate = kInvalidNode;
        auto it = adopted.find(from);
        if (onSpans) {
            // Its own materials and keys each: the spans are the placement's.
            const std::vector<ShownSpan> spans = ShownSpans(document, into, attachment);
            payload.model = kInvalidIndex;
            payload.asset.path.clear();
            if (spans.empty()) {
                ++unshown;
                continue;
            }
            it = adopted.emplace(attachment, Adopt(document, into, from, nextChannel, report, &spans)).first;
            hang = gate = AddSpanGate(document, into, attachment, spans, nextChannel);
        } else if (it == adopted.end()) {
            it = adopted.emplace(from, Adopt(document, into, from, nextChannel, report, nullptr)).first;
        }
        if (models.insert(from).second) {
            ++report.models;
            unplayed += model.physics.empty() && model.poseStages.empty() ? 0u : 1u;
        }
        if (rig == RigConvention::PivotRelative) {
            Place(document, into, attachment, from, it->second, nextChannel, hang, gate, report.diagnostics);
        } else {
            PlaceExplicit(document, into, attachment, from, it->second, nextChannel,
                          onSpans ? hang : attachment, gate, report.diagnostics);
        }
        std::get<AttachmentPayload>(document.models[into].nodes.nodes[attachment].payload).model = kInvalidIndex;
        ++report.placements;
    }

    if (report.placements != 0) {
        report.diagnostics.info(DiagCode::LossyKindConversion,
                                std::to_string(report.placements) + " placement(s) of " +
                                    std::to_string(report.models) +
                                    " attached model(s) written into the model where they sit, " +
                                    (onSpans ? std::string("playing where their attachment shows")
                                             : "playing on " + std::to_string(report.loops) + " global loop(s)"),
                                ElementRef(ElementKind::Document, 0));
    }
    if (unshown != 0) {
        report.diagnostics.info(DiagCode::FeatureDropped,
                                std::to_string(unshown) +
                                    " attached model(s) left out: their attachment shows in no clip",
                                ElementRef(ElementKind::Document, 0));
    }
    if (skipped != 0) {
        report.diagnostics.warn(DiagCode::OperationUnsupported,
                                std::to_string(skipped) +
                                    " placement(s) stay out: their model's rig is not the host's kind",
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
