// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/scene_ops.h>

#include <whiteout/models/wem/anim/detach.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/materials/ops.h>
#include <whiteout/models/wem/nodes/remove.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    std::string out = name.empty() ? prefix : prefix + " " + name;
    if (out.size() > kNameBytes) {
        out.resize(kNameBytes);
    }
    return out;
}

bool NameTaken(const NodeTree& tree, const std::string& name) {
    return std::any_of(tree.nodes.begin(), tree.nodes.end(),
                       [&](const Node& node) { return !node.removed && node.name == name; });
}

/// @p stem, or `stem 2`, `stem 3`... — the first no node of @p tree has.
std::string UniqueName(const NodeTree& tree, std::string stem) {
    if (stem.size() > kNameBytes) {
        stem.resize(kNameBytes);
    }
    if (!NameTaken(tree, stem)) {
        return stem;
    }
    for (u32 n = 2;; ++n) {
        const std::string suffix = " " + std::to_string(n);
        std::string name = stem.substr(0, kNameBytes - suffix.size()) + suffix;
        if (!NameTaken(tree, name)) {
            return name;
        }
    }
}

bool Turned(const Quaternion& q) {
    return std::abs(q.w) < 1.0f - 1e-6f;
}

/// A leaf whose own axes say where something goes: an emitter's direction and
/// spawn area, a box's sides. `inline_models.cpp`'s rule.
bool FrameMatters(NodeKind kind) {
    switch (kind) {
    case NodeKind::Helper:
    case NodeKind::Bone:
    case NodeKind::Attachment:
    case NodeKind::Light:
    case NodeKind::Camera:
    case NodeKind::Event:
    case NodeKind::Wc3FaceFx:
        return false;
    default:
        return true;
    }
}

bool IsQuat(geom::AttrType type) {
    return type == geom::AttrType::Quat || type == geom::AttrType::F32x4;
}

bool IsFloat(geom::AttrType type) {
    return type == geom::AttrType::F32 || type == geom::AttrType::F32x2 || type == geom::AttrType::F32x3 ||
           type == geom::AttrType::F32x4;
}

/// Every @p T in @p bytes through @p f, in place. A key's tangents and control
/// values are stored beside it as more of the same, and every map here is
/// linear, so they go the way the value does.
template <class T, class F>
u32 MapValues(std::vector<u8>& bytes, F&& f) {
    const std::size_t count = bytes.size() / sizeof(T);
    for (std::size_t i = 0; i < count; ++i) {
        T value;
        std::memcpy(&value, bytes.data() + i * sizeof(T), sizeof(T));
        value = f(value);
        std::memcpy(bytes.data() + i * sizeof(T), &value, sizeof(T));
    }
    return static_cast<u32>(count);
}

void ScaleFloats(std::vector<u8>& bytes, f32 factor) {
    MapValues<f32>(bytes, [factor](f32 v) { return v * factor; });
}

template <class T>
std::vector<u8> Bytes(const T& value) {
    std::vector<u8> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

/// The channel keying @p node's @p what (its first, `sub` 0), or `kInvalidIndex`.
u32 NodeChannelIndex(const Model& model, u32 node, Channel what) {
    const std::vector<AnimChannel>& channels = model.animChannels.channels;
    for (u32 c = 0; c < channels.size(); ++c) {
        const TrackTarget& target = channels[c].target;
        if (target.kind == TrackTarget::Kind::Node && target.node == node && target.channel == what &&
            target.sub == 0) {
            return c;
        }
    }
    return kInvalidIndex;
}

/// Every sub-track of @p model's clips keying channel @p id, with its clip.
template <class F>
void ForEachTrack(Document& document, u32 model, u32 id, F&& f) {
    for (Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            for (SubTrack& track : container.subTracks) {
                if (track.channel == id) {
                    f(clip, track);
                }
            }
        }
    }
}

/// @p model's frame loop, made when it has none.
u32 FrameLoop(Document& document, u32 model) {
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model == model && IsGlobalLoop(clip) && clip.name == kFrameLoopName && !clip.containers.empty()) {
            return c;
        }
    }
    Clip loop;
    loop.name = kFrameLoopName;
    loop.model = model;
    loop.duration = 1.0f;
    loop.looping = true;
    loop.flags = ClipFlags::AutoPlay | ClipFlags::WorldClocked;
    loop.containers.emplace_back();
    document.clips.push_back(std::move(loop));
    return static_cast<u32>(document.clips.size() - 1);
}

/// Whether @p track keys @p clip inside its own span: an `.mdx` import keeps
/// the neighbouring windows' bracket keys beside a clip's own, outside it.
bool KeysWithin(const Clip& clip, const SubTrack& track) {
    return std::any_of(track.times.begin(), track.times.end(),
                       [&](f32 t) { return t >= -1e-4f && t <= clip.duration + 1e-4f; });
}

/// A key at @p track's start holding @p rest, its tangents flat: zero where
/// they are derivatives (a Hermite vector's), the value where they are
/// positions.
void KeyAtStart(SubTrack& track, const std::vector<u8>& rest, bool derivative) {
    const std::size_t per = ValuesPerKey(track.interp);
    std::vector<u8> key;
    for (std::size_t v = 0; v < per; ++v) {
        if (v != 0 && derivative) {
            key.insert(key.end(), rest.size(), u8{0});
        } else {
            key.insert(key.end(), rest.begin(), rest.end());
        }
    }
    const auto at = std::lower_bound(track.times.begin(), track.times.end(), 0.0f);
    const std::size_t index = static_cast<std::size_t>(at - track.times.begin());
    track.times.insert(at, 0.0f);
    track.values.insert(track.values.begin() + static_cast<std::ptrdiff_t>(index * key.size()), key.begin(),
                        key.end());
    if (!track.tcb.empty()) {
        track.tcb.insert(track.tcb.begin() + static_cast<std::ptrdiff_t>(index * 3), {0.0f, 0.0f, 0.0f});
    }
}

} // namespace

