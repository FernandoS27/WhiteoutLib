// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/references.h>

#include <whiteout/models/wem/nodes/references.h>
#include <whiteout/models/wem/physics/cloth_bake.h>
#include <whiteout/models/wem/physics/soft_region.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {

namespace {

std::string number(u64 value) {
    return std::to_string(value);
}

u32 Remapped(std::span<const u32> remap, u32 value, u32 none) {
    return value < remap.size() ? remap[value] : none;
}

/// Drops the joints on @p gone bodies and the cloths' ids of @p gone colliders.
void Sweep(PhysicsSet& physics, const std::set<u32>& gone, std::vector<u32>& removed) {
    std::erase_if(physics.joints, [&](const PhysicsJoint& joint) {
        const bool drop = gone.count(joint.bodyA) != 0 || gone.count(joint.bodyB) != 0;
        if (drop) {
            removed.push_back(joint.id);
        }
        return drop;
    });
    for (Cloth& cloth : physics.cloths) {
        std::erase_if(cloth.colliders, [&](u32 id) { return gone.count(id) != 0; });
    }
    for (SoftBody& soft : physics.soft) {
        std::erase_if(soft.colliders, [&](u32 id) { return gone.count(id) != 0; });
    }
    for (PhysicsRig& rig : physics.rigs) {
        std::erase_if(rig.bodies, [&](u32 id) { return gone.count(id) != 0; });
    }
    for (PhysicsBody& body : physics.bodies) {
        if (body.wow.has_value() && gone.count(body.wow->parent) != 0) {
            body.wow->parent = 0;
        }
    }
}

/// Whether @p kind has a limit range: a twist, an angle or a slide.
bool HasRange(JointKind kind) {
    return kind == JointKind::ConeTwist || kind == JointKind::Revolute || kind == JointKind::Prismatic;
}

void ScaleTranslation(Matrix44f& m, f32 factor) {
    m.data[3][0] *= factor;
    m.data[3][1] *= factor;
    m.data[3][2] *= factor;
}

Vector3f Row(const Matrix44f& m, int r) {
    return Vector3f{m.data[r][0], m.data[r][1], m.data[r][2]};
}

/// One byte per vertex: whether a face of @p section uses it.
std::vector<u8> VerticesOfSection(const Mesh& mesh, u32 section) {
    const geom::FaceSet& faces = mesh.faceSet();
    const std::span<const u32> sections = mesh.faceSections();
    std::vector<u8> used(faces.vertexCount, 0);
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceValence.size(); ++f) {
        const u32 valence = faces.faceValence[f];
        if (f < sections.size() && sections[f] == section) {
            for (u32 c = 0; c < valence && corner + c < faces.cornerVertex.size(); ++c) {
                const u32 v = faces.cornerVertex[corner + c];
                if (v < used.size()) {
                    used[v] = 1;
                }
            }
        }
        corner += valence;
    }
    return used;
}

/// The joint frame's origin in model space at rest, through its body's node.
Vector3f RestOrigin(const Model& model, const PhysicsBody& body, const Matrix44f& frame) {
    if (body.node >= model.nodes.size()) {
        return Row(frame, 3);
    }
    // A record's frames sit in its node's frame at rest (WEM_PHYSICS_DESIGN.md
    // §3): a pivot rig's node stands at its pivot, unturned; an explicit-bind
    // one at its bind.
    if (model.nodes.rig == RigConvention::PivotRelative) {
        const Vector3f pivot = model.nodes.nodes[body.node].pivot;
        const Vector3f at = Row(frame, 3);
        return Vector3f{at.x + pivot.x, at.y + pivot.y, at.z + pivot.z};
    }
    const Transform world = model.nodes.worldBind(body.node);
    return TransformPoint(world, Row(frame, 3));
}

/// Whether @p points lie in one plane (or a line), to a millionth of their
/// spread: a hull with no volume, which no game cooks.
bool Flat(const std::vector<Vector3f>& points) {
    if (points.size() < 4) {
        return true;
    }
    const Vector3f o = points[0];
    Vector3f far = o;
    for (const Vector3f& p : points) {
        if ((p - o).length() > (far - o).length()) {
            far = p;
        }
    }
    const f32 spread = (far - o).length();
    if (spread <= 0.0f) {
        return true;
    }
    const Vector3f line = (far - o) * (1.0f / spread);
    Vector3f third = o;
    f32 off = 0.0f;
    for (const Vector3f& p : points) {
        const Vector3f d = p - o;
        const f32 away = (d - line * d.dot(line)).length();
        if (away > off) {
            off = away;
            third = p;
        }
    }
    const Vector3f normal = cross(far - o, third - o);
    if (normal.length() <= 1e-12f * spread * spread) {
        return true;
    }
    const Vector3f n = normal.normalized();
    f32 low = 0.0f, high = 0.0f;
    for (const Vector3f& p : points) {
        const f32 h = (p - o).dot(n);
        low = std::min(low, h);
        high = std::max(high, h);
    }
    return high - low <= 1e-6f * spread;
}

} // namespace

std::vector<u32> RemapPhysicsNodes(PhysicsSet& physics, std::span<const u32> remap) {
    // A body or a collider on no node -- the root is none -- stays as it is;
    // one whose node is gone goes with it.
    std::unordered_set<const u32*> orphaned;
    ForEachPhysicsNodeReference(physics, [&](NodeReference row, u32& node, u32, u32) {
        const bool rode = node != kInvalidNode;
        node = Remapped(remap, node, kInvalidNode);
        if (rode && node == kInvalidNode &&
            (row == NodeReference::PhysicsBody || row == NodeReference::ClothCollider)) {
            orphaned.insert(&node);
        }
    });
    std::vector<u32> removed;
    std::set<u32> gone;
    std::erase_if(physics.bodies, [&](const PhysicsBody& body) {
        if (orphaned.count(&body.node) == 0) {
            return false;
        }
        gone.insert(body.id);
        removed.push_back(body.id);
        return true;
    });
    std::erase_if(physics.colliders, [&](const ClothCollider& collider) {
        if (orphaned.count(&collider.node) == 0) {
            return false;
        }
        gone.insert(collider.id);
        removed.push_back(collider.id);
        return true;
    });
    // A cloth or fracture bone that is gone leaves its list; the record stays.
    // A fracture's skin nodes keep their places: its layers name them by index.
    for (Cloth& cloth : physics.cloths) {
        if (cloth.recipe) {
            std::erase(cloth.recipe->bones, kInvalidNode);
        }
    }
    for (PhysicsRig& rig : physics.rigs) {
        if (rig.fracture) {
            std::erase(rig.fracture->bones, kInvalidNode);
        }
    }
    for (SoftBody& body : physics.soft) {
        // A bone's own lump whose bone is gone keeps its place in the list,
        // because the layers name lumps by number: it is a surface lump that
        // hangs from nothing, so no later binding gives it a lane.
        for (SoftLump& lump : body.lumps) {
            if (lump.kind != SoftLumpKind::Surface && lump.node == kInvalidNode) {
                lump.kind = SoftLumpKind::Surface;
                lump.carrier = kInvalidNode;
            }
        }
        for (SoftEffect& effect : body.effects) {
            if (effect.bulge) {
                std::erase(effect.bulge->chain, kInvalidNode);
            }
        }
    }
    Sweep(physics, gone, removed);
    return removed;
}

