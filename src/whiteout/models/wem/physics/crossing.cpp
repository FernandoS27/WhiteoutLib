// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/crossing.h>

#include <whiteout/models/wem/anim/key_reduce.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/meshes/remove.h>
#include <whiteout/models/wem/physics/cloth_cage.h>
#include <whiteout/models/wem/physics/references.h>
#include <whiteout/models/wem/physics/switches.h>
#include <whiteout/models/wem/retarget.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

namespace {

bool Keeps(const PhysicsCaps& caps, RigStart start, bool hasOnDeath) {
    if (caps.switches) {
        return !hasOnDeath || start == RigStart::OnDeath;
    }
    return start == RigStart::Always || start == RigStart::Animated;
}

/// A fracture's rig holds its pieces until its blast. A target that simulates
/// every body from creation would drop them at spawn: its clip is baked instead
/// (EDIT_MODE_FRACTURE_DESIGN.md §12).
bool Keeps(const PhysicsCaps& caps, const PhysicsRig& rig, bool hasOnDeath) {
    if (rig.fracture && !caps.switches) {
        return false;
    }
    return Keeps(caps, rig.start, hasOnDeath);
}

/// Leaves only the rigs @p caps keeps, dropping the bodies no kept rig holds.
u32 ChooseRigs(Model& model, ProfileId target, const PhysicsCaps& caps, Diagnostics& out) {
    PhysicsSet& physics = model.physics;
    const bool hasOnDeath = std::any_of(physics.rigs.begin(), physics.rigs.end(), [](const PhysicsRig& rig) {
        return rig.start == RigStart::OnDeath && !rig.bodies.empty();
    });
    std::set<u32> kept;
    for (const PhysicsRig& rig : physics.rigs) {
        if (Keeps(caps, rig, hasOnDeath)) {
            kept.insert(rig.bodies.begin(), rig.bodies.end());
        }
    }
    u32 changed = 0;
    std::vector<u32> gone;
    for (const PhysicsRig& rig : physics.rigs) {
        if (Keeps(caps, rig, hasOnDeath)) {
            continue;
        }
        ++changed;
        const char* why = rig.fracture ? "a fracture's pieces would fall at spawn: bake its clip"
                          : caps.switches ? "the model's death rig is the one the target switches on"
                                          : "the target simulates from creation and cannot start it later";
        out.warn(DiagCode::PhysicsRigDropped, "rig '" + rig.name + "': " + why,
                 ElementRef(ElementKind::PhysicsRecord, rig.id), target);
        for (const u32 id : rig.bodies) {
            if (kept.count(id) == 0) {
                gone.push_back(id);
            }
        }
    }
    if (!gone.empty()) {
        const std::vector<u32> removed = RemovePhysicsBodies(physics, gone);
        InvalidatePhysicsChannels(model.animChannels, removed, out);
        DetachPoseStages(model.poseStages, removed);
    }
    return changed;
}

/// Every body's state in each clip of @p m, as the switch rule reads it
/// (EDIT_MODE_PHYSICS_BAKE_DESIGN.md §4.5): at 0 and at every switch key's
/// time, which is where a state can change, so a key keeps its time.
struct ClipStates {
    u32 clip = kInvalidIndex;
    std::vector<f32> times;
    std::vector<std::vector<BodySwitch>> states; ///< Per time, in body order.
};

std::vector<ClipStates> RuleStates(const Document& document, u32 m) {
    const Model& model = document.models[m];
    std::vector<ClipStates> out;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model != m || clip.containers.empty() || IsGlobalLoop(clip)) {
            continue;
        }
        ClipStates read;
        read.clip = c;
        read.times.push_back(0.0f);
        for (const SubTrackContainer& container : clip.containers) {
            for (const SubTrack& track : container.subTracks) {
                const AnimChannel* channel = model.animChannels.find(track.channel);
                if (channel == nullptr || channel->target.kind != TrackTarget::Kind::Physics ||
                    (channel->target.channel != Channel::PhysicsDynamic &&
                     channel->target.channel != Channel::PhysicsRagdoll)) {
                    continue;
                }
                for (const f32 t : track.times) {
                    if (t > 0.0f && t <= clip.duration) {
                        read.times.push_back(t);
                    }
                }
            }
        }
        std::sort(read.times.begin(), read.times.end());
        read.times.erase(std::unique(read.times.begin(), read.times.end()), read.times.end());
        const SwitchReader reader(document, m, c, SwitchScope::Clip);
        std::vector<BodySwitch> previous;
        for (const f32 t : read.times) {
            std::vector<BodySwitch> now;
            reader.read(t, previous, now);
            read.states.push_back(now);
            previous = std::move(now);
        }
        out.push_back(std::move(read));
    }
    return out;
}