void TurnFrame(Document& document, u32 model, u32 node, const Quaternion& turn, f32 scale, FrameCounts& counts) {
    for (const Channel what : {Channel::Rotation, Channel::Scale}) {
        const bool rotation = what == Channel::Rotation;
        if (rotation ? !Turned(turn) : scale == 1.0f) {
            continue;
        }
        Model& host = document.models[model];
        u32 index = NodeChannelIndex(host, node, what);
        if (index == kInvalidIndex) {
            AnimChannel channel;
            channel.id = host.animChannels.nextFreeId();
            channel.target.kind = TrackTarget::Kind::Node;
            channel.target.node = node;
            channel.target.channel = what;
            channel.valueType = rotation ? geom::AttrType::Quat : geom::AttrType::F32x3;
            host.animChannels.add(channel);
            index = static_cast<u32>(host.animChannels.channels.size() - 1);
        }
        AnimChannel& channel = host.animChannels.channels[index];
        const geom::AttrType type = channel.valueType;
        if (rotation ? !IsQuat(type) : !IsFloat(type)) {
            continue;
        }
        const auto apply = [&](std::vector<u8>& bytes) {
            if (rotation) {
                MapValues<Quaternion>(bytes, [&](const Quaternion& q) { return turn * q; });
            } else {
                ScaleFloats(bytes, scale);
            }
        };
        if (!channel.hasInitValue()) {
            channel.initValue = rotation ? Bytes(Quaternion{0, 0, 0, 1})
                                : type == geom::AttrType::F32 ? Bytes(1.0f)
                                                              : Bytes(Vector3f{1, 1, 1});
        }
        apply(channel.initValue);
        const std::vector<u8> rest = channel.initValue;
        const u32 id = channel.id;

        bool keyed = false;
        bool global = false;
        ForEachTrack(document, model, id, [&](Clip& clip, SubTrack& track) {
            const bool loop = IsGlobalLoop(clip);
            keyed = keyed || (loop ? !track.times.empty() : KeysWithin(clip, track));
            global = global || (loop && !track.times.empty());
            counts.keysRewritten += static_cast<u32>(track.times.size());
            apply(track.values);
        });
        if (global) {
            continue;
        }
        const auto add = [&](Clip& clip) {
            SubTrack track;
            track.channel = id;
            track.interp = rotation ? Interpolation::Slerp : Interpolation::Linear;
            track.times = {0.0f};
            track.values = rest;
            if (clip.containers.empty()) {
                clip.containers.emplace_back();
            }
            clip.containers.front().subTracks.push_back(std::move(track));
            ++counts.keysAdded;
        };
        if (!keyed) {
            add(document.clips[FrameLoop(document, model)]);
            continue;
        }
        const bool derivative = !rotation;
        for (Clip& clip : document.clips) {
            if (clip.model != model || IsGlobalLoop(clip)) {
                continue;
            }
            SubTrack* own = nullptr;
            for (SubTrackContainer& container : clip.containers) {
                for (SubTrack& track : container.subTracks) {
                    own = own == nullptr && track.channel == id ? &track : own;
                }
            }
            if (own == nullptr) {
                add(clip);
            } else if (!KeysWithin(clip, *own)) {
                KeyAtStart(*own, rest, derivative && own->interp == Interpolation::Hermite);
                ++counts.keysAdded;
            }
        }
    }
}

namespace {

void GrowBox(Extent& box, const Placement& placement) {
    if (!box.valid()) {
        return;
    }
    const Extent before = box;
    ResetExtent(box);
    for (u32 c = 0; c < 8; ++c) {
        const Vector3f corner{(c & 1) ? before.maximum.x : before.minimum.x,
                              (c & 2) ? before.maximum.y : before.minimum.y,
                              (c & 4) ? before.maximum.z : before.minimum.z};
        GrowExtent(box, placement.point(corner));
    }
    FinishExtent(box);
    box.sphereRadius = before.sphereRadius * placement.scale;
}

void Offset(Extent& box, const Vector3f& by) {
    if (!box.valid()) {
        return;
    }
    box.minimum = box.minimum + by;
    box.maximum = box.maximum + by;
}

/// The model-space points a payload holds, placed: after `RescaleNodePayload`
/// has scaled them about the origin, so what is left is the turn and the move.
/// A @p frame node's collision shape is stored scaled about its pivot instead,
/// since the turn reaches it through the node.
void PlacePoints(NodePayload& payload, const Placement& placement, bool frame, const Vector3f& oldPivot,
                 const Vector3f& newPivot, u32& approximated) {
    Placement rigid = placement;
    rigid.scale = 1.0f;
    if (auto* bone = std::get_if<BonePayload>(&payload)) {
        GrowBox(bone->bounds, rigid);
        bone->sphere.center = rigid.point(bone->sphere.center);
    } else if (auto* camera = std::get_if<CameraPayload>(&payload)) {
        camera->target = rigid.point(camera->target);
    } else if (auto* collision = std::get_if<CollisionPayload>(&payload)) {
        CollisionShapeDesc& shape = collision->shape;
        if (frame) {
            const Vector3f by = newPivot - oldPivot * placement.scale;
            Offset(shape.box, by);
            shape.sphere.center = shape.sphere.center + by;
        } else {
            approximated += Turned(placement.rotation) && shape.box.valid() ? 1u : 0u;
            GrowBox(shape.box, rigid);
            shape.sphere.center = rigid.point(shape.sphere.center);
        }
    }
}

} // namespace

bool Placement::moves() const {
    return Turned(rotation) || scale != 1.0f || translation.x != 0.0f || translation.y != 0.0f ||
           translation.z != 0.0f;
}