void SoftFollowRemovedNodes(PhysicsSet& physics, std::span<const u8> doomed, std::span<const u32> replacement) {
    const auto goes = [&](u32 node) { return node < doomed.size() && doomed[node] != 0; };
    const auto heir = [&](u32 node) { return node < replacement.size() ? replacement[node] : kInvalidNode; };
    for (SoftBody& body : physics.soft) {
        for (SoftLump& lump : body.lumps) {
            if (lump.kind != SoftLumpKind::Surface && goes(lump.node)) {
                // Its points' weight goes to the heir's family, whose own lump
                // takes it: this one stays in the list with nothing to carry,
                // and hangs from nothing, so no later binding gives it a lane.
                lump.kind = SoftLumpKind::Surface;
                lump.carrier = kInvalidNode;
                lump.node = kInvalidNode;
            } else if (goes(lump.carrier)) {
                lump.carrier = heir(lump.carrier);
            }
        }
    }
}

std::vector<u32> RemovePhysicsBodies(PhysicsSet& physics, std::span<const u32> ids) {
    const std::set<u32> gone(ids.begin(), ids.end());
    std::vector<u32> removed;
    std::erase_if(physics.bodies, [&](const PhysicsBody& body) {
        const bool drop = gone.count(body.id) != 0;
        if (drop) {
            removed.push_back(body.id);
        }
        return drop;
    });
    Sweep(physics, gone, removed);
    return removed;
}

std::vector<u32> RemapPhysicsMeshes(PhysicsSet& physics, std::span<const u32> meshRemap) {
    std::vector<u32> removed;
    for (PhysicsRig& rig : physics.rigs) {
        if (!rig.fracture) {
            continue;
        }
        for (u32& mesh : rig.fracture->sources) {
            mesh = Remapped(meshRemap, mesh, kInvalidIndex);
        }
        for (u32& mesh : rig.fracture->made) {
            mesh = Remapped(meshRemap, mesh, kInvalidIndex);
        }
    }
    std::erase_if(physics.cloths, [&](Cloth& cloth) {
        cloth.cage.mesh = Remapped(meshRemap, cloth.cage.mesh, kInvalidIndex);
        std::erase_if(cloth.bindings, [&](ClothBinding& binding) {
            binding.section.mesh = Remapped(meshRemap, binding.section.mesh, kInvalidIndex);
            return binding.section.mesh == kInvalidIndex;
        });
        if (cloth.cage.mesh == kInvalidIndex) {
            removed.push_back(cloth.id);
            return true;
        }
        return false;
    });
    return removed;
}

namespace {

/// Part @p index of @p body's riders is let go. Its lumps keep their places in
/// the list, hanging from nothing with nothing to carry, and the lumps of the
/// parts after it name them by their new places.
void DropRider(SoftBody& body, std::size_t index) {
    body.riders.erase(body.riders.begin() + static_cast<std::ptrdiff_t>(index));
    for (SoftLump& lump : body.lumps) {
        if (lump.rider == index + 1) {
            lump.rider = 0;
            lump.carrier = kInvalidNode;
        } else if (lump.rider > index + 1) {
            --lump.rider;
        }
    }
}

} // namespace

std::vector<u32> RemapSoftMeshes(PhysicsSet& physics, std::span<const u32> meshRemap) {
    std::vector<u32> removed;
    std::erase_if(physics.soft, [&](SoftBody& body) {
        for (u32& mesh : body.meshes) {
            mesh = Remapped(meshRemap, mesh, kInvalidIndex);
        }
        std::erase(body.meshes, kInvalidIndex);
        std::erase_if(body.inside, [&](SoftPart& part) {
            part.mesh = Remapped(meshRemap, part.mesh, kInvalidIndex);
            return part.mesh == kInvalidIndex;
        });
        for (std::size_t r = body.riders.size(); r-- > 0;) {
            body.riders[r].mesh = Remapped(meshRemap, body.riders[r].mesh, kInvalidIndex);
            if (body.riders[r].mesh == kInvalidIndex) {
                DropRider(body, r);
            }
        }
        if (body.drips != kInvalidIndex) {
            body.drips = Remapped(meshRemap, body.drips, kInvalidIndex);
        }
        for (SoftEffect& effect : body.effects) {
            if (effect.place.mesh != kInvalidIndex) {
                effect.place.mesh = Remapped(meshRemap, effect.place.mesh, kInvalidIndex);
            }
            if (effect.jiggle) {
                for (u32& mesh : effect.jiggle->kickMesh) {
                    mesh = Remapped(meshRemap, mesh, kInvalidIndex);
                }
            }
            if (effect.drip) {
                for (u32& mesh : effect.drip->siteMesh) {
                    mesh = Remapped(meshRemap, mesh, kInvalidIndex);
                }
            }
        }
        if (body.meshes.empty()) {
            removed.push_back(body.id);
            return true;
        }
        return false;
    });
    return removed;
}

void DropSoftLayers(Mesh& mesh) {
    for (const char* layer : {geom::names::kSoftBody, geom::names::kSoftSoftness, geom::names::kSoftPainted,
                              geom::names::kSoftJoined, geom::names::kSoftBindLump, geom::names::kSoftBindWeight}) {
        mesh.attributes.remove(layer, geom::Domain::Vertex);
    }
}

u32 RemapSoftSlots(Model& model, u32 slot, u32 replacement) {
    const auto follow = [&](u32& material) {
        if (material == kInvalidIndex) {
            return;
        }
        if (material == slot) {
            material = replacement;
        }
        if (material != kInvalidIndex && material >= slot) {
            material = material == slot ? kInvalidIndex : material - 1;
        }
    };
    // Whether @p mesh draws faces with @p material as it stands.
    const auto draws = [&](u32 mesh, u32 material) {
        return mesh < model.meshes.size() &&
               std::any_of(model.meshes[mesh].sections.begin(), model.meshes[mesh].sections.end(),
                           [&](const MeshSection& section) { return section.materialSlot == material; });
    };
    u32 letGo = 0;
    for (SoftBody& body : model.physics.soft) {
        // What the body keeps clear of follows its faces to the replacement,
        // with whatever else draws with it: more to keep clear of, never less.
        // A part of a slot nothing replaces drew no face.
        std::erase_if(body.inside, [&](SoftPart& part) {
            const bool gone = part.material == slot && replacement == kInvalidIndex;
            follow(part.material);
            return gone;
        });
        // A part that rides is its own faces and no others. Where the
        // replacement already draws faces of its mesh the two cannot be told
        // apart, whichever of the two the part was, and it is let go rather
        // than grown.
        for (std::size_t r = body.riders.size(); r-- > 0;) {
            SoftPart& part = body.riders[r];
            const bool joined = replacement != kInvalidIndex && part.material == replacement && draws(part.mesh, slot);
            if (joined || (part.material == slot && (replacement == kInvalidIndex || draws(part.mesh, replacement)))) {
                DropRider(body, r);
                ++letGo;
                continue;
            }
            follow(part.material);
        }
        for (SoftEffect& effect : body.effects) {
            if (effect.drip) {
                follow(effect.drip->material);
            }
        }
    }
    return letGo;
}