/// For a target with switches: every rig `Keeps` keeps, and every rig with a
/// member moving in some clip, with the joint partners of what it keeps
/// (§8.6). An *Always* ragdoll beside an *On death* one stays: per-body
/// switches need no single ragdoll.
u32 ChooseSwitchedRigs(Model& model, ProfileId target, const PhysicsCaps& caps, std::span<const ClipStates> states,
                       Diagnostics& out) {
    PhysicsSet& physics = model.physics;
    std::vector<u8> moving(physics.bodies.size(), 0);
    for (const ClipStates& clip : states) {
        for (const std::vector<BodySwitch>& row : clip.states) {
            for (std::size_t b = 0; b < row.size() && b < moving.size(); ++b) {
                moving[b] |= row[b].active ? 1 : 0;
            }
        }
    }
    const auto moves = [&](u32 id) {
        for (std::size_t b = 0; b < physics.bodies.size(); ++b) {
            if (physics.bodies[b].id == id) {
                return moving[b] != 0;
            }
        }
        return false;
    };
    const bool hasOnDeath = std::any_of(physics.rigs.begin(), physics.rigs.end(), [](const PhysicsRig& rig) {
        return rig.start == RigStart::OnDeath && !rig.bodies.empty();
    });
    const auto keeps = [&](const PhysicsRig& rig) {
        return Keeps(caps, rig.start, hasOnDeath) || std::any_of(rig.bodies.begin(), rig.bodies.end(), moves);
    };
    std::set<u32> kept;
    for (const PhysicsRig& rig : physics.rigs) {
        if (keeps(rig)) {
            kept.insert(rig.bodies.begin(), rig.bodies.end());
        }
    }
    // What a kept body hangs from stays with it: a cape root's spine.
    std::set<u32> partners;
    for (const PhysicsJoint& joint : physics.joints) {
        if (kept.count(joint.bodyA) != 0 || kept.count(joint.bodyB) != 0) {
            partners.insert(joint.bodyA);
            partners.insert(joint.bodyB);
        }
    }
    kept.insert(partners.begin(), partners.end());
    u32 changed = 0;
    std::vector<u32> gone;
    for (const PhysicsRig& rig : physics.rigs) {
        if (keeps(rig)) {
            continue;
        }
        ++changed;
        out.warn(DiagCode::PhysicsRigDropped,
                 "rig '" + rig.name + "': no body of it moves in any clip, and the model's death rig is the one the "
                 "target switches on",
                 ElementRef(ElementKind::PhysicsRecord, rig.id), target);
        for (const u32 id : rig.bodies) {
            if (kept.count(id) == 0) {
                gone.push_back(id);
            }
        }
    }
    if (!gone.empty()) {
        const std::vector<u32> removed = RemovePhysicsBodies(physics, gone);
        InvalidatePhysicsChannels(model.animChannels, removed, out);
        DetachPoseStages(model.poseStages, removed);
    }
    return changed;
}

/// The rule's states as the target's switches (§8.6): each rig member's rest,
/// and its `PhysicsDynamic` keyed in a clip wherever its state there leaves
/// it; a rig's drop so folded into its members. An *On death* member keeps
/// its channel, keyed or not, as the target's death rig. An *Inherit* body
/// keeps the flag, whose ancestor rule is the target's own, and a body no rig
/// holds keeps what it had. Blend has no target field: dropped.
void SwitchesFromRule(Document& document, u32 m, std::span<const u32> readIds, std::span<const ClipStates> states,
                      ProfileId target, Diagnostics& out) {
    Model& model = document.models[m];
    PhysicsSet& physics = model.physics;
    std::vector<u32> order;
    for (const PhysicsRig& rig : physics.rigs) {
        for (const u32 id : rig.bodies) {
            if (std::find(order.begin(), order.end(), id) == order.end()) {
                order.push_back(id);
            }
        }
    }
    for (const u32 id : order) {
        PhysicsBody* body = physics.body(id);
        const auto column = std::find(readIds.begin(), readIds.end(), id);
        if (body == nullptr || body->inheritDynamic || column == readIds.end()) {
            continue;
        }
        const std::size_t at = static_cast<std::size_t>(column - readIds.begin());
        TrackTarget rest;
        rest.kind = TrackTarget::Kind::Physics;
        rest.sub = id;
        rest.channel = Channel::PhysicsDynamic;
        body->simulates = PhysicsSwitchRest(model, rest, GameOf(document.defaultProfile)) >= 0.5f;
        const bool onDeath = std::any_of(physics.rigs.begin(), physics.rigs.end(), [&](const PhysicsRig& rig) {
            return rig.start == RigStart::OnDeath && std::find(rig.bodies.begin(), rig.bodies.end(), id) != rig.bodies.end();
        });
        u32 channel = onDeath ? PhysicsSwitchChannel(model, id, Channel::PhysicsDynamic)
                              : FindPhysicsChannel(model, id, Channel::PhysicsDynamic);
        for (const ClipStates& read : states) {
            Clip& clip = document.clips[read.clip];
            if (channel != kInvalidIndex) {
                for (SubTrackContainer& container : clip.containers) {
                    std::erase_if(container.subTracks, [&](const SubTrack& track) { return track.channel == channel; });
                }
            }
            const bool leaves = std::any_of(read.states.begin(), read.states.end(), [&](const std::vector<BodySwitch>& row) {
                return at < row.size() && row[at].active != body->simulates;
            });
            if (!leaves) {
                continue;
            }
            if (channel == kInvalidIndex) {
                channel = PhysicsSwitchChannel(model, id, Channel::PhysicsDynamic);
            }
            SubTrack track;
            track.channel = channel;
            track.interp = Interpolation::Step;
            f32 last = -1.0f;
            for (std::size_t k = 0; k < read.times.size(); ++k) {
                const f32 on = at < read.states[k].size() && read.states[k][at].active ? 1.0f : 0.0f;
                if (on != last) {
                    track.times.push_back(read.times[k]);
                    const u8* bytes = reinterpret_cast<const u8*>(&on);
                    track.values.insert(track.values.end(), bytes, bytes + sizeof(f32));
                    last = on;
                }
            }
            // Held to a loop's end: StarCraft II wraps a looping track at its
            // own last key, which would turn the last state back to the first.
            if (clip.looping && !track.times.empty() && track.times.back() < clip.duration) {
                track.times.push_back(clip.duration);
                const u8* bytes = reinterpret_cast<const u8*>(&last);
                track.values.insert(track.values.end(), bytes, bytes + sizeof(f32));
            }
            clip.containers.front().subTracks.push_back(std::move(track));
        }
    }
    // The drops are in the members' keys now; Blend has nowhere to go.
    std::vector<u32> dropped;
    bool blended = false;
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind != TrackTarget::Kind::Physics ||
            (channel.target.channel != Channel::PhysicsRagdoll && channel.target.channel != Channel::PhysicsBlend)) {
            continue;
        }
        dropped.push_back(channel.id);
        if (channel.target.channel == Channel::PhysicsBlend) {
            for (const Clip& clip : document.clips) {
                const SubTrack* track = clip.model == m ? FindSubTrack(clip, channel.id) : nullptr;
                blended |= track != nullptr && !track->times.empty();
            }
        }
    }
    if (dropped.empty()) {
        return;
    }
    for (Clip& clip : document.clips) {
        if (clip.model != m) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            std::erase_if(container.subTracks, [&](const SubTrack& track) {
                return std::find(dropped.begin(), dropped.end(), track.channel) != dropped.end();
            });
        }
    }
    std::erase_if(model.animChannels.channels, [&](const AnimChannel& channel) {
        return std::find(dropped.begin(), dropped.end(), channel.id) != dropped.end();
    });
    if (blended) {
        out.info(DiagCode::PhysicsUnsupported, "a body's Blend: the target hands a body back to its animation at once",
                 ElementRef(), target);
    }
}