SceneRescaleResult RescaleScene(Document& document, f32 factor) {
    SceneRescaleResult result;
    result.rescale = RescaleDocument(document, factor);
    if (!result.rescale.ok || factor == 1.0f) {
        return result;
    }
    FrameCounts counts;
    u32 kept = 0;
    for (u32 m = 0; m < document.models.size(); ++m) {
        const NodeTree& tree = document.models[m].nodes;
        if (tree.rig != RigConvention::PivotRelative) {
            continue;
        }
        for (u32 n = 0; n < document.models[m].nodes.size(); ++n) {
            const Node& node = document.models[m].nodes.nodes[n];
            if (node.removed || node.kind != NodeKind::Wc3CornEmitter) {
                continue;
            }
            // Its children would grow twice.
            if (!document.models[m].nodes.children(n).empty()) {
                ++kept;
                continue;
            }
            TurnFrame(document, m, n, Quaternion{0, 0, 0, 1}, factor, counts);
            ++result.framesScaled;
        }
    }
    result.keysAdded = counts.keysAdded;
    if (result.framesScaled != 0) {
        result.rescale.diagnostics.info(DiagCode::GeometryRescaled,
                                        std::to_string(result.framesScaled) +
                                            " PopcornFX emitter(s) scaled on their own frame, " +
                                            std::to_string(counts.keysAdded) + " key(s) added");
    }
    if (kept != 0) {
        result.rescale.diagnostics.warn(DiagCode::GeometryRescaled,
                                        std::to_string(kept) +
                                            " PopcornFX emitter(s) with children keep their effect's size");
    }
    return result;
}

std::vector<u32> SubtreeNodes(const NodeTree& tree, std::span<const u32> roots) {
    std::vector<u8> in(tree.size(), 0);
    std::vector<u32> stack;
    for (const u32 root : roots) {
        if (root < tree.size()) {
            stack.push_back(root);
        }
    }
    while (!stack.empty()) {
        const u32 n = stack.back();
        stack.pop_back();
        if (in[n] != 0 || tree.nodes[n].removed) {
            continue;
        }
        in[n] = 1;
        for (const u32 child : tree.children(n)) {
            stack.push_back(child);
        }
    }
    std::vector<u32> out;
    for (u32 n = 0; n < in.size(); ++n) {
        if (in[n] != 0) {
            out.push_back(n);
        }
    }
    return out;
}

std::vector<u32> VertexOwners(const Mesh& mesh) {
    const u32 count = mesh.vertexCount();
    std::vector<u32> owner(count, kInvalidNode);
    if (!mesh.skin.empty()) {
        for (u32 v = 0; v < count && v < mesh.skin.vertexCount(); ++v) {
            f32 best = -1.0f;
            for (const geom::Influence& influence : mesh.skin.forVertex(v)) {
                if (influence.weight > best) {
                    best = influence.weight;
                    owner[v] = influence.bone;
                }
            }
        }
    }
    const bool rigid = std::any_of(mesh.sections.begin(), mesh.sections.end(),
                                   [](const MeshSection& section) { return section.rigidNode.has_value(); });
    if (!rigid) {
        return owner;
    }
    // A rigid section binds every vertex of its faces, over the skin (§5.6).
    const geom::FaceSet& faces = mesh.faceSet();
    const std::span<const u32> sections = mesh.faceSections();
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        const u32 valence = faces.faceValence[f];
        const u32 section = f < sections.size() ? sections[f] : 0u;
        if (section < mesh.sections.size() && mesh.sections[section].rigidNode) {
            for (u32 c = 0; c < valence && corner + c < faces.cornerVertex.size(); ++c) {
                const u32 v = faces.cornerVertex[corner + c];
                if (v < count) {
                    owner[v] = *mesh.sections[section].rigidNode;
                }
            }
        }
        corner += valence;
    }
    return owner;
}