namespace {

/// The corners of @p face of @p mesh, as vertices; empty where it has none.
std::span<const u32> CornersOf(const geom::FaceSet& faces, const std::vector<std::size_t>& first, u32 face) {
    if (face >= faces.faceCount() || faces.faceValence[face] < 3) {
        return {};
    }
    return std::span<const u32>(faces.cornerVertex.data() + first[face], faces.faceValence[face]);
}

/// Each face's first corner in @p faces' run of corners.
std::vector<std::size_t> FirstCorners(const geom::FaceSet& faces) {
    std::vector<std::size_t> first(faces.faceCount(), 0);
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        first[f] = corner;
        corner += faces.faceValence[f];
    }
    return first;
}

/// The point of triangle a, b, c nearest @p p.
Vector3f NearestOnTriangle(const Vector3f& p, const Vector3f& a, const Vector3f& b, const Vector3f& c) {
    const Vector3f ab = b - a, ac = c - a, ap = p - a;
    const f32 d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }
    const Vector3f bp = p - b;
    const f32 d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return b;
    }
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return a + ab * (d1 / (d1 - d3));
    }
    const Vector3f cp = p - c;
    const f32 d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return c;
    }
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return a + ac * (d2 / (d2 - d6));
    }
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const f32 sum = va + vb + vc;
    if (!(std::abs(sum) > 0.0f)) {
        return a;
    }
    return a + ab * (vb / sum) + ac * (vc / sum);
}

/// @p p's weights on a, b, c, in their plane: past an edge they leave 0 to 1,
/// which is how a place on a polygon's later triangles is said.
Vector3f WeightsOn(const Vector3f& p, const Vector3f& a, const Vector3f& b, const Vector3f& c) {
    const Vector3f ab = b - a, ac = c - a, ap = p - a;
    const f32 d00 = ab.dot(ab), d01 = ab.dot(ac), d11 = ac.dot(ac);
    const f32 d20 = ap.dot(ab), d21 = ap.dot(ac);
    const f32 det = d00 * d11 - d01 * d01;
    if (!(std::abs(det) > 1e-30f)) {
        return Vector3f{1, 0, 0};
    }
    const f32 v = (d11 * d20 - d01 * d21) / det;
    const f32 w = (d00 * d21 - d01 * d20) / det;
    return Vector3f{1.0f - v - w, v, w};
}

} // namespace

