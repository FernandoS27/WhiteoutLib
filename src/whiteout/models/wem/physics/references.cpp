// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/references.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string>

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
    for (PhysicsRig& rig : physics.rigs) {
        std::erase_if(rig.bodies, [&](u32 id) { return gone.count(id) != 0; });
    }
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
    const Transform world = model.nodes.worldBind(body.node);
    return TransformPoint(world, Row(frame, 3));
}

} // namespace

std::vector<u32> RemapPhysicsNodes(PhysicsSet& physics, std::span<const u32> remap) {
    std::vector<u32> removed;
    std::set<u32> gone;
    std::erase_if(physics.bodies, [&](PhysicsBody& body) {
        if (body.node == kInvalidNode) {
            return false;
        }
        body.node = Remapped(remap, body.node, kInvalidNode);
        if (body.node == kInvalidNode) {
            gone.insert(body.id);
            removed.push_back(body.id);
            return true;
        }
        return false;
    });
    std::erase_if(physics.colliders, [&](ClothCollider& collider) {
        // The root is no node, and stays the root.
        if (collider.node == kInvalidNode) {
            return false;
        }
        collider.node = Remapped(remap, collider.node, kInvalidNode);
        if (collider.node == kInvalidNode) {
            gone.insert(collider.id);
            removed.push_back(collider.id);
            return true;
        }
        return false;
    });
    Sweep(physics, gone, removed);
    return removed;
}

std::vector<u32> RemapPhysicsMeshes(PhysicsSet& physics, std::span<const u32> meshRemap) {
    std::vector<u32> removed;
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

std::vector<u32> RemapPhysicsSections(PhysicsSet& physics, u32 mesh, std::span<const u32> sectionRemap) {
    std::vector<u32> removed;
    std::erase_if(physics.cloths, [&](Cloth& cloth) {
        std::erase_if(cloth.bindings, [&](ClothBinding& binding) {
            if (binding.section.mesh != mesh) {
                return false;
            }
            binding.section.section = Remapped(sectionRemap, binding.section.section, kInvalidIndex);
            return binding.section.section == kInvalidIndex;
        });
        if (cloth.cage.mesh != mesh) {
            return false;
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
        if (channel.target.kind == TrackTarget::Kind::Physics &&
            std::find(removed.begin(), removed.end(), channel.target.sub) != removed.end()) {
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
    for (PhysicsJoint& joint : physics.joints) {
        ScaleTranslation(joint.frameA, factor);
        ScaleTranslation(joint.frameB, factor);
        joint.restLength *= factor;
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
}

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
            if (shape.kind == PhysicsShapeKind::ConvexHull && shape.points.size() < 4) {
                out.warn(DiagCode::PhysicsShapeDegenerate,
                         "a hull of " + number(shape.points.size()) + " points has no volume",
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
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind != TrackTarget::Kind::Physics || channel.target.sub == 0) {
            continue;
        }
        const bool found = channel.target.channel == Channel::PhysicsDynamic
                               ? physics.body(channel.target.sub) != nullptr
                               : channel.target.channel == Channel::ClothActive &&
                                     physics.cloth(channel.target.sub) != nullptr;
        if (!found) {
            out.error(DiagCode::PhysicsReferenceInvalid,
                      "channel " + number(channel.id) + " names physics record " +
                          number(channel.target.sub) + ", which is not there",
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
    for (const PhysicsBody& body : physics.bodies) {
        const ElementRef where(ElementKind::PhysicsRecord, body.id);
        for (const PhysicsShape& shape : body.shapes) {
            if ((caps.shapeKinds & (1u << static_cast<u32>(shape.kind))) == 0) {
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
        if (body.gravityScale != 1.0f) {
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
        if (a != nullptr && b != nullptr &&
            (physics.firstBodyOn(a->node) != a || physics.firstBodyOn(b->node) != b)) {
            out.warn(DiagCode::PhysicsJointBodyAmbiguous,
                     "a joint's body is not the first on its node, which the profile binds", where,
                     profile);
        }
        if (joint.linearSpring.hz != 0.0f || joint.restLength != 0.0f || joint.breakForce != 0.0f ||
            joint.breakTorque != 0.0f) {
            out.info(DiagCode::PhysicsJointFieldDropped, "a joint field the profile cannot say", where,
                     profile);
        }
    }
    if (!physics.cloths.empty() && !caps.cloth) {
        out.warn(DiagCode::PhysicsUnsupported, "cloth the profile cannot carry",
                 ElementRef(ElementKind::Document, 0), profile);
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