/// A target that runs its bodies from creation cannot key them: each clip
/// that switches bodies is reported (§8.6).
void ReportUnswitched(const Document& document, u32 m, ProfileId target, Diagnostics& out) {
    const Model& model = document.models[m];
    for (const Clip& clip : document.clips) {
        if (clip.model != m) {
            continue;
        }
        const bool switches = std::any_of(clip.containers.begin(), clip.containers.end(), [&](const SubTrackContainer& container) {
            return std::any_of(container.subTracks.begin(), container.subTracks.end(), [&](const SubTrack& track) {
                const AnimChannel* channel = model.animChannels.find(track.channel);
                return channel != nullptr && channel->target.kind == TrackTarget::Kind::Physics && !track.times.empty() &&
                       channel->target.channel != Channel::ClothActive;
            });
        });
        if (switches) {
            out.warn(DiagCode::PhysicsUnsupported,
                     "clip '" + clip.name + "': its physics switches cannot be said; the target runs its bodies from creation",
                     ElementRef(), target);
        }
    }
}

/// The vertices the faces of @p section use.
std::vector<u8> VerticesOf(const Mesh& mesh, u32 section) {
    const geom::FaceSet& faces = mesh.faceSet();
    const std::span<const u32> sections = mesh.faceSections();
    std::vector<u8> used(faces.vertexCount, 0);
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceValence.size(); ++f) {
        for (u32 k = 0; k < faces.faceValence[f]; ++k) {
            const u32 v = faces.cornerVertex[corner + k];
            if (f < sections.size() && sections[f] == section && v < used.size()) {
                used[v] = 1;
            }
        }
        corner += faces.faceValence[f];
    }
    return used;
}

u32 CountOf(const std::vector<u8>& used) {
    return static_cast<u32>(std::count(used.begin(), used.end(), u8{1}));
}