u32 SoftFollowRenumberedFaces(PhysicsSet& physics, u32 mesh, const Mesh& before, const Mesh& after) {
    const geom::FaceSet& was = before.faceSet();
    const geom::FaceSet& now = after.faceSet();
    const std::vector<std::size_t> wasFirst = FirstCorners(was), nowFirst = FirstCorners(now);
    const std::span<const Vector3f> wasAt = before.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const std::span<const Vector3f> nowAt = after.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const std::span<const u32> bodyOf = after.attributes.get<u32>(geom::names::kSoftBody, geom::Domain::Vertex);
    // Where a place stands on a mesh: its weights on its face's first triangle.
    const auto pointOn = [](const geom::FaceSet& faces, const std::vector<std::size_t>& first,
                            std::span<const Vector3f> positions, u32 face, const Vector3f& weights, Vector3f& out) {
        const std::span<const u32> corners = CornersOf(faces, first, face);
        if (corners.empty() || corners[0] >= positions.size() || corners[1] >= positions.size() ||
            corners[2] >= positions.size()) {
            return false;
        }
        out = positions[corners[0]] * weights.x + positions[corners[1]] * weights.y + positions[corners[2]] * weights.z;
        return true;
    };
    // The mesh's size, for what "the same spot" means.
    Vector3f low{0, 0, 0}, high{0, 0, 0};
    for (std::size_t v = 0; v < nowAt.size(); ++v) {
        const Vector3f& p = nowAt[v];
        low = v == 0 ? p : Vector3f{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = v == 0 ? p : Vector3f{std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    const f32 same = 1e-5f * std::max((high - low).length(), 1e-6f);
    u32 written = 0;
    const auto follow = [&](u32 body, u32& placedOn, u32& face, Vector3f& weights) {
        if (placedOn != mesh || face == kInvalidIndex) {
            return;
        }
        Vector3f stood;
        if (!pointOn(was, wasFirst, wasAt, face, weights, stood)) {
            return;
        }
        // Still there under its number: an edit elsewhere on the mesh.
        Vector3f stands;
        if (pointOn(now, nowFirst, nowAt, face, weights, stands) && (stands - stood).length() <= same) {
            return;
        }
        // The nearest face, one of the body's where there is any: a corner of
        // its first triangle is a point of the body.
        f32 least = std::numeric_limits<f32>::max();
        u32 best = kInvalidIndex;
        Vector3f on{0, 0, 0};
        for (int pass = 0; pass < 2 && best == kInvalidIndex; ++pass) {
            for (u32 f = 0; f < now.faceCount(); ++f) {
                const std::span<const u32> corners = CornersOf(now, nowFirst, f);
                if (corners.empty()) {
                    continue;
                }
                bool sound = true, held = false;
                for (const u32 v : corners) {
                    sound = sound && v < nowAt.size();
                }
                for (std::size_t k = 0; sound && k < 3; ++k) {
                    held = held || (corners[k] < bodyOf.size() && bodyOf[corners[k]] == body);
                }
                if (!sound || (pass == 0 && !held)) {
                    continue;
                }
                for (std::size_t k = 1; k + 1 < corners.size(); ++k) {
                    const Vector3f near =
                        NearestOnTriangle(stood, nowAt[corners[0]], nowAt[corners[k]], nowAt[corners[k + 1]]);
                    const f32 apart = (near - stood).length();
                    if (apart < least) {
                        least = apart;
                        best = f;
                        on = near;
                    }
                }
            }
        }
        ++written;
        if (best == kInvalidIndex) {
            // No face is left to stand on: it has no place, which every reader takes.
            placedOn = kInvalidIndex;
            face = kInvalidIndex;
            return;
        }
        const std::span<const u32> corners = CornersOf(now, nowFirst, best);
        face = best;
        weights = WeightsOn(on, nowAt[corners[0]], nowAt[corners[1]], nowAt[corners[2]]);
    };
    for (SoftBody& body : physics.soft) {
        for (SoftEffect& effect : body.effects) {
            follow(body.id, effect.place.mesh, effect.place.face, effect.place.weights);
            if (effect.jiggle) {
                SoftJiggle& j = *effect.jiggle;
                for (std::size_t k = 0; k < j.kickMesh.size() && k < j.kickFace.size() && k < j.kickWeights.size(); ++k) {
                    follow(body.id, j.kickMesh[k], j.kickFace[k], j.kickWeights[k]);
                }
            }
            if (effect.drip) {
                SoftDrip& d = *effect.drip;
                for (std::size_t k = 0; k < d.siteMesh.size() && k < d.siteFace.size() && k < d.siteWeights.size(); ++k) {
                    follow(body.id, d.siteMesh[k], d.siteFace[k], d.siteWeights[k]);
                }
            }
        }
    }
    return written;
}

u32 SoftFollowMergedMeshes(PhysicsSet& physics, std::span<const u8> merged, u32 keep, std::span<const u32> faceBase) {
    const auto in = [&](u32 mesh) { return mesh < merged.size() && merged[mesh] != 0; };
    const auto goes = [&](u32 mesh) { return in(mesh) && mesh != keep; };
    // A place on an absorbed mesh: its face stands after the faces before it.
    const auto place = [&](u32& mesh, u32& face) {
        if (goes(mesh)) {
            face = face != kInvalidIndex && mesh < faceBase.size() ? faceBase[mesh] + face : face;
            mesh = keep;
        }
    };
    u32 letGo = 0;
    for (SoftBody& body : physics.soft) {
        for (u32& mesh : body.meshes) {
            mesh = goes(mesh) ? keep : mesh;
        }
        std::sort(body.meshes.begin(), body.meshes.end());
        body.meshes.erase(std::unique(body.meshes.begin(), body.meshes.end()), body.meshes.end());
        // The merged mesh draws with one material: what the body keeps clear
        // of is all of it that is not its own, and a part that rode can no
        // longer be told from the faces it was merged with.
        bool whole = false;
        std::erase_if(body.inside, [&](SoftPart& part) {
            if (!in(part.mesh)) {
                return false;
            }
            part = SoftPart{keep, kInvalidIndex};
            return std::exchange(whole, true);
        });
        for (std::size_t r = body.riders.size(); r-- > 0;) {
            if (in(body.riders[r].mesh)) {
                DropRider(body, r);
                ++letGo;
            }
        }
        for (SoftEffect& effect : body.effects) {
            place(effect.place.mesh, effect.place.face);
            if (effect.jiggle) {
                SoftJiggle& jiggle = *effect.jiggle;
                for (std::size_t k = 0; k < jiggle.kickMesh.size() && k < jiggle.kickFace.size(); ++k) {
                    place(jiggle.kickMesh[k], jiggle.kickFace[k]);
                }
            }
            if (effect.drip) {
                SoftDrip& drip = *effect.drip;
                for (std::size_t k = 0; k < drip.siteMesh.size() && k < drip.siteFace.size(); ++k) {
                    place(drip.siteMesh[k], drip.siteFace[k]);
                }
            }
        }
    }
    return letGo;
}

std::vector<u32> RemapPhysicsSections(PhysicsSet& physics, u32 mesh, std::span<const u32> sectionRemap) {
    std::vector<u32> removed;
    std::erase_if(physics.cloths, [&](Cloth& cloth) {
        // The recipe's sections of origin run beside the bindings, and go with them.
        std::vector<u32>* from = cloth.recipe && cloth.recipe->from.size() == cloth.bindings.size()
                                     ? &cloth.recipe->from
                                     : nullptr;
        for (std::size_t b = cloth.bindings.size(); b-- > 0;) {
            ClothBinding& binding = cloth.bindings[b];
            if (binding.section.mesh != mesh) {
                continue;
            }
            binding.section.section = Remapped(sectionRemap, binding.section.section, kInvalidIndex);
            if (from != nullptr) {
                (*from)[b] = Remapped(sectionRemap, (*from)[b], kInvalidIndex);
            }
            if (binding.section.section == kInvalidIndex) {
                cloth.bindings.erase(cloth.bindings.begin() + static_cast<std::ptrdiff_t>(b));
                if (from != nullptr) {
                    from->erase(from->begin() + static_cast<std::ptrdiff_t>(b));
                }
            }
        }
        if (cloth.cage.mesh != mesh) {
            return false;
        }
        if (cloth.recipe && cloth.recipe->cageFrom != kInvalidIndex) {
            cloth.recipe->cageFrom = Remapped(sectionRemap, cloth.recipe->cageFrom, kInvalidIndex);
        }
        cloth.cage.section = Remapped(sectionRemap, cloth.cage.section, kInvalidIndex);
        if (cloth.cage.section == kInvalidIndex) {
            removed.push_back(cloth.id);
            return true;
        }
        return false;
    });
    return removed;
}

void InvalidatePhysicsChannels(AnimChannelTable& channels, std::span<const u32> removed, Diagnostics& out) {
    if (removed.empty()) {
        return;
    }
    u32 invalidated = 0;
    for (AnimChannel& channel : channels.channels) {
        if (channel.target.kind != TrackTarget::Kind::Physics || channel.target.sub == 0) {
            continue;
        }
        // A soft body's channels carry an effect or a lump above its id.
        const u32 record = IsSoftChannel(channel.target.channel) ? SoftBodyOfSub(channel.target.sub)
                                                                 : channel.target.sub;
        if (std::find(removed.begin(), removed.end(), record) != removed.end()) {
            channel.target.sub = 0;
            ++invalidated;
        }
    }
    if (invalidated != 0) {
        out.warn(DiagCode::AnimChannelInvalidated,
                 number(invalidated) + " physics channels named a record that is gone", ElementRef());
    }
}

void DetachPoseStages(std::span<PoseStage> stages, std::span<const u32> removed) {
    const auto gone = [&](u32 id) { return std::find(removed.begin(), removed.end(), id) != removed.end(); };
    for (PoseStage& stage : stages) {
        stage.rig = gone(stage.rig) ? 0 : stage.rig;
        stage.cloth = gone(stage.cloth) ? 0 : stage.cloth;
    }
}

Matrix44f AffineInverse(const Matrix44f& m) {
    const auto& a = m.data;
    const f32 c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    const f32 c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    const f32 c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    const f32 det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    Matrix44f out = Matrix44f::identity();
    if (det == 0.0f || !std::isfinite(det)) {
        return out;
    }
    const f32 inv = 1.0f / det;
    out.data[0][0] = c00 * inv;
    out.data[1][0] = c01 * inv;
    out.data[2][0] = c02 * inv;
    out.data[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inv;
    out.data[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inv;
    out.data[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inv;
    out.data[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inv;
    out.data[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inv;
    out.data[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inv;
    for (int c = 0; c < 3; ++c) {
        out.data[3][c] = -(a[3][0] * out.data[0][c] + a[3][1] * out.data[1][c] + a[3][2] * out.data[2][c]);
    }
    return out;
}

void RebasePhysicsFrames(PhysicsSet& physics, std::span<const Matrix44f> change) {
    const auto through = [&](Matrix44f& m, u32 node) {
        if (node < change.size()) {
            m = m * change[node];
        }
    };
    for (PhysicsBody& body : physics.bodies) {
        for (PhysicsShape& shape : body.shapes) {
            through(shape.transform, body.node);
        }
    }
    for (PhysicsJoint& joint : physics.joints) {
        const PhysicsBody* a = physics.body(joint.bodyA);
        const PhysicsBody* b = physics.body(joint.bodyB);
        if (a != nullptr) {
            through(joint.frameA, a->node);
        }
        if (b != nullptr) {
            through(joint.frameB, b->node);
        }
    }
    for (ClothCollider& collider : physics.colliders) {
        through(collider.transform, collider.node);
    }
}

bool PhysicsUsesNode(const PhysicsSet& physics, u32 node) {
    return std::any_of(physics.bodies.begin(), physics.bodies.end(),
                       [node](const PhysicsBody& b) { return b.node == node; }) ||
           std::any_of(physics.colliders.begin(), physics.colliders.end(),
                       [node](const ClothCollider& c) { return c.node == node; });
}

void RescalePhysics(PhysicsSet& physics, f32 factor) {
    for (PhysicsBody& body : physics.bodies) {
        for (PhysicsShape& shape : body.shapes) {
            ScaleTranslation(shape.transform, factor);
            shape.halfExtents = shape.halfExtents * factor;
            shape.radius *= factor;
            shape.length *= factor;
            for (Vector3f& p : shape.points) {
                p = p * factor;
            }
            for (Vector3f& v : shape.vertices) {
                v = v * factor;
            }
        }
    }
    // A force or a torque at an unchanged density goes as mass times length:
    // the fourth power, the one D3 applies with actor scale.
    const f32 fourth = factor * factor * factor * factor;
    for (PhysicsJoint& joint : physics.joints) {
        ScaleTranslation(joint.frameA, factor);
        ScaleTranslation(joint.frameB, factor);
        joint.restLength *= factor;
        joint.breakForce *= fourth;
        joint.breakTorque *= fourth;
        joint.maxMotorForce *= fourth;
        if (joint.friction == JointFriction::Torque) {
            joint.frictionAmount *= fourth;
        }
        if (joint.kind == JointKind::Prismatic) {
            joint.lower *= factor;
            joint.upper *= factor;
            joint.referenceTranslation *= factor;
            joint.motorSpeed *= factor;
        }
    }
    for (PhysicsRig& rig : physics.rigs) {
        if (rig.wow.has_value() && rig.wow->vegetation.has_value()) {
            WowVegetation& push = *rig.wow->vegetation;
            push.posMaxPush *= factor;
            push.posPushAmt *= factor;
            push.velMaxPush *= factor;
            push.minPushDist *= factor * factor;
        }
    }
    for (ClothCollider& collider : physics.colliders) {
        ScaleTranslation(collider.transform, factor);
        collider.radius *= factor;
        collider.length *= factor;
    }
    for (Cloth& cloth : physics.cloths) {
        cloth.wind = cloth.wind * factor;
        if (cloth.sc2.has_value()) {
            cloth.sc2->skinOffset *= factor;
        }
    }
    for (SoftBody& body : physics.soft) {
        for (SoftLump& lump : body.lumps) {
            lump.anchor = lump.anchor * factor;
            lump.rest = lump.rest * factor;
            lump.depth *= factor;
            lump.clear *= factor;
        }
        for (SoftEffect& effect : body.effects) {
            effect.place.radius *= factor;
            if (effect.bulge) {
                for (Vector3f& point : effect.bulge->line) {
                    point = point * factor;
                }
            }
            if (effect.melt) {
                effect.melt->floor *= factor;
            }
            // A Drip's strands as built: where each hangs from, and how wide.
            if (effect.drip) {
                for (Vector3f& at : effect.drip->strandAt) {
                    at = at * factor;
                }
                for (f32& width : effect.drip->strandWidth) {
                    width *= factor;
                }
            }
        }
    }
}

std::string SoftEffectFault(const Model& model, const SoftBody& body, const SoftEffect& effect) {
    const u32 nodeCount = model.nodes.size();
    const auto meshOk = [&](u32 mesh) { return mesh < model.meshes.size(); };
    if (!effect.settingsMatchKind()) {
        return "it has no settings of its kind, or another's";
    }
    const SoftPlace& place = effect.place;
    if (place.mesh != kInvalidIndex && (!meshOk(place.mesh) || place.face >= model.meshes[place.mesh].faceCount())) {
        return "its place is off its mesh";
    }
    if (effect.bulge) {
        for (const u32 node : effect.bulge->chain) {
            if (node >= nodeCount) {
                return "it travels along a node the model lacks";
            }
        }
    }
    if (effect.drip) {
        const SoftDrip& d = *effect.drip;
        if (d.strandNodes.size() != 3 * d.strandAt.size() || d.strandWidth.size() != d.strandAt.size()) {
            return "its strands' lists differ in length";
        }
        if (!d.dropNodes.empty() && d.dropNodes.size() != d.strandAt.size()) {
            return "its droplets are not one a strand";
        }
        for (const u32 node : d.dropNodes) {
            if (node != kInvalidNode && node >= nodeCount) {
                return "a droplet hangs from a node the model lacks";
            }
        }
        // A strand's bone that was removed keeps its place, as none.
        for (const u32 node : d.strandNodes) {
            if (node != kInvalidNode && node >= nodeCount) {
                return "a strand hangs from a node the model lacks";
            }
        }
        const std::size_t sites = d.siteMesh.size();
        if (d.siteFace.size() != sites || d.siteWeights.size() != sites || d.siteThickness.size() != sites) {
            return "its sites' lists differ in length";
        }
    }
    if (effect.jiggle) {
        const SoftJiggle& j = *effect.jiggle;
        const std::size_t kicks = j.kickIds.size();
        if (j.kickMesh.size() != kicks || j.kickFace.size() != kicks || j.kickWeights.size() != kicks ||
            j.kickDirection.size() != kicks || j.kickStrength.size() != kicks) {
            return "its kicks' lists differ in length";
        }
        for (const u32 id : j.kickIds) {
            if (id == 0 || id >= body.nextEffect) {
                return "a kick's id is zero or past the next";
            }
        }
    }
    return {};
}

namespace {

/// EDIT_MODE_SOFTBODY_DESIGN.md §13's rules for one soft body.
void CheckSoftBody(const Model& model, const SoftBody& body, Diagnostics& out) {
    const ElementRef where(ElementKind::PhysicsRecord, body.id);
    const auto error = [&](const std::string& what) {
        out.error(DiagCode::PhysicsReferenceInvalid, "soft body '" + body.name + "': " + what, where);
    };
    const auto warn = [&](const std::string& what) {
        out.warn(DiagCode::PhysicsReferenceInvalid, "soft body '" + body.name + "': " + what, where);
    };
    const u32 nodeCount = model.nodes.size();
    const auto meshOk = [&](u32 mesh) { return mesh < model.meshes.size(); };
    if (body.meshes.empty() || !std::all_of(body.meshes.begin(), body.meshes.end(), meshOk)) {
        error("it holds no mesh, or one the model lacks");
    }
    if (body.drips != kInvalidIndex && !meshOk(body.drips)) {
        error("its drips are a mesh the model lacks");
    }
    for (const std::vector<SoftPart>* parts : {&body.inside, &body.riders}) {
        for (const SoftPart& part : *parts) {
            if (!meshOk(part.mesh) ||
                (part.material != kInvalidIndex && part.material >= model.materialSlots.size())) {
                error("a part names a mesh or a material the model lacks");
            }
        }
    }
    for (const u32 id : body.colliders) {
        if (model.physics.collider(id) == nullptr) {
            error("it names a missing collider");
        }
    }
    if (body.influences == 0) {
        error("its lanes were solved for no weight a point");
    }
    // A channel names the body in sixteen bits of its sub, and an effect or a
    // lump in the other sixteen (`SoftSub`).
    if (body.id > 0xFFFFu || body.nextEffect > 0x10000u || body.lumps.size() >= 0xFFFFu) {
        error("its id, an effect's or a lump's number is past what a channel can name");
    }

    // The effects.
    std::set<u32> effects;
    for (const SoftEffect& effect : body.effects) {
        if (effect.id == 0 || effect.id >= body.nextEffect || !effects.insert(effect.id).second) {
            error("an effect's id is zero, repeated or past the next");
        }
        if (const std::string fault = SoftEffectFault(model, body, effect); !fault.empty()) {
            error("effect '" + effect.name + "': " + fault);
        }
        if (effect.jiggle) {
            for (const u32 id : effect.jiggle->kickIds) {
                if (!effects.insert(id).second) {
                    error("a kick's id is repeated");
                }
            }
        }
    }

    // The lumps.
    std::set<u32> own;
    for (const SoftLump& lump : body.lumps) {
        if (lump.carrier != kInvalidNode && lump.carrier >= nodeCount) {
            error("a lump hangs from a node the model lacks");
        }
        if (lump.kind == SoftLumpKind::Surface) {
            if (lump.node != kInvalidNode) {
                error("a surface lump names a bone of its own");
            }
        } else if (lump.node >= nodeCount) {
            error("a bone's own lump names a node the model lacks");
        } else if (!own.insert(lump.node).second) {
            error("two lumps are the own lump of bone '" + model.nodes.nodes[lump.node].name + "'");
        }
        if (lump.effect != 0 && effects.count(lump.effect) == 0) {
            error("a lump is claimed by an effect that is not there");
        }
    }

    // The lanes, against the skin they were solved for.
    SoftSkinSource skin(model, body.influences);
    std::set<u32> ridden;
    u32 missing = 0, over = 0;
    for (const u32 m : body.meshes) {
        if (!meshOk(m)) {
            continue;
        }
        const Mesh& mesh = model.meshes[m];
        const auto bodyOf = mesh.attributes.get<u32>(geom::names::kSoftBody, geom::Domain::Vertex);
        const auto lumps = mesh.attributes.get<std::array<u32, 4>>(geom::names::kSoftBindLump, geom::Domain::Vertex);
        const auto weights =
            mesh.attributes.get<std::array<f32, 4>>(geom::names::kSoftBindWeight, geom::Domain::Vertex);
        for (u32 v = 0; v < bodyOf.size(); ++v) {
            if (bodyOf[v] != body.id) {
                continue;
            }
            const std::span<const geom::Influence> influences = skin.of(m, v);
            for (const geom::Influence& influence : influences) {
                ridden.insert(influence.bone);
            }
            if (v >= lumps.size() || v >= weights.size()) {
                continue;
            }
            // Per family, what the lanes take of the bone's weight.
            std::array<std::pair<u32, f32>, 4> taken{};
            u32 families = 0;
            for (std::size_t lane = 0; lane < 4; ++lane) {
                if (lumps[v][lane] == 0) {
                    continue;
                }
                if (lumps[v][lane] > body.lumps.size()) {
                    ++missing;
                    continue;
                }
                const u32 family = body.lumps[lumps[v][lane] - 1].family();
                u32 at = 0;
                while (at < families && taken[at].first != family) {
                    ++at;
                }
                if (at == families) {
                    taken[families++] = {family, 0.0f};
                }
                taken[at].second += weights[v][lane];
            }
            for (u32 f = 0; f < families; ++f) {
                f32 has = 0.0f;
                for (const geom::Influence& influence : influences) {
                    if (influence.bone == taken[f].first) {
                        has = influence.weight;
                    }
                }
                if (taken[f].second > has + 1e-4f) {
                    ++over;
                }
            }
        }
    }
    if (missing != 0) {
        error(number(missing) + " lanes name a lump that is not there");
    }
    if (over != 0) {
        warn(number(over) + " points' lanes take more of a bone than the point rides it by; remake it");
    }
    for (const SoftLump& lump : body.lumps) {
        if (lump.kind != SoftLumpKind::Surface && lump.node < nodeCount && ridden.count(lump.node) == 0) {
            warn("it has a lump for bone '" + model.nodes.nodes[lump.node].name +
                 "', which its points no longer ride; remake it");
        }
    }
}

} // namespace

void CheckPhysics(const Model& model, Diagnostics& out) {
    const PhysicsSet& physics = model.physics;
    const u32 nodeCount = model.nodes.size();
    std::set<u32> ids;
    const auto record = [&](u32 id) {
        if (id == 0 || !ids.insert(id).second || id >= physics.nextId) {
            out.error(DiagCode::PhysicsReferenceInvalid,
                      "physics record id " + number(id) + " is zero, repeated or past nextId",
                      ElementRef(ElementKind::PhysicsRecord, id));
        }
    };

    for (const PhysicsBody& body : physics.bodies) {
        record(body.id);
        if (body.node >= nodeCount) {
            out.error(DiagCode::PhysicsReferenceInvalid, "a body rides no node of the tree",
                      ElementRef(ElementKind::PhysicsRecord, body.id));
        }
        for (const PhysicsShape& shape : body.shapes) {
            if (shape.kind == PhysicsShapeKind::Capsule && shape.points.size() != 0 && shape.points.size() != 2) {
                out.error(DiagCode::PhysicsShapeDegenerate,
                          "a capsule states " + number(shape.points.size()) + " ends, not two",
                          ElementRef(ElementKind::PhysicsRecord, body.id));
            }
            if (shape.kind == PhysicsShapeKind::ConvexHull && shape.points.size() < 4) {
                out.warn(DiagCode::PhysicsShapeDegenerate,
                         "a hull of " + number(shape.points.size()) + " points has no volume",
                         ElementRef(ElementKind::PhysicsRecord, body.id));
            } else if (shape.kind == PhysicsShapeKind::ConvexHull && Flat(shape.points)) {
                out.warn(DiagCode::PhysicsShapeDegenerate, "a flat hull: its points lie in one plane",
                         ElementRef(ElementKind::PhysicsRecord, body.id));
            }
            const bool round = shape.kind == PhysicsShapeKind::Sphere || shape.kind == PhysicsShapeKind::Capsule ||
                               shape.kind == PhysicsShapeKind::Cylinder;
            const Vector3f& h = shape.halfExtents;
            if ((round && !(shape.radius > 0.0f)) ||
                (shape.kind == PhysicsShapeKind::Cylinder && !(shape.length > 0.0f)) ||
                (shape.kind == PhysicsShapeKind::Box && !(h.x > 0.0f && h.y > 0.0f && h.z > 0.0f))) {
                out.error(DiagCode::PhysicsShapeDegenerate, std::string("a ") + ToString(shape.kind) + " with no size",
                          ElementRef(ElementKind::PhysicsRecord, body.id));
            }
            if (shape.kind == PhysicsShapeKind::TriangleMesh &&
                (shape.triangles.size() < 3 || shape.triangles.size() % 3 != 0 ||
                 std::any_of(shape.triangles.begin(), shape.triangles.end(),
                             [&](u32 v) { return v >= shape.vertices.size(); }))) {
                out.error(DiagCode::PhysicsShapeDegenerate,
                          "a triangle mesh with no triangle, or an index past its vertices",
                          ElementRef(ElementKind::PhysicsRecord, body.id));
            }
        }
        if (body.wow.has_value() && body.wow->parent != 0) {
            const PhysicsBody* parent = physics.body(body.wow->parent);
            if (parent == nullptr) {
                out.error(DiagCode::PhysicsReferenceInvalid, "a body hangs off a missing body",
                          ElementRef(ElementKind::PhysicsRecord, body.id));
            } else if (parent->motion != BodyMotion::Kinematic) {
                out.warn(DiagCode::PhysicsReferenceInvalid,
                         "a body hangs off a body that is not kinematic, so nothing carries it",
                         ElementRef(ElementKind::PhysicsRecord, body.id));
            }
        }
        const bool massless = std::none_of(body.shapes.begin(), body.shapes.end(),
                                           [](const PhysicsShape& s) { return s.material.density > 0.0f; });
        if (body.motion == BodyMotion::Dynamic && massless) {
            out.warn(DiagCode::PhysicsShapeDegenerate, "a dynamic body with no mass",
                     ElementRef(ElementKind::PhysicsRecord, body.id));
        }
    }
    for (const PhysicsJoint& joint : physics.joints) {
        record(joint.id);
        const PhysicsBody* a = physics.body(joint.bodyA);
        const PhysicsBody* b = physics.body(joint.bodyB);
        if (a == nullptr || b == nullptr || joint.bodyA == joint.bodyB) {
            out.error(DiagCode::PhysicsReferenceInvalid,
                      "a joint names a missing body, or the same body twice",
                      ElementRef(ElementKind::PhysicsRecord, joint.id));
            continue;
        }
        // Both frames are stored; where they disagree the joint snaps on the
        // first step.
        const Vector3f pa = RestOrigin(model, *a, joint.frameA);
        const Vector3f pb = RestOrigin(model, *b, joint.frameB);
        const Vector3f d = pa - pb;
        const f32 gap = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (gap > 0.01f) {
            out.warn(DiagCode::PhysicsJointSnaps,
                     "a joint's two frames are " + std::to_string(gap) + " apart at rest",
                     ElementRef(ElementKind::PhysicsRecord, joint.id));
        }
    }
    for (const ClothCollider& collider : physics.colliders) {
        record(collider.id);
        if (collider.node != kInvalidNode && collider.node >= nodeCount) {
            out.error(DiagCode::PhysicsReferenceInvalid, "a cloth collider rides no node of the tree",
                      ElementRef(ElementKind::PhysicsRecord, collider.id));
        }
    }
    for (const PhysicsRig& rig : physics.rigs) {
        record(rig.id);
        if (std::any_of(rig.bodies.begin(), rig.bodies.end(), [&](u32 id) { return physics.body(id) == nullptr; })) {
            out.error(DiagCode::PhysicsReferenceInvalid, "a rig names a missing body",
                      ElementRef(ElementKind::PhysicsRecord, rig.id));
        }
        if (rig.fracture) {
            const FractureRecipe& recipe = *rig.fracture;
            const auto mesh = [&](u32 m) { return m == kInvalidIndex || m < model.meshes.size(); };
            const auto node = [&](u32 n) { return n == kInvalidNode || n < model.nodes.size(); };
            const bool ok = std::all_of(recipe.sources.begin(), recipe.sources.end(), mesh) &&
                            std::all_of(recipe.made.begin(), recipe.made.end(), mesh) &&
                            std::all_of(recipe.skinNodes.begin(), recipe.skinNodes.end(), node) &&
                            node(recipe.helper) && node(recipe.field) && node(recipe.wholeGate) &&
                            node(recipe.piecesGate) && std::all_of(recipe.bones.begin(), recipe.bones.end(), node) &&
                            recipe.sources.size() == recipe.pieces.size() &&
                            recipe.made.size() == 2 * recipe.pieces.size();
            if (!ok) {
                out.error(DiagCode::PhysicsReferenceInvalid,
                          "rig '" + rig.name + "': its fracture names a mesh or node the model lacks",
                          ElementRef(ElementKind::PhysicsRecord, rig.id));
            }
        }
    }
    for (const PoseStage& stage : model.poseStages) {
        const bool rigOk = stage.rig == 0 || (stage.kind == StageKind::Ragdoll && physics.rig(stage.rig) != nullptr);
        const bool clothOk = stage.cloth == 0 || (stage.kind == StageKind::Cloth && physics.cloth(stage.cloth) != nullptr);
        if (!rigOk || !clothOk) {
            out.error(DiagCode::PhysicsReferenceInvalid,
                      "stage '" + stage.name + "' names a " + (rigOk ? "cloth" : "rig") +
                          " that is not there, or is not of its kind",
                      ElementRef());
        }
    }
    for (const Cloth& cloth : physics.cloths) {
        record(cloth.id);
        const ElementRef where(ElementKind::PhysicsRecord, cloth.id);
        for (const u32 id : cloth.colliders) {
            if (physics.collider(id) == nullptr) {
                out.error(DiagCode::PhysicsReferenceInvalid, "a cloth names a missing collider", where);
            }
        }
        if (cloth.recipe) {
            for (const u32 bone : cloth.recipe->bones) {
                if (bone >= model.nodes.size()) {
                    out.error(DiagCode::PhysicsReferenceInvalid, "a cloth bone is no node of the model", where);
                }
            }
            if (!(cloth.recipe->threshold > 0.0f && cloth.recipe->threshold <= 1.0f)) {
                out.error(DiagCode::PhysicsReferenceInvalid, "a cloth's threshold is outside (0, 1]", where);
            }
        }
        const bool cageOk = cloth.cage.mesh < model.meshes.size() &&
                            cloth.cage.section < model.meshes[cloth.cage.mesh].sections.size();
        if (!cageOk) {
            out.error(DiagCode::ClothTopologyInvalid, "a cloth's cage is no section of the model", where);
            continue;
        }
        const Mesh& mesh = model.meshes[cloth.cage.mesh];
        if (!hasFlag(mesh.sections[cloth.cage.section].flags, SectionFlags::ClothSimulated)) {
            out.error(DiagCode::ClothTopologyInvalid, "a cloth's cage section is not flagged ClothSimulated",
                      where);
        }
        // The cage's vertices, for the bind references.
        const std::vector<u8> inCage = VerticesOfSection(mesh, cloth.cage.section);
        const auto bind =
            mesh.attributes.get<std::array<u32, 4>>(geom::names::kClothBindVertex, geom::Domain::Vertex);
        for (const ClothBinding& binding : cloth.bindings) {
            if (binding.section.mesh != cloth.cage.mesh ||
                binding.section.section >= mesh.sections.size()) {
                out.error(DiagCode::ClothTopologyInvalid,
                          "a cloth binding is no section of its cage's mesh", where);
                continue;
            }
            u32 outside = 0;
            const std::vector<u8> bound = VerticesOfSection(mesh, binding.section.section);
            for (u32 v = 0; v < bound.size() && v < bind.size(); ++v) {
                if (bound[v] == 0) {
                    continue;
                }
                for (const u32 lane : bind[v]) {
                    if (lane != geom::kInvalidId && (lane >= inCage.size() || inCage[lane] == 0)) {
                        ++outside;
                    }
                }
            }
            if (outside != 0) {
                out.error(DiagCode::ClothTopologyInvalid,
                          number(outside) + " bind lanes name a vertex outside the cage", where);
            }
        }
    }
    for (const SoftBody& body : physics.soft) {
        record(body.id);
        CheckSoftBody(model, body, out);
    }
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind != TrackTarget::Kind::Physics || channel.target.sub == 0) {
            continue;
        }
        // Each switch names one kind of record: a body's, a cloth's, a rig's,
        // and a blend a rig's or a body's.
        const u32 sub = channel.target.sub;
        bool found = false;
        switch (channel.target.channel) {
        case Channel::PhysicsDynamic:
            found = physics.body(sub) != nullptr;
            break;
        case Channel::ClothActive:
            found = physics.cloth(sub) != nullptr;
            break;
        case Channel::PhysicsRagdoll:
            found = physics.rig(sub) != nullptr;
            break;
        case Channel::PhysicsBlend:
            found = physics.rig(sub) != nullptr || physics.body(sub) != nullptr;
            break;
        case Channel::ClothDriverTranslation:
        case Channel::ClothDriverRotation:
            // A driver of a cloth there, within its free particles.
            if (const Cloth* cloth = physics.cloth(ClothOfDriverSub(sub))) {
                found = DriverOfSub(sub) < ClothDriversOf(model, *cloth).vertices.size();
            }
            break;
        case Channel::SoftAmount:
            // An effect of a soft body there, or one of its Jiggle's kicks.
            if (const SoftBody* body = physics.softBody(SoftBodyOfSub(sub))) {
                const u32 id = SoftIndexOfSub(sub);
                found = body->effect(id) != nullptr;
                for (const SoftEffect& effect : body->effects) {
                    found = found || (effect.jiggle && std::find(effect.jiggle->kickIds.begin(),
                                                                 effect.jiggle->kickIds.end(),
                                                                 id) != effect.jiggle->kickIds.end());
                }
            }
            break;
        case Channel::SoftLumpTranslation:
        case Channel::SoftLumpRotation:
        case Channel::SoftLumpScale:
            // A lump of a soft body there.
            if (const SoftBody* body = physics.softBody(SoftBodyOfSub(sub))) {
                found = SoftIndexOfSub(sub) >= 1 && SoftIndexOfSub(sub) <= body->lumps.size();
            }
            break;
        default:
            break;
        }
        if (!found) {
            out.error(DiagCode::PhysicsReferenceInvalid,
                      "channel " + number(channel.id) + " names physics record " + number(sub) +
                          (physics.hasId(sub) ? ", which is not the kind its switch belongs to"
                                              : ", which is not there"),
                      ElementRef(ElementKind::Channel, channel.id));
        }
    }
}

void CheckPhysicsForProfile(const Model& model, ProfileId profile, Diagnostics& out) {
    const PhysicsSet& physics = model.physics;
    const PhysicsCaps& caps = Profile(profile).physics;
    if (physics.empty() || !caps.any()) {
        return; // a profile without physics bakes it
    }
    // StarCraft II binds a joint by bone and fixes gravity; World of Warcraft
    // names bodies and derives a limit from its range.
    const bool sc2 = GameOf(profile) == Game::StarCraft;
    for (const PhysicsBody& body : physics.bodies) {
        const ElementRef where(ElementKind::PhysicsRecord, body.id);
        for (const PhysicsShape& shape : body.shapes) {
            const auto carries = [&](PhysicsShapeKind kind) {
                return (caps.shapeKinds & (1u << static_cast<u32>(kind))) != 0;
            };
            if (!carries(shape.kind) && shape.kind == PhysicsShapeKind::Cylinder &&
                carries(PhysicsShapeKind::ConvexHull)) {
                out.info(DiagCode::PhysicsUnsupported, "a cylinder the profile carries as a hull of its 16-sided prism",
                         where, profile);
            } else if (!carries(shape.kind)) {
                out.warn(DiagCode::PhysicsUnsupported,
                         std::string("a ") + ToString(shape.kind) + " shape the profile cannot carry",
                         where, profile);
            }
            if (shape.kind == PhysicsShapeKind::ConvexHull && shape.points.size() > caps.maxHullVertices) {
                out.info(DiagCode::PhysicsHullSimplified,
                         "a hull of " + number(shape.points.size()) + " points past the cooked limit",
                         where, profile);
            }
        }
        if (sc2 && body.gravityScale != 1.0f) {
            out.info(DiagCode::PhysicsGravityScaleDropped, "a body's gravity scale is not 1", where,
                     profile);
        }
    }
    for (const PhysicsJoint& joint : physics.joints) {
        const ElementRef where(ElementKind::PhysicsRecord, joint.id);
        if ((caps.jointKinds & (1u << static_cast<u32>(joint.kind))) == 0) {
            out.warn(DiagCode::PhysicsJointKindUnsupported,
                     std::string("a ") + ToString(joint.kind) + " joint the profile cannot carry", where,
                     profile);
        }
        const PhysicsBody* a = physics.body(joint.bodyA);
        const PhysicsBody* b = physics.body(joint.bodyB);
        if (sc2 && a != nullptr && b != nullptr &&
            (physics.firstBodyOn(a->node) != a || physics.firstBodyOn(b->node) != b)) {
            out.warn(DiagCode::PhysicsJointBodyAmbiguous,
                     "a joint's body is not the first on its node, which the profile binds", where,
                     profile);
        }
        // A feature the caps say the format has no field for, once per joint.
        const bool motor = !caps.motors && joint.motor != JointMotorMode::Off;
        const bool breaks = !caps.jointBreaking && (joint.breakForce != 0.0f || joint.breakTorque != 0.0f);
        if (motor || breaks) {
            out.warn(DiagCode::PhysicsJointFieldDropped,
                     std::string(motor && breaks ? "a joint's motor and breaking"
                                 : motor         ? "a joint's motor"
                                                 : "a joint's breaking") +
                         ", which the profile cannot carry",
                     where, profile);
        }
        const bool dropped =
            sc2 ? joint.linearSpring.hz != 0.0f || joint.restLength != 0.0f || joint.friction == JointFriction::Torque
                : joint.collideConnected || joint.friction == JointFriction::GravityHold ||
                      (HasRange(joint.kind) && joint.limitEnabled != (joint.upper > joint.lower));
        if (dropped) {
            out.info(DiagCode::PhysicsJointFieldDropped, "a joint field the profile cannot say", where,
                     profile);
        }
    }
    if (!physics.cloths.empty() && !caps.cloth) {
        out.warn(DiagCode::PhysicsUnsupported, "cloth the profile cannot carry",
                 ElementRef(ElementKind::Document, 0), profile);
        return;
    }
    for (const ClothCollider& collider : physics.colliders) {
        if (caps.cloth && !caps.planeColliders && collider.kind == ClothColliderKind::Plane) {
            out.warn(DiagCode::PhysicsUnsupported, "a plane cloth collider the profile cannot carry",
                     ElementRef(ElementKind::PhysicsRecord, collider.id), profile);
        }
    }
    for (const Cloth& cloth : physics.cloths) {
        if (cloth.cage.mesh >= model.meshes.size()) {
            continue;
        }
        const Mesh& mesh = model.meshes[cloth.cage.mesh];
        std::set<u32> particles;
        const std::vector<u8> inCage = VerticesOfSection(mesh, cloth.cage.section);
        for (u32 v = 0; v < inCage.size(); ++v) {
            if (inCage[v] != 0) {
                particles.insert(v);
            }
        }
        if (particles.size() < 3 || particles.size() > caps.maxClothParticles) {
            out.warn(DiagCode::ClothParticleLimit,
                     "a cage of " + number(particles.size()) + " particles, outside 3-" +
                         number(caps.maxClothParticles),
                     ElementRef(ElementKind::PhysicsRecord, cloth.id), profile);
        }
        for (const u32 v : particles) {
            for (const geom::Influence& influence : mesh.skin.forVertex(v)) {
                if (influence.bone > caps.maxClothAnchorBone) {
                    out.warn(DiagCode::ClothAnchorBoneOutOfRange,
                             "a cloth anchor on node " + number(influence.bone) +
                                 ", past the byte the profile stores",
                             ElementRef(ElementKind::PhysicsRecord, cloth.id), profile);
                    break;
                }
            }
        }
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout
