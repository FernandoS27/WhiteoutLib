// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/crossing.h>

#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/physics/references.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
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

/// Leaves only the rigs @p caps keeps, dropping the bodies no kept rig holds.
u32 ChooseRigs(Model& model, ProfileId target, const PhysicsCaps& caps, Diagnostics& out) {
    PhysicsSet& physics = model.physics;
    const bool hasOnDeath = std::any_of(physics.rigs.begin(), physics.rigs.end(), [](const PhysicsRig& rig) {
        return rig.start == RigStart::OnDeath && !rig.bodies.empty();
    });
    std::set<u32> kept;
    for (const PhysicsRig& rig : physics.rigs) {
        if (Keeps(caps, rig.start, hasOnDeath)) {
            kept.insert(rig.bodies.begin(), rig.bodies.end());
        }
    }
    u32 changed = 0;
    std::vector<u32> gone;
    for (const PhysicsRig& rig : physics.rigs) {
        if (Keeps(caps, rig.start, hasOnDeath)) {
            continue;
        }
        ++changed;
        out.warn(DiagCode::PhysicsRigDropped,
                 "rig '" + rig.name + "': " +
                     (caps.switches ? "the model's death rig is the one the target switches on"
                                    : "the target simulates from creation and cannot start it later"),
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

/// A rig's start as the target's switches: an `OnDeath` rig's bodies on from
/// the start of every *Death* clip of @p m, off elsewhere.
void StartsToSwitches(Document& document, u32 m) {
    Model& model = document.models[m];
    const f32 on = 1.0f;
    for (const PhysicsRig& rig : model.physics.rigs) {
        for (const u32 id : rig.bodies) {
            PhysicsBody* body = model.physics.body(id);
            if (body == nullptr) {
                continue;
            }
            switch (rig.start) {
            case RigStart::Always:
                body->simulates = true;
                break;
            case RigStart::Never:
                body->simulates = false;
                break;
            case RigStart::OnDeath: {
                body->simulates = false;
                const u32 channel = PhysicsSwitchChannel(model, id, Channel::PhysicsDynamic);
                for (Clip& clip : document.clips) {
                    if (clip.model != m || clip.containers.empty() || !IsDeathClipName(clip.name)) {
                        continue;
                    }
                    for (SubTrackContainer& container : clip.containers) {
                        std::erase_if(container.subTracks,
                                      [&](const SubTrack& track) { return track.channel == channel; });
                    }
                    SubTrack track;
                    track.channel = channel;
                    track.interp = Interpolation::Step;
                    track.times.push_back(0.0f);
                    track.values.resize(sizeof(f32));
                    std::memcpy(track.values.data(), &on, sizeof(f32));
                    clip.containers.front().subTracks.push_back(std::move(track));
                }
                break;
            }
            case RigStart::Animated:
            case RigStart::Count:
                break;
            }
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

u32 PhysicsSwitchChannel(Model& model, u32 record, Channel channel) {
    for (const AnimChannel& entry : model.animChannels.channels) {
        if (entry.target.kind == TrackTarget::Kind::Physics && entry.target.sub == record &&
            entry.target.channel == channel) {
            return entry.id;
        }
    }
    AnimChannel made;
    made.id = model.animChannels.nextFreeId();
    made.target.kind = TrackTarget::Kind::Physics;
    made.target.sub = record;
    made.target.channel = channel;
    made.valueType = geom::AttrType::F32;
    return model.animChannels.add(made);
}

bool NeedsPhysicsFit(const Document& document, ProfileId target) {
    const PhysicsCaps& caps = Profile(target).physics;
    if (!caps.any()) {
        return false;
    }
    for (const Model& model : document.models) {
        for (const PhysicsRig& rig : model.physics.rigs) {
            if (rig.start != RigStart::Animated) {
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
    for (u32 m = 0; m < document.models.size(); ++m) {
        Model& model = document.models[m];
        if (!model.physics.rigs.empty()) {
            changed += ChooseRigs(model, target, caps, out);
            if (caps.switches) {
                StartsToSwitches(document, m);
            }
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