/// Collapses @p cloth's cage down to @p limit particles. Returns the count left.
u32 DecimateCage(Mesh& mesh, u32 section, u32 limit) {
    u32 count = CountOf(VerticesOf(mesh, section));
    if (count <= limit || !mesh.ensureConnectivity().ok()) {
        return count;
    }
    const auto positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const std::span<u8> movable =
        mesh.attributes.getOrCreate<u8>(geom::names::kClothMovable, geom::Domain::Vertex, geom::AttrType::Bool);
    const std::span<std::array<u32, 4>> lanes = mesh.attributes.getOrCreate<std::array<u32, 4>>(
        geom::names::kClothBindVertex, geom::Domain::Vertex, geom::AttrType::U32x4);
    const std::span<std::array<f32, 4>> weights = mesh.attributes.getOrCreate<std::array<f32, 4>>(
        geom::names::kClothBindWeight, geom::Domain::Vertex, geom::AttrType::F32x4);
    const auto movableAt = [&](geom::VertexId v) { return v.value() < movable.size() && movable[v.value()] != 0; };
    const auto length = [&](geom::VertexId a, geom::VertexId b) {
        const Vector3f& p = positions[a.value()];
        const Vector3f& q = positions[b.value()];
        return (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) + (q.z - p.z) * (q.z - p.z);
    };
    while (count > limit) {
        const geom::Topology& topology = std::as_const(mesh).topology();
        const std::span<const u32> sections = mesh.faceSections();
        const auto inCage = [&](geom::HalfedgeId h) {
            const geom::FaceId f = topology.face(h);
            return f.valid() && f.value() < sections.size() && sections[f.value()] == section;
        };
        // The edges of the cage, shortest first; a free end goes into a pinned one.
        std::vector<std::pair<f32, geom::HalfedgeId>> edges;
        for (u32 e = 0; e < topology.edgeCount(); ++e) {
            if (topology.isDeleted(geom::EdgeId(e))) {
                continue;
            }
            geom::HalfedgeId h = geom::Topology::halfedge(geom::EdgeId(e), 0);
            if (!inCage(h) && !inCage(geom::Topology::opposite(h))) {
                continue;
            }
            if (!movableAt(topology.from(h)) && movableAt(topology.to(h))) {
                h = geom::Topology::opposite(h);
            }
            edges.emplace_back(length(topology.from(h), topology.to(h)), h);
        }
        std::sort(edges.begin(), edges.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        bool collapsed = false;
        for (const auto& [_, h] : edges) {
            if (!geom::IsCollapseLegal(mesh, h)) {
                continue;
            }
            const u32 from = topology.from(h).value();
            const u32 to = topology.to(h).value();
            // What the particle drove, it now drives through its survivor.
            for (std::size_t v = 0; v < lanes.size(); ++v) {
                for (std::size_t k = 0; k < 4; ++k) {
                    if (lanes[v][k] != from) {
                        continue;
                    }
                    lanes[v][k] = to;
                    for (std::size_t j = 0; j < 4; ++j) {
                        if (j != k && lanes[v][j] == to && v < weights.size()) {
                            weights[v][j] += weights[v][k];
                            weights[v][k] = 0.0f;
                            lanes[v][k] = geom::kInvalidId;
                            break;
                        }
                    }
                }
            }
            if (to < movable.size() && from < movable.size()) {
                movable[to] = static_cast<u8>(std::min(movable[to], movable[from]));
            }
            collapsed = geom::CollapseEdge(mesh, h);
            if (collapsed) {
                --count;
                break;
            }
        }
        if (!collapsed) {
            break;
        }
    }
    geom::GarbageCollect(mesh);
    return CountOf(VerticesOf(mesh, section));
}

} // namespace

bool CarriesCloth(const Document& document) {
    return std::any_of(document.models.begin(), document.models.end(), [](const Model& model) {
        return !model.physics.cloths.empty() || !model.physics.colliders.empty();
    });
}

u32 DropClothForProfile(Document& document, ProfileId target, Diagnostics& out) {
    if (Profile(target).physics.cloth) {
        return 0;
    }
    u32 dropped = 0;
    for (u32 m = 0; m < document.models.size(); ++m) {
        Model& model = document.models[m];
        if (model.physics.cloths.empty() && model.physics.colliders.empty()) {
            continue;
        }
        std::vector<u32> ids;
        std::vector<SectionRef> cages;
        for (const Cloth& cloth : model.physics.cloths) {
            ids.push_back(cloth.id);
            if (cloth.cage.mesh < model.meshes.size() &&
                cloth.cage.section < model.meshes[cloth.cage.mesh].sections.size()) {
                cages.push_back(cloth.cage);
            }
            for (const ClothBinding& binding : cloth.bindings) {
                if (binding.section.mesh < model.meshes.size() &&
                    binding.section.section < model.meshes[binding.section.mesh].sections.size()) {
                    SectionFlags& flags = model.meshes[binding.section.mesh].sections[binding.section.section].flags;
                    flags = static_cast<SectionFlags>(static_cast<u32>(flags) &
                                                      ~static_cast<u32>(SectionFlags::ClothInfluenced));
                }
            }
            out.info(DiagCode::PhysicsUnsupported,
                     "a cloth left out: the target runs none, so its faces keep their own skin",
                     ElementRef(ElementKind::PhysicsRecord, cloth.id), target);
            ++dropped;
        }
        // Highest section first, so the ones still to go keep their numbers.
        std::sort(cages.begin(), cages.end(), [](const SectionRef& a, const SectionRef& b) {
            return a.mesh != b.mesh ? a.mesh < b.mesh : a.section > b.section;
        });
        cages.erase(std::unique(cages.begin(), cages.end()), cages.end());
        for (const SectionRef& cage : cages) {
            if (!EraseSection(document, m, cage.mesh, cage.section, out)) {
                // Its faces are the whole mesh: left as the hidden section it is.
                out.warn(DiagCode::PhysicsUnsupported, "a cloth's cage is its whole mesh, and stays as a hidden geoset",
                         ElementRef(ElementKind::Mesh, cage.mesh), target);
            }
        }
        // The erase took the cloths whose cages went; the rest go too.
        model.physics.cloths.clear();
        for (const ClothCollider& collider : model.physics.colliders) {
            ids.push_back(collider.id);
        }
        model.physics.colliders.clear();
        std::vector<u32> channels;
        for (const AnimChannel& channel : model.animChannels.channels) {
            if (channel.target.kind == TrackTarget::Kind::Physics &&
                (channel.target.channel == Channel::ClothActive ||
                 channel.target.channel == Channel::ClothDriverTranslation ||
                 channel.target.channel == Channel::ClothDriverRotation ||
                 std::find(ids.begin(), ids.end(), channel.target.sub) != ids.end())) {
                channels.push_back(channel.id);
            }
        }
        EraseChannels(document, m, channels);
        DetachPoseStages(model.poseStages, ids);
        for (Mesh& mesh : model.meshes) {
            for (const char* layer : {geom::names::kClothMovable, geom::names::kClothBindVertex,
                                      geom::names::kClothBindWeight}) {
                mesh.attributes.remove(layer, geom::Domain::Vertex);
            }
        }
    }
    return dropped;
}