PlaceResult PlaceNodes(Document& document, u32 model, std::span<const u32> roots, const Placement& placement) {
    PlaceResult result;
    if (model >= document.models.size()) {
        result.diagnostics.error(DiagCode::IndexOutOfRange, "no model " + std::to_string(model));
        return result;
    }
    if (document.models[model].nodes.rig != RigConvention::PivotRelative) {
        result.diagnostics.error(DiagCode::OperationUnsupported, "a placement moves a pivot rig only",
                                 ElementRef(ElementKind::Document, model));
        return result;
    }
    if (!std::isfinite(placement.scale) || placement.scale <= 0.0f) {
        result.diagnostics.error(DiagCode::GeometryRescaled, "a placement's scale must be finite and positive");
        return result;
    }
    const std::vector<u32> nodes = SubtreeNodes(document.models[model].nodes, roots);
    result.ok = true;
    if (nodes.empty() || !placement.moves()) {
        return result;
    }

    const f32 k = placement.scale;
    const bool turned = Turned(placement.rotation);
    std::vector<u8> member;
    std::vector<u8> frame;
    std::vector<u8> grown;
    {
        const NodeTree& tree = document.models[model].nodes;
        member.assign(tree.size(), 0);
        frame.assign(tree.size(), 0);
        grown.assign(tree.size(), 0);
        for (const u32 n : nodes) {
            member[n] = 1;
            // A frame turns with the placement only on a leaf: a child would turn twice.
            const bool leaf = tree.children(n).empty();
            frame[n] = turned && leaf && FrameMatters(tree.nodes[n].kind) ? 1 : 0;
            grown[n] = k != 1.0f && leaf && tree.nodes[n].kind == NodeKind::Wc3CornEmitter ? 1 : 0;
        }
    }

    // Pivots, rests and payloads. A root's parent stays where it was, so its
    // rest is placed as the point its pivot is; an inner node's as a vector.
    Model& host = document.models[model];
    NodeTree& tree = host.nodes;
    for (const u32 n : nodes) {
        Node& node = tree.nodes[n];
        const u32 parent = node.parent;
        const bool inner = parent < tree.size() && parent != n && member[parent] != 0;
        const Vector3f parentPivot =
            parent < tree.size() && parent != n && !inner ? tree.nodes[parent].pivot : Vector3f{0, 0, 0};
        const Vector3f oldPivot = node.pivot;
        node.pivot = placement.point(node.pivot);
        const auto rest = [&](Transform& t) {
            t.translation = inner ? placement.vector(t.translation)
                                  : placement.point(t.translation + parentPivot) - parentPivot;
            t.rotation = frame[n] != 0 ? placement.rotation * t.rotation
                                       : placement.rotation * t.rotation * placement.rotation.conjugate();
        };
        rest(node.local);
        for (Transform& pose : node.poses) {
            rest(pose);
        }
        if (k != 1.0f) {
            RescaleNodePayload(node.payload, k);
        }
        PlacePoints(node.payload, placement, frame[n] != 0, oldPivot, node.pivot, result.approximated);
        ++result.nodesPlaced;
    }

    // Their keys: a translation is a vector, a rotation conjugates, a length
    // scales by its power. A turned frame's rotation and a grown PopcornFX
    // emitter's scale are `TurnFrame`'s, below.
    std::unordered_map<u32, u32> placed; // channel id -> index
    for (u32 c = 0; c < host.animChannels.channels.size(); ++c) {
        const AnimChannel& channel = host.animChannels.channels[c];
        const TrackTarget& target = channel.target;
        if (target.kind != TrackTarget::Kind::Node || target.node >= tree.size() || member[target.node] == 0) {
            continue;
        }
        if ((target.channel == Channel::Rotation && frame[target.node] != 0) ||
            (target.channel == Channel::Scale && grown[target.node] != 0)) {
            continue;
        }
        placed.emplace(channel.id, c);
    }
    const auto place = [&](const AnimChannel& channel, std::vector<u8>& bytes) -> u32 {
        const geom::AttrType type = channel.valueType;
        switch (channel.target.channel) {
        case Channel::Translation:
        case Channel::Target:
            return type == geom::AttrType::F32x3
                       ? MapValues<Vector3f>(bytes, [&](const Vector3f& v) { return placement.vector(v); })
                       : 0u;
        case Channel::Rotation:
            return IsQuat(type) ? MapValues<Quaternion>(bytes,
                                                        [&](const Quaternion& q) {
                                                            return placement.rotation * q *
                                                                   placement.rotation.conjugate();
                                                        })
                                : 0u;
        case Channel::Scale:
            // A uniform scale commutes with the turn; any other only along the
            // axes the turn keeps.
            if (turned && type == geom::AttrType::F32x3) {
                MapValues<Vector3f>(bytes, [&](const Vector3f& s) {
                    if (std::abs(s.x - s.y) > 1e-4f || std::abs(s.y - s.z) > 1e-4f) {
                        ++result.approximated;
                    }
                    return s;
                });
            }
            return 0u;
        default: {
            const int power = ChannelLengthPower(host, channel);
            if (power == 0 || k == 1.0f || !IsFloat(type)) {
                return 0u;
            }
            const f32 multiplier = power == 1 ? k : std::pow(k, static_cast<f32>(power));
            ScaleFloats(bytes, multiplier);
            return static_cast<u32>(bytes.size() / sizeof(f32));
        }
        }
    };
    for (const auto& [id, c] : placed) {
        AnimChannel& channel = host.animChannels.channels[c];
        if (channel.hasInitValue()) {
            place(channel, channel.initValue);
        }
    }
    for (Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            for (SubTrack& track : container.subTracks) {
                const auto it = placed.find(track.channel);
                if (it == placed.end()) {
                    continue;
                }
                place(document.models[model].animChannels.channels[it->second], track.values);
                result.keysRewritten += static_cast<u32>(track.times.size());
            }
        }
    }
    FrameCounts counts;
    for (const u32 n : nodes) {
        if (frame[n] == 0 && grown[n] == 0) {
            continue;
        }
        TurnFrame(document, model, n, frame[n] != 0 ? placement.rotation : Quaternion{0, 0, 0, 1},
                  grown[n] != 0 ? k : 1.0f, counts);
        ++result.framesTurned;
    }
    result.keysRewritten += counts.keysRewritten;
    result.keysAdded = counts.keysAdded;

    // The vertices riding them. A mesh that rides whole turns its every corner;
    // one that rides in part, the corners of its riding vertices.
    for (Mesh& mesh : document.models[model].meshes) {
        const std::vector<u32> owner = VertexOwners(mesh);
        std::vector<u8> rides(owner.size(), 0);
        u32 riding = 0;
        for (u32 v = 0; v < owner.size(); ++v) {
            rides[v] = owner[v] < member.size() && member[owner[v]] != 0 ? 1 : 0;
            riding += rides[v];
        }
        if (riding == 0) {
            continue;
        }
        const bool whole = riding == owner.size();
        const std::span<Vector3f> positions =
            mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        for (u32 v = 0; v < positions.size() && v < rides.size(); ++v) {
            if (rides[v] != 0) {
                positions[v] = placement.point(positions[v]);
            }
        }
        std::vector<u8> cornerRides;
        if (!whole && mesh.attributes.has(geom::names::kNormal, geom::Domain::Halfedge)) {
            mesh.ensureConnectivity();
            const geom::Topology& topology = mesh.topology();
            cornerRides.assign(topology.halfedgeCount(), 0);
            for (u32 h = 0; h < topology.halfedgeCount(); ++h) {
                const u32 v = static_cast<u32>(topology.from(geom::HalfedgeId(h)).index());
                cornerRides[h] = v < rides.size() ? rides[v] : 0;
            }
        }
        const auto turnsAt = [&](geom::Domain domain, u32 i) {
            if (whole) {
                return true;
            }
            const std::vector<u8>& of = domain == geom::Domain::Vertex ? rides : cornerRides;
            return i < of.size() && of[i] != 0;
        };
        for (const geom::Domain domain : {geom::Domain::Vertex, geom::Domain::Halfedge}) {
            const std::span<Vector3f> normals = mesh.attributes.get<Vector3f>(geom::names::kNormal, domain);
            for (u32 i = 0; i < normals.size(); ++i) {
                if (turnsAt(domain, i)) {
                    normals[i] = placement.rotation.rotate_vector(normals[i]);
                }
            }
            const std::span<std::array<f32, 4>> tangents =
                mesh.attributes.get<std::array<f32, 4>>(geom::names::kTangent, domain);
            for (u32 i = 0; i < tangents.size(); ++i) {
                if (turnsAt(domain, i)) {
                    std::array<f32, 4>& t = tangents[i];
                    const Vector3f axis = placement.rotation.rotate_vector(Vector3f{t[0], t[1], t[2]});
                    t = {axis.x, axis.y, axis.z, t[3]};
                }
            }
        }
        mesh.recomputeBounds();
        result.verticesPlaced += riding;
    }

    const PhysicsSet& physics = document.models[model].physics;
    const bool riders = std::any_of(physics.bodies.begin(), physics.bodies.end(), [&](const PhysicsBody& body) {
        return body.node < member.size() && member[body.node] != 0;
    });
    if (riders) {
        result.diagnostics.warn(DiagCode::OperationUnsupported,
                                "the bodies on the placed nodes stay where they were",
                                ElementRef(ElementKind::Document, model));
    }
    return result;
}