bool CarriesSoft(const Document& document) {
    return std::any_of(document.models.begin(), document.models.end(),
                       [](const Model& model) { return !model.physics.soft.empty(); });
}

u32 DropSoftForProfile(Document& document, ProfileId target, Diagnostics& out) {
    u32 dropped = 0;
    for (u32 m = 0; m < document.models.size(); ++m) {
        Model& model = document.models[m];
        // The channels go whether or not a body is left to name them.
        std::vector<u32> channels;
        for (const AnimChannel& channel : model.animChannels.channels) {
            if (channel.target.kind == TrackTarget::Kind::Physics && IsSoftChannel(channel.target.channel)) {
                channels.push_back(channel.id);
            }
        }
        if (!channels.empty()) {
            EraseChannels(document, m, channels);
        }
        for (const SoftBody& body : model.physics.soft) {
            out.info(DiagCode::PhysicsUnsupported, "soft body '" + body.name + "' left out: a file holds its bones and keys",
                     ElementRef(ElementKind::PhysicsRecord, body.id), target);
            ++dropped;
        }
        model.physics.soft.clear();
        for (Mesh& mesh : model.meshes) {
            DropSoftLayers(mesh);
        }
    }
    return dropped;
}

bool IsDeathClipName(std::string_view name) {
    constexpr std::string_view kDeath = "death";
    if (name.size() < kDeath.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kDeath.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != kDeath[i]) {
            return false;
        }
    }
    return name.size() == kDeath.size() || name[kDeath.size()] == ' ';
}

namespace {

/// @p p through @p m, row-vector, as a node's frame places it.
Vector3f Through(const Matrix44f& m, const Vector3f& p) {
    return Vector3f{p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0] + m.data[3][0],
                    p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1] + m.data[3][1],
                    p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2] + m.data[3][2]};
}

/// Where @p body's mass sits in its node's frame, and how much there is: each
/// shape's centre weighed by its density and volume, a hull's or a mesh's by
/// the box round its points.
std::pair<Vector3f, f32> MassCentre(const PhysicsBody& body) {
    constexpr f32 kPi = 3.14159265f;
    Vector3f sum{0, 0, 0};
    f32 total = 0.0f;
    for (const PhysicsShape& shape : body.shapes) {
        const Vector3f origin{shape.transform.data[3][0], shape.transform.data[3][1], shape.transform.data[3][2]};
        Vector3f centre = origin;
        f32 volume = 0.0f;
        switch (shape.kind) {
        case PhysicsShapeKind::Box:
            volume = 8.0f * std::abs(shape.halfExtents.x * shape.halfExtents.y * shape.halfExtents.z);
            break;
        case PhysicsShapeKind::Sphere:
            volume = 4.0f / 3.0f * kPi * shape.radius * shape.radius * shape.radius;
            break;
        case PhysicsShapeKind::Capsule:
        case PhysicsShapeKind::Cylinder: {
            f32 length = shape.length;
            if (shape.kind == PhysicsShapeKind::Capsule && shape.points.size() == 2) {
                centre = (shape.points[0] + shape.points[1]) * 0.5f;
                length = (shape.points[1] - shape.points[0]).length();
            }
            volume = kPi * shape.radius * shape.radius * length +
                     (shape.kind == PhysicsShapeKind::Capsule ? 4.0f / 3.0f * kPi * shape.radius * shape.radius * shape.radius
                                                              : 0.0f);
            break;
        }
        case PhysicsShapeKind::ConvexHull:
        case PhysicsShapeKind::TriangleMesh: {
            const std::vector<Vector3f>& points = shape.kind == PhysicsShapeKind::ConvexHull ? shape.points : shape.vertices;
            if (points.empty()) {
                break;
            }
            Vector3f low = Through(shape.transform, points.front());
            Vector3f high = low;
            Vector3f mean{0, 0, 0};
            for (const Vector3f& p : points) {
                const Vector3f at = Through(shape.transform, p);
                low = Vector3f{std::min(low.x, at.x), std::min(low.y, at.y), std::min(low.z, at.z)};
                high = Vector3f{std::max(high.x, at.x), std::max(high.y, at.y), std::max(high.z, at.z)};
                mean = mean + at;
            }
            centre = mean * (1.0f / static_cast<f32>(points.size()));
            volume = (high.x - low.x) * (high.y - low.y) * (high.z - low.z);
            break;
        }
        case PhysicsShapeKind::Count:
            break;
        }
        const f32 mass = std::max(shape.material.density, 0.0f) * std::max(volume, 0.0f);
        sum = sum + centre * mass;
        total += mass;
    }
    if (!(total > 0.0f)) {
        // No mass to weigh by: the shapes' centres alike.
        for (const PhysicsShape& shape : body.shapes) {
            const Vector3f origin{shape.transform.data[3][0], shape.transform.data[3][1], shape.transform.data[3][2]};
            sum = sum + (shape.kind == PhysicsShapeKind::Capsule && shape.points.size() == 2
                             ? (shape.points[0] + shape.points[1]) * 0.5f
                             : origin);
        }
        return {sum * (1.0f / static_cast<f32>(std::max<std::size_t>(body.shapes.size(), 1))), 0.0f};
    }
    return {sum * (1.0f / total), total};
}

/// `CarryGravityScale` for one model.
u32 CarryModelGravity(Model& model, ProfileId target, f32 gravity, Diagnostics& out) {
    PhysicsSet& physics = model.physics;
    struct Faller {
        u32 body = 0;
        Vector3f at{0, 0, 0}; ///< Its centre of mass at rest, model space.
        f32 mass = 0.0f;
        f32 scale = 1.0f;
    };
    std::vector<Faller> fallers;
    for (u32 b = 0; b < physics.bodies.size(); ++b) {
        const PhysicsBody& body = physics.bodies[b];
        if (body.motion != BodyMotion::Dynamic || body.shapes.empty() || std::abs(body.gravityScale - 1.0f) <= 1e-4f ||
            body.node >= model.nodes.size()) {
            continue;
        }
        // At rest a pivot rig's node stands at its pivot, unturned, as the
        // pose places its shapes; an explicit bind's at its bind.
        Matrix44f frame = Matrix44f::identity();
        if (model.nodes.rig == RigConvention::PivotRelative) {
            frame.data[3][0] = model.nodes.nodes[body.node].pivot.x;
            frame.data[3][1] = model.nodes.nodes[body.node].pivot.y;
            frame.data[3][2] = model.nodes.nodes[body.node].pivot.z;
        } else {
            frame = ToMatrix(model.nodes.worldBind(body.node));
        }
        const auto [centre, mass] = MassCentre(body);
        fallers.push_back({b, Through(frame, centre), std::max(mass, 1e-6f), body.gravityScale});
    }
    if (fallers.empty()) {
        return 0;
    }
    // The Local channels nothing of the model uses yet.
    u32 used = 0;
    for (const PhysicsBody& body : physics.bodies) {
        used |= body.forceChannels & 0xFFFFu;
    }
    for (const Node& node : model.nodes.nodes) {
        if (const ForceFieldPayload* field = std::get_if<ForceFieldPayload>(&node.payload)) {
            used |= field->channels & 0xFFFFu;
        }
    }
    std::vector<u32> free;
    for (u32 bit = 0; bit < 16; ++bit) {
        if ((used & (1u << bit)) == 0) {
            free.push_back(bit);
        }
    }
    if (free.empty()) {
        out.warn(DiagCode::PhysicsGravityScaleDropped,
                 std::to_string(fallers.size()) +
                     " bodies' gravity scale: every Local force channel is taken, so none is left to carry it",
                 ElementRef(ElementKind::PhysicsRecord, physics.bodies[fallers.front().body].id), target);
        return 0;
    }
    // One group a body while the channels last; past them, the two groups
    // that fall most alike and stand nearest go together.
    std::vector<std::vector<u32>> groups;
    for (u32 f = 0; f < fallers.size(); ++f) {
        groups.push_back({f});
    }
    const auto centreOf = [&](const std::vector<u32>& group) {
        Vector3f sum{0, 0, 0};
        f32 mass = 0.0f;
        f32 scale = 0.0f;
        for (const u32 f : group) {
            sum = sum + fallers[f].at * fallers[f].mass;
            scale += fallers[f].scale * fallers[f].mass;
            mass += fallers[f].mass;
        }
        return std::make_pair(sum * (1.0f / mass), scale / mass);
    };
    const f32 height = std::max(ModelHeight(model), 1.0f);
    while (groups.size() > free.size()) {
        std::size_t into = 0;
        std::size_t from = 1;
        f32 best = std::numeric_limits<f32>::max();
        for (std::size_t a = 0; a < groups.size(); ++a) {
            const auto [atA, scaleA] = centreOf(groups[a]);
            for (std::size_t b = a + 1; b < groups.size(); ++b) {
                const auto [atB, scaleB] = centreOf(groups[b]);
                const f32 cost = (atA - atB).length() + 10.0f * height * std::abs(scaleA - scaleB);
                if (cost < best) {
                    best = cost;
                    into = a;
                    from = b;
                }
            }
        }
        groups[into].insert(groups[into].end(), groups[from].begin(), groups[from].end());
        groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(from));
    }
    const auto taken = [&](const std::string& name) {
        return std::any_of(model.nodes.nodes.begin(), model.nodes.nodes.end(),
                           [&](const Node& node) { return node.name == name; });
    };
    u32 carried = 0;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const auto [at, scale] = centreOf(groups[g]);
        const u32 bit = 1u << free[g];
        char stem[32];
        std::snprintf(stem, sizeof stem, "Gravity %.2f", static_cast<double>(scale));
        std::string name = stem;
        for (u32 n = 2; taken(name); ++n) {
            name = std::string(stem) + " " + std::to_string(n);
        }
        Node node;
        node.name = std::move(name);
        node.kind = NodeKind::ForceField;
        node.parent = kInvalidNode;
        node.local.translation = at;
        node.pivot = at;
        ForceFieldPayload field;
        field.kind = ForceKind::Directional;
        field.volume = ForceVolume::Sphere;
        field.unbounded = true;
        field.affectsParticles = false;
        field.affectsBodies = true;
        field.channels = bit;
        field.strength = (1.0f - scale) * gravity;
        // Drawn round the model; unbounded, it pushes everywhere.
        field.width = field.height = field.length = height;
        node.payload = field;
        model.nodes.add(std::move(node));
        for (const u32 f : groups[g]) {
            PhysicsBody& body = physics.bodies[fallers[f].body];
            body.forceChannels |= bit;
            body.gravityScale = 1.0f;
            ++carried;
        }
    }
    out.info(DiagCode::PhysicsGravityScaleDropped,
             std::to_string(carried) + " bodies' gravity scale carried by " + std::to_string(groups.size()) +
                 " force field(s), the game's own being fixed at 1",
             ElementRef(ElementKind::Document, 0), target);
    return static_cast<u32>(groups.size());
}

/// Whether @p joint springs its twist back to rest, which StarCraft II cannot.
bool SpringsTwist(const PhysicsJoint& joint) {
    return joint.kind == JointKind::ConeTwist && joint.motor == JointMotorMode::Position && joint.angularSpring.hz > 0.0f;
}

/// `HoldSprungTwists` for one model.
u32 HoldModelTwists(Model& model, ProfileId target, Diagnostics& out) {
    u32 held = 0;
    for (PhysicsJoint& joint : model.physics.joints) {
        if (!SpringsTwist(joint)) {
            continue;
        }
        joint.lower = joint.limitEnabled ? std::clamp(joint.lower, -kHeldTwist, kHeldTwist) : -kHeldTwist;
        joint.upper = joint.limitEnabled ? std::clamp(joint.upper, -kHeldTwist, kHeldTwist) : kHeldTwist;
        if (joint.upper < joint.lower) {
            joint.lower = joint.upper = 0.0f;
        }
        joint.limitEnabled = true;
        joint.motor = JointMotorMode::Off;
        joint.maxMotorForce = 0.0f;
        joint.angularSpring = JointSpring{};
        ++held;
    }
    if (held != 0) {
        out.info(DiagCode::PhysicsJointFieldDropped,
                 std::to_string(held) + " shoulder joint(s) whose motor springs the twist back have it held, the "
                                        "game having no joint spring",
                 ElementRef(ElementKind::Document, 0), target);
    }
    return held;
}

/// Whether @p target fixes a body's gravity scale at 1, and @p model has a
/// dynamic body it is not.
bool ScalesGravity(const Model& model, ProfileId target) {
    return GameOf(target) == Game::StarCraft && Profile(target).physics.any() &&
           std::any_of(model.physics.bodies.begin(), model.physics.bodies.end(), [](const PhysicsBody& body) {
               return body.motion == BodyMotion::Dynamic && std::abs(body.gravityScale - 1.0f) > 1e-4f;
           });
}

/// Whether @p target has no joint spring (StarCraft II) and @p model a
/// shoulder that springs its twist.
bool SpringsTwists(const Model& model, ProfileId target) {
    return GameOf(target) == Game::StarCraft && Profile(target).physics.any() &&
           std::any_of(model.physics.joints.begin(), model.physics.joints.end(), SpringsTwist);
}

} // namespace

u32 HoldSprungTwists(Document& document, ProfileId target, Diagnostics& out) {
    u32 held = 0;
    for (Model& model : document.models) {
        if (SpringsTwists(model, target)) {
            held += HoldModelTwists(model, target, out);
        }
    }
    return held;
}

u32 CarryGravityScale(Document& document, ProfileId target, f32 gravity, Diagnostics& out) {
    u32 made = 0;
    for (Model& model : document.models) {
        if (ScalesGravity(model, target)) {
            made += CarryModelGravity(model, target, gravity, out);
        }
    }
    return made;
}

bool NeedsPhysicsFit(const Document& document, ProfileId target) {
    const PhysicsCaps& caps = Profile(target).physics;
    if (!caps.any()) {
        return false;
    }
    for (const Model& model : document.models) {
        if (ScalesGravity(model, target) || SpringsTwists(model, target)) {
            return true;
        }
        for (const PhysicsRig& rig : model.physics.rigs) {
            if (rig.start != RigStart::Animated) {
                return true;
            }
        }
        // A drop or a Blend to fold, or switches a target without them reports.
        for (const AnimChannel& channel : model.animChannels.channels) {
            if (channel.target.kind == TrackTarget::Kind::Physics &&
                (channel.target.channel == Channel::PhysicsRagdoll || channel.target.channel == Channel::PhysicsBlend ||
                 (!caps.switches && channel.target.channel == Channel::PhysicsDynamic))) {
                return true;
            }
        }
        for (const Cloth& cloth : model.physics.cloths) {
            if (caps.cloth && caps.maxClothParticles != 0 && cloth.cage.mesh < model.meshes.size() &&
                CountOf(VerticesOf(model.meshes[cloth.cage.mesh], cloth.cage.section)) > caps.maxClothParticles) {
                return true;
            }
        }
    }
    return false;
}

u32 FitPhysicsToProfile(Document& document, ProfileId target, Diagnostics& out) {
    const PhysicsCaps& caps = Profile(target).physics;
    if (!caps.any()) {
        return 0;
    }
    u32 changed = 0;
    // A cylinder the target lacks goes as the prism the host's engine runs it
    // as, where the target has hulls (World of Warcraft).
    const auto carries = [&](PhysicsShapeKind kind) { return (caps.shapeKinds & (1u << static_cast<u32>(kind))) != 0; };
    const bool prism = !carries(PhysicsShapeKind::Cylinder) && carries(PhysicsShapeKind::ConvexHull);
    for (u32 m = 0; m < document.models.size(); ++m) {
        Model& model = document.models[m];
        for (PhysicsBody& body : model.physics.bodies) {
            for (PhysicsShape& shape : body.shapes) {
                if (!prism || shape.kind != PhysicsShapeKind::Cylinder) {
                    continue;
                }
                shape.points = CylinderPrism(shape);
                shape.kind = PhysicsShapeKind::ConvexHull;
                shape.radius = 0.0f;
                shape.length = 0.0f;
                ++changed;
                out.info(DiagCode::PhysicsUnsupported, "a cylinder carried as a hull of its 16-sided prism",
                         ElementRef(ElementKind::PhysicsRecord, body.id), target);
            }
        }
        if (caps.switches && !model.physics.bodies.empty()) {
            // The rule read once, before any body goes, then written out.
            std::vector<u32> ids;
            for (const PhysicsBody& body : model.physics.bodies) {
                ids.push_back(body.id);
            }
            const std::vector<ClipStates> states = RuleStates(document, m);
            if (!model.physics.rigs.empty()) {
                changed += ChooseSwitchedRigs(model, target, caps, states, out);
            }
            SwitchesFromRule(document, m, ids, states, target, out);
            // In the units the export writes, the rescale coming after.
            if (ScalesGravity(model, target)) {
                const f32 factor = RescaleFactorBetween(document.defaultProfile, target);
                changed += CarryModelGravity(model, target,
                                             PhysicsHostFor(GameOf(target)).gravity.length() / std::max(factor, 1e-6f), out);
            }
            if (SpringsTwists(model, target)) {
                changed += HoldModelTwists(model, target, out);
            }
        } else if (!model.physics.rigs.empty()) {
            changed += ChooseRigs(model, target, caps, out);
        }
        if (!caps.switches) {
            ReportUnswitched(document, m, target, out);
        }
        if (!caps.cloth || caps.maxClothParticles == 0) {
            continue;
        }
        for (const Cloth& cloth : model.physics.cloths) {
            if (cloth.cage.mesh >= model.meshes.size()) {
                continue;
            }
            Mesh& mesh = model.meshes[cloth.cage.mesh];
            const u32 before = CountOf(VerticesOf(mesh, cloth.cage.section));
            if (before <= caps.maxClothParticles) {
                continue;
            }
            const u32 after = DecimateCage(mesh, cloth.cage.section, caps.maxClothParticles);
            ++changed;
            const ElementRef where(ElementKind::PhysicsRecord, cloth.id);
            if (after > caps.maxClothParticles) {
                out.warn(DiagCode::ClothParticleLimit,
                         "a cage of " + std::to_string(before) + " particles decimated only to " +
                             std::to_string(after) + ", past the target's " + std::to_string(caps.maxClothParticles),
                         where, target);
            } else {
                out.info(DiagCode::ClothParticleLimit,
                         "a cage of " + std::to_string(before) + " particles decimated to " + std::to_string(after),
                         where, target);
            }
        }
    }
    return changed;
}

} // namespace wem
} // namespace models
} // namespace whiteout