namespace {

template <class T>
T OptionAt(const std::vector<T>& values, u32 index, T fallback) {
    return index < values.size() ? values[index] : fallback;
}

/// @p model's meshes less the ones @p keep refuses, every index that named one
/// renumbered — a section channel's, a bone's gate. Per old mesh, its new
/// index or `kInvalidIndex`.
std::vector<u32> KeepMeshes(Model& model, const std::vector<u8>& keep) {
    std::vector<u32> remap(model.meshes.size(), kInvalidIndex);
    std::vector<Mesh> kept;
    for (u32 m = 0; m < model.meshes.size(); ++m) {
        if (m < keep.size() && keep[m] == 0) {
            continue;
        }
        remap[m] = static_cast<u32>(kept.size());
        kept.push_back(std::move(model.meshes[m]));
    }
    model.meshes = std::move(kept);
    std::erase_if(model.animChannels.channels, [&](const AnimChannel& channel) {
        return channel.target.kind == TrackTarget::Kind::Section && channel.target.mesh < remap.size() &&
               remap[channel.target.mesh] == kInvalidIndex;
    });
    for (AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Section && channel.target.mesh < remap.size()) {
            channel.target.mesh = remap[channel.target.mesh];
        }
    }
    for (Node& node : model.nodes.nodes) {
        if (auto* bone = std::get_if<BonePayload>(&node.payload); bone != nullptr && bone->gateMesh < remap.size()) {
            bone->gateMesh = remap[bone->gateMesh];
        }
    }
    return remap;
}

/// The channel of @p table on @p target's node, sub and channel, or null.
const AnimChannel* NodeChannel(const AnimChannelTable& table, const TrackTarget& target) {
    for (const AnimChannel& channel : table.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node && channel.target.node == target.node &&
            channel.target.sub == target.sub && channel.target.channel == target.channel) {
            return &channel;
        }
    }
    return nullptr;
}

} // namespace

MergeResult MergeModel(Document& into, u32 model, Document donor, u32 from, const MergeOptions& options) {
    MergeResult result;
    if (model >= into.models.size() || from >= donor.models.size()) {
        result.diagnostics.error(DiagCode::IndexOutOfRange, "no such model to merge into or from");
        return result;
    }
    if (into.models[model].nodes.rig != RigConvention::PivotRelative ||
        donor.models[from].nodes.rig != RigConvention::PivotRelative) {
        result.diagnostics.error(DiagCode::OperationUnsupported, "a merge joins two pivot rigs only");
        return result;
    }
    // Any choice at all: what is left behind takes what only it used along.
    const bool choosing = !options.nodes.empty() || !options.meshes.empty() || !options.slots.empty() ||
                          !options.clips.empty() || !options.asHelper.empty();
    result.nodeOf.assign(donor.models[from].nodes.size(), kInvalidNode);
    result.meshOf.assign(donor.models[from].meshes.size(), kInvalidIndex);
    result.clipOf.assign(donor.clips.size(), kInvalidIndex);

    // 1. The one model, alone, with the clips it brings and nothing that named the rest.
    std::vector<u32> clipOrigin;
    {
        Model kept = std::move(donor.models[from]);
        std::vector<Clip> brought;
        for (u32 c = 0; c < donor.clips.size(); ++c) {
            Clip& clip = donor.clips[c];
            if (clip.model != from) {
                continue;
            }
            const bool loop = IsGlobalLoop(clip);
            const bool bring = options.clips.empty() ? loop : OptionAt<u8>(options.clips, c, 0) != 0;
            if (!bring) {
                if (!loop) {
                    ++result.clipsLeft;
                }
                continue;
            }
            clip.model = 0;
            brought.push_back(std::move(clip));
            clipOrigin.push_back(c);
        }
        if (!kept.physics.empty() || !kept.poseStages.empty()) {
            result.diagnostics.info(DiagCode::FeatureDropped,
                                    "the merged model's physics and pose stages stay behind");
        }
        kept.physics = PhysicsSet{};
        kept.poseStages.clear();
        kept.testPoses.clear();
        kept.tPose = kInvalidIndex;
        kept.trackSets.clear();
        kept.animSet = kInvalidIndex;
        std::erase_if(kept.animChannels.channels, [](const AnimChannel& channel) {
            return IsPhysicsChannel(channel.target) || IsStageChannel(channel.target);
        });
        for (Node& node : kept.nodes.nodes) {
            node.skin.poseDeltas.clear();
            node.skin.poseSources.clear();
            if (auto* attached = std::get_if<AttachmentPayload>(&node.payload)) {
                attached->model = kInvalidIndex;
            }
        }
        donor.models.clear();
        donor.models.push_back(std::move(kept));
        donor.clips = std::move(brought);
        donor.animSets.clear();
    }

    // 1b. The meshes it brings, then the nodes it leaves out: their children
    // and their skin go to the nearest node kept, their events with them.
    std::vector<u32> meshOrigin;
    {
        Model& kept = donor.models.front();
        const std::vector<u32> meshes = KeepMeshes(kept, options.meshes);
        for (u32 m = 0; m < meshes.size(); ++m) {
            if (meshes[m] != kInvalidIndex) {
                meshOrigin.push_back(m);
            }
        }
    }
    std::vector<u32> nodeOrigin(donor.models.front().nodes.size());
    std::iota(nodeOrigin.begin(), nodeOrigin.end(), 0u);
    {
        Model& kept = donor.models.front();
        NodeTree& tree = kept.nodes;
        std::vector<u8> skipped(tree.size(), 0);
        bool any = false;
        for (u32 n = 0; n < tree.size(); ++n) {
            if (OptionAt(options.nodes, n, MergeNodeAction::Add) == MergeNodeAction::Skip) {
                skipped[n] = 1;
                any = true;
            }
        }
        if (any) {
            // Their channels and events stay behind with them.
            for (Clip& clip : donor.clips) {
                std::erase_if(clip.events, [&](const ClipEvent& event) {
                    return event.node < skipped.size() && skipped[event.node] != 0;
                });
            }
            std::erase_if(kept.animChannels.channels, [&](const AnimChannel& channel) {
                return channel.target.kind == TrackTarget::Kind::Node && channel.target.node < skipped.size() &&
                       skipped[channel.target.node] != 0;
            });
            NodeReferencers referencers;
            referencers.meshes = kept.meshes;
            referencers.channels = &kept.animChannels;
            referencers.clips = donor.clips;
            for (u32 n = tree.size(); n-- > 0;) {
                if (skipped[n] != 0) {
                    RemoveNode(tree, n, RemovePolicy::ReparentChildren, SkinPolicy::ReassignToParent,
                               /*preserveWorld=*/true, referencers);
                }
            }
            const NodeRemaps remaps = CompactNodes(tree, referencers, result.diagnostics);
            std::vector<u32> origin(remaps.newCount, kInvalidNode);
            for (u32 n = 0; n < remaps.nodes.size(); ++n) {
                if (remaps.nodes[n] < origin.size()) {
                    origin[remaps.nodes[n]] = n;
                }
            }
            nodeOrigin = std::move(origin);
        }
    }

    // 2. Its units.
    if (options.matchUnits) {
        const f32 factor = RescaleFactorBetween(donor.defaultProfile, options.profile);
        if (factor != 1.0f) {
            SceneRescaleResult rescaled = RescaleScene(donor, factor);
            if (rescaled.rescale.ok) {
                result.unitScale = factor;
            }
        }
    }

    // 3. Its materials, in exactly the model's profiles: derived where it has
    // none, from the profile nearest it, and drawn by what that one draws.
    const ProfileMask ours = into.declaredMask();
    {
        Model& merged = donor.models.front();
        const auto sourceFor = [&](ProfileId to) {
            const ProfileId sibling = to == ProfileId::Wc3Classic    ? ProfileId::Wc3Reforged
                                      : to == ProfileId::Wc3Reforged ? ProfileId::Wc3Classic
                                                                     : ProfileId::Count;
            if (sibling != ProfileId::Count && merged.setFor(sibling)) {
                return sibling;
            }
            if (merged.setFor(donor.defaultProfile)) {
                return donor.defaultProfile;
            }
            return merged.profileSets.empty() ? ProfileId::Count : merged.profileSets.front().profile;
        };
        for (const ProfileId profile : into.profiles) {
            if (donor.models.front().setFor(profile)) {
                continue;
            }
            const ProfileId source = sourceFor(profile);
            if (source == ProfileId::Count) {
                break;
            }
            donor.declare(source);
            const DeriveResult derived = DeriveProfile(donor, source, profile);
            if (!derived.ok) {
                result.diagnostics.append(derived.diagnostics);
                continue;
            }
            ++result.setsDerived;
            for (Mesh& mesh : donor.models.front().meshes) {
                for (MeshSection& section : mesh.sections) {
                    if (HasProfile(section.profiles, source)) {
                        section.profiles |= ProfileBit(profile);
                    }
                }
            }
        }
        Model& kept = donor.models.front();
        std::erase_if(kept.profileSets,
                      [&](const ProfileMaterialSet& set) { return !HasProfile(ours, set.profile); });
        for (Mesh& mesh : kept.meshes) {
            for (MeshSection& section : mesh.sections) {
                section.profiles &= ours;
            }
        }
        std::erase_if(kept.animChannels.channels, [&](const AnimChannel& channel) {
            return IsMaterialTarget(channel.target.kind) && !HasProfile(ours, channel.target.material.profile);
        });
        donor.profiles = into.profiles;
        donor.defaultProfile = into.defaultProfile;
    }

    // 4. What no profile of the model runs becomes a helper; so does a camera,
    // the merged model's portrait camera and not this one's — unless the
    // choices name which become helpers.
    std::vector<u8> demoted(donor.models.front().nodes.size(), 0);
    for (u32 n = 0; n < demoted.size(); ++n) {
        Node& node = donor.models.front().nodes.nodes[n];
        bool carried = false;
        for (const ProfileId profile : into.profiles) {
            carried = carried || CarriesNodeKind(profile, node.kind);
        }
        const bool asked = OptionAt<u8>(options.asHelper, nodeOrigin[n], 0) != 0;
        const bool cameraStays = !options.asHelper.empty() || node.kind != NodeKind::Camera;
        if (carried && !asked && cameraStays) {
            continue;
        }
        node.kind = NodeKind::Helper;
        node.resetPayloadForKind();
        demoted[n] = 1;
        ++result.nodesDemoted;
    }
    std::erase_if(donor.models.front().animChannels.channels, [&](const AnimChannel& channel) {
        const TrackTarget& target = channel.target;
        return target.kind == TrackTarget::Kind::Node && target.node < demoted.size() && demoted[target.node] != 0 &&
               target.channel != Channel::Translation && target.channel != Channel::Rotation &&
               target.channel != Channel::Scale;
    });

    // 5. In as a model of its own, every texture index rebased.
    const u32 clipBase = static_cast<u32>(into.clips.size());
    const u32 textureBase = static_cast<u32>(into.textures.size());
    const u32 appended = AppendDocument(into, std::move(donor), result.diagnostics);
    if (appended == kInvalidIndex) {
        result.diagnostics.error(DiagCode::OperationUnsupported, "the model's materials cannot be merged");
        return result;
    }

    // 6. Copied into ours under the group's helper.
    Model& host = into.models[model];
    const Model& source = into.models[appended];
    const std::string group = UniqueName(host.nodes, options.name.empty() ? std::string("Merged") : options.name);
    {
        Node helper;
        helper.name = group;
        helper.kind = NodeKind::Helper;
        if (options.groupParent < host.nodes.size() && !host.nodes.nodes[options.groupParent].removed) {
            helper.parent = options.groupParent;
        }
        result.group = host.nodes.add(std::move(helper));
    }
    const u32 base = host.nodes.size();
    const u32 meshBase = static_cast<u32>(host.meshes.size());

    // Where each node goes: a node of its own, or the one of ours it becomes.
    NodeTree nodes = source.nodes;
    const u32 count = nodes.size();
    std::vector<u32> remap(count, kInvalidNode);
    std::vector<u8> becomes(count, 0);
    {
        u32 next = base;
        for (u32 i = 0; i < count; ++i) {
            const u32 origin = nodeOrigin[i];
            const u32 target = OptionAt(options.into, origin, kInvalidNode);
            if (OptionAt(options.nodes, origin, MergeNodeAction::Add) == MergeNodeAction::Into) {
                if (target < result.group && !host.nodes.nodes[target].removed) {
                    remap[i] = target;
                    becomes[i] = 1;
                    continue;
                }
                result.diagnostics.warn(DiagCode::OperationUnsupported,
                                        "'" + nodes.nodes[i].name + "' had no node of ours to become; it is added",
                                        ElementRef(ElementKind::Node, origin));
            }
            remap[i] = next++;
        }
    }

    std::vector<u32> slots(source.materialSlots.size(), kInvalidIndex);
    std::vector<u8> slotBrought(source.materialSlots.size(), 0);
    {
        std::vector<u8> used(source.materialSlots.size(), choosing ? 0 : 1);
        if (choosing) {
            for (const Mesh& mesh : source.meshes) {
                for (const MeshSection& section : mesh.sections) {
                    if (section.materialSlot < used.size()) {
                        used[section.materialSlot] = 1;
                    }
                }
            }
            for (u32 i = 0; i < count; ++i) {
                if (becomes[i] != 0) {
                    continue;
                }
                NodePayload payload = nodes.nodes[i].payload;
                ForEachMaterialLink(payload, [&](u32& slot) {
                    if (slot < used.size()) {
                        used[slot] = 1;
                    }
                });
            }
        }
        for (u32 s = 0; s < source.materialSlots.size(); ++s) {
            const u32 ourSlot = choosing ? OptionAt(options.slots, s, kInvalidIndex) : kInvalidIndex;
            if (ourSlot < host.materialSlots.size()) {
                slots[s] = ourSlot;
                continue;
            }
            if (used[s] == 0) {
                continue;
            }
            std::string name = group + "|" + source.materialSlots[s];
            while (host.slotIndex(name) != kInvalidIndex) {
                name += "'";
            }
            slots[s] = host.addSlot(name);
            slotBrought[s] = 1;
            ++result.slots;
        }
    }
    const auto slotOf = [&](u32 slot) { return slot < slots.size() ? slots[slot] : slot; };
    for (ProfileMaterialSet& set : host.profileSets) {
        set.resizeBindings(host.materialSlots.size());
        const ProfileMaterialSet* own = source.setFor(set.profile);
        if (own == nullptr) {
            if (result.slots != 0) {
                result.diagnostics.warn(DiagCode::OperationUnsupported,
                                        std::string("the merged model has no ") + ToString(set.profile) +
                                            " materials; its slots are unbound there");
            }
            continue;
        }
        if (!choosing) {
            const u32 materialBase = static_cast<u32>(set.materials.size());
            set.materials.insert(set.materials.end(), own->materials.begin(), own->materials.end());
            for (u32 s = 0; s < slots.size() && s < own->slotBindings.size(); ++s) {
                if (!own->slotBindings[s].bound(own->defaultLook)) {
                    continue;
                }
                for (u32& material : set.slotBindings[slots[s]].byLook) {
                    material = materialBase + own->slotBindings[s].byLook[own->defaultLook];
                }
            }
            continue;
        }
        // Only the materials a slot brought draws with.
        std::vector<u32> materialOf(own->materials.size(), kInvalidIndex);
        for (u32 s = 0; s < slots.size() && s < own->slotBindings.size(); ++s) {
            if (slotBrought[s] == 0 || !own->slotBindings[s].bound(own->defaultLook)) {
                continue;
            }
            const u32 material = own->slotBindings[s].byLook[own->defaultLook];
            if (material >= materialOf.size()) {
                continue;
            }
            if (materialOf[material] == kInvalidIndex) {
                materialOf[material] = static_cast<u32>(set.materials.size());
                set.materials.push_back(own->materials[material]);
            }
            for (u32& bound : set.slotBindings[slots[s]].byLook) {
                bound = materialOf[material];
            }
        }
    }

    std::vector<Mesh> meshes = source.meshes;
    AnimChannelTable channels = source.animChannels;
    // A node that becomes ours brings its channels only as a pair.
    std::erase_if(channels.channels, [&](const AnimChannel& channel) {
        const TrackTarget& target = channel.target;
        return target.kind == TrackTarget::Kind::Node && target.node < count && becomes[target.node] != 0 &&
               OptionAt<u8>(options.pair, nodeOrigin[target.node], 0) == 0;
    });
    const u32 clipCount = static_cast<u32>(into.clips.size()) - clipBase;
    NodeReferencers referencers;
    referencers.meshes = meshes;
    referencers.channels = &channels;
    referencers.clips = std::span<Clip>(into.clips.data() + clipBase, clipCount);
    RemapNodeReferencers(nodes, remap, referencers, result.diagnostics);
    u32 eventLoops = 0;
    for (u32 i = 0; i < count; ++i) {
        result.nodeOf[nodeOrigin[i]] = remap[i];
        if (becomes[i] != 0) {
            continue;
        }
        const u32 origin = nodeOrigin[i];
        Node node = std::move(nodes.nodes[i]);
        const bool root = node.parent >= count || node.parent == i;
        node.parent = root ? result.group : remap[node.parent];
        const u32 hangFrom = OptionAt(options.parent, origin, kInvalidNode);
        if (hangFrom < result.group && !host.nodes.nodes[hangFrom].removed) {
            node.parent = hangFrom;
        }
        if (OptionAt<u8>(options.keepName, origin, 0) == 0) {
            node.name = Behind(group, node.name);
        }
        if (auto* attached = std::get_if<AttachmentPayload>(&node.payload)) {
            // Its id would break the climb `toMdx` gives ours.
            attached->model = kInvalidIndex;
            std::erase_if(node.native.entries,
                          [](const NativeBag::Entry& entry) { return entry.name == "mdxAttachmentId"; });
        } else if (auto* bone = std::get_if<BonePayload>(&node.payload)) {
            if (bone->gateMesh != kInvalidIndex) {
                bone->gateMesh += meshBase;
            }
        } else if (auto* event = std::get_if<EventPayload>(&node.payload); event != nullptr && choosing) {
            // The donor numbered its loops, and ours number them afresh.
            if (event->id != 0xFFFFFFFFu) {
                event->id = 0xFFFFFFFFu;
                ++eventLoops;
            }
        }
        ForEachMaterialLink(node.payload, [&](u32& slot) { slot = slotOf(slot); });
        host.nodes.add(std::move(node));
        ++result.nodes;
    }
    if (eventLoops != 0) {
        result.diagnostics.info(DiagCode::FeatureDropped,
                                std::to_string(eventLoops) + " events fire in the animations, not on the merged loops");
    }

    for (u32 m = 0; m < meshes.size(); ++m) {
        Mesh& mesh = meshes[m];
        mesh.name = Behind(group, mesh.name);
        const std::vector<u32> owners = VertexOwners(mesh);
        const bool bound = std::any_of(owners.begin(), owners.end(), [](u32 n) { return n != kInvalidNode; });
        for (MeshSection& section : mesh.sections) {
            section.materialSlot = slotOf(section.materialSlot);
            if (!section.name.empty()) {
                section.name = Behind(group, section.name);
            }
            // A mesh bound to nothing rides the group.
            if (!bound) {
                section.rigidNode = result.group;
            }
        }
        mesh.recomputeBounds();
        if (mesh.bounds.valid()) {
            GrowExtent(host.bounds, mesh.bounds.minimum);
            GrowExtent(host.bounds, mesh.bounds.maximum);
        }
        if (m < meshOrigin.size()) {
            result.meshOf[meshOrigin[m]] = static_cast<u32>(host.meshes.size());
        }
        host.meshes.push_back(std::move(mesh));
    }
    FinishExtent(host.bounds);
    result.meshes = static_cast<u32>(meshes.size());

    // A channel nothing brought keys, and which holds no value of its own,
    // stays behind with what keyed it.
    std::unordered_set<u32> keyed;
    if (choosing) {
        for (u32 c = clipBase; c < clipBase + clipCount; ++c) {
            for (const SubTrackContainer& container : into.clips[c].containers) {
                for (const SubTrack& track : container.subTracks) {
                    if (!track.times.empty()) {
                        keyed.insert(track.channel);
                    }
                }
            }
        }
    }
    std::unordered_map<u32, u32> ids;
    u32 nextId = host.animChannels.nextFreeId();
    for (AnimChannel channel : channels.channels) {
        TrackTarget& target = channel.target;
        if (target.kind == TrackTarget::Kind::Physics) {
            continue;
        }
        if (target.kind == TrackTarget::Kind::Node && target.node >= host.nodes.size()) {
            continue;
        }
        if (choosing && !channel.hasInitValue() && keyed.find(channel.id) == keyed.end()) {
            continue;
        }
        if (target.kind == TrackTarget::Kind::Section) {
            target.mesh += meshBase;
        } else if (IsMaterialTarget(target.kind)) {
            const u32 slot = target.material.slot;
            if (choosing && (slot >= slotBrought.size() || slotBrought[slot] == 0)) {
                continue;
            }
            target.material.slot = slotOf(slot);
            target.material.look = 0;
        }
        // A pair's channel lands on our node's own, where it has one.
        if (target.kind == TrackTarget::Kind::Node && target.node < result.group) {
            if (const AnimChannel* own = NodeChannel(host.animChannels, target)) {
                ids[channel.id] = own->id;
                continue;
            }
        }
        ids[channel.id] = nextId;
        channel.id = nextId++;
        host.animChannels.add(channel);
    }

    for (u32 c = clipBase; c < clipBase + clipCount; ++c) {
        Clip clip = into.clips[c];
        const u32 origin = clipOrigin[c - clipBase];
        const bool loop = IsGlobalLoop(clip);
        clip.model = model;
        clip.trackSets.clear();
        clip.physics.reset();
        if (loop) {
            clip.name = Behind(group, clip.name);
            // The loop's number is ours to hand out: the merged one may be taken.
            std::erase_if(clip.native.entries,
                          [](const NativeBag::Entry& entry) { return entry.name == "globalSequenceId"; });
            clip.events.clear();
        } else {
            const std::string named = OptionAt(options.clipNames, origin, std::string());
            if (!named.empty()) {
                clip.name = named;
            }
            std::erase_if(clip.events, [&](const ClipEvent& event) {
                return event.node != kInvalidNode && event.node >= host.nodes.size();
            });
        }
        for (SubTrackContainer& container : clip.containers) {
            std::vector<SubTrack> kept;
            for (SubTrack& track : container.subTracks) {
                const auto it = ids.find(track.channel);
                if (it == ids.end()) {
                    continue;
                }
                track.channel = it->second;
                kept.push_back(std::move(track));
            }
            container.subTracks = std::move(kept);
        }
        if (!loop) {
            // Off the donor's timeline: the export lays it after ours.
            DetachClip(host.animChannels, clip);
            ++result.clips;
        } else {
            ++result.loops;
        }
        result.clipOf[origin] = static_cast<u32>(into.clips.size()) - clipCount;
        into.clips.push_back(std::move(clip));
    }

    // 7. The model it came in as, and its own clips, go again.
    into.clips.erase(into.clips.begin() + clipBase, into.clips.begin() + clipBase + clipCount);
    into.models.erase(into.models.begin() + appended);

    // 8. The textures only a set it left behind named.
    for (u32 t = static_cast<u32>(into.textures.size()); t-- > textureBase;) {
        if (CountTextureReferencers(into, t).total() == 0) {
            RemoveTexture(into, t, kInvalidIndex);
        }
    }
    result.textures = static_cast<u32>(into.textures.size()) - textureBase;
    result.ok = true;
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
