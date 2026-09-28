// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "d3_physics.h"

#include <whiteout/models/wem/nodes/node.h>
#include <whiteout/sno/d3/native/physics_rules.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <whiteout/sno/d3/native/geometry.h>

namespace whiteout {
namespace models {
namespace wem {
namespace d3_physics {

namespace {

namespace d3n = sno::d3::native;

/// Both builders stop at 64 bodies that got a fixture.
constexpr std::size_t kMaxBodies = 64;
/// The appearance-level list is capped before it is walked.
constexpr std::size_t kMaxAppearanceJoints = 8;
constexpr i32 kAnchoredLod = 0;
constexpr i32 kCollapseLod = 1;

/// What the actor's `.phy` gives every shape and body.
struct Material {
    f32 classDensity = d3n::densityForClass(0);
    f32 friction = 0.3f;
    f32 restitution = 0.0f;
    f32 linearDamping = 0.0f;
    f32 angularDamping = 0.0f;
};

Material MaterialOf(const d3n::Physics* phy) {
    Material material;
    if (phy != nullptr) {
        material.classDensity = d3n::densityForClass(phy->nBodyClass);
        material.friction = phy->flFriction;
        material.restitution = phy->flRestitution;
        material.linearDamping = phy->flLinearDamping;
        material.angularDamping = phy->flAngularDamping;
    }
    return material;
}

Quaternion QuatOf(const Vector4f& v) {
    return Quaternion(v.x, v.y, v.z, v.w);
}

/// A frame as Domino receives it: the file's rotation times @p pre, when the
/// kind pre-rotates.
Matrix44f FrameOf(const d3n::PRTransform& frame, const Quaternion* pre) {
    Transform x;
    x.translation = frame.vTranslation;
    x.rotation = pre != nullptr ? QuatOf(frame.qRotation) * *pre : QuatOf(frame.qRotation);
    x.scale = Vector3f{1, 1, 1};
    return ToMatrix(x);
}

Matrix44f At(const Vector3f& p) {
    Matrix44f m = Matrix44f::identity();
    m.data[3][0] = p.x;
    m.data[3][1] = p.y;
    m.data[3][2] = p.z;
    return m;
}

/// A constraint as the client's joint builder reads it; false for type 4,
/// which it has no case for.
bool JointOf(const d3n::ConstraintParameters& c, u32 bodyA, u32 bodyB, PhysicsJoint& joint) {
    // A revolute's frames turn by (1/2,1/2,1/2,1/2), a shoulder's by its
    // conjugate: a cyclic permutation of the axes.
    static const Quaternion kRevolute(0.5f, 0.5f, 0.5f, 0.5f);
    static const Quaternion kShoulder(-0.5f, -0.5f, -0.5f, 0.5f);
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.collideConnected = (c.dwFlags & 1) != 0;
    joint.breakForce = c.flParam05;
    joint.breakTorque = c.flParam11;
    switch (c.eConstraintType) {
    case 0:
        joint.kind = JointKind::Revolute;
        joint.frameA = FrameOf(c.tFrameB, &kRevolute);
        joint.frameB = FrameOf(c.tFrameC, &kRevolute);
        joint.lower = c.flLimitLower;
        joint.upper = c.flLimitUpper;
        joint.limitEnabled = (c.dwFlags & 2) != 0;
        return true;
    case 1:
        joint.kind = JointKind::ConeTwist;
        joint.frameA = FrameOf(c.tFrameB, &kShoulder);
        joint.frameB = FrameOf(c.tFrameC, &kShoulder);
        joint.cone = c.flConeAngle;
        joint.lower = c.flTwistLower;
        joint.upper = c.flTwistUpper;
        joint.limitEnabled = true; // unconditional for a shoulder
        return true;
    case 2:
        // Both rotations are discarded.
        joint.kind = JointKind::Spherical;
        joint.frameA = At(c.tFrameB.vTranslation);
        joint.frameB = At(c.tFrameC.vTranslation);
        return true;
    case 3:
        joint.kind = JointKind::Weld;
        joint.frameA = FrameOf(c.tFrameB, nullptr);
        joint.frameB = FrameOf(c.tFrameC, nullptr);
        // The explicit pair, else a spring the client makes of the stiffness,
        // else its default (D3_PHYSICS_PLAN.md §3.4).
        if (c.flParam07 > 0.0f) {
            joint.angularSpring = JointSpring{c.flParam07, c.flParam08};
        } else if (c.flParam06 > 0.0f) {
            joint.angularSpring = JointSpring{std::max(1.0f - c.flParam06, 0.0f) * 15.0f + 1.0f, 0.9f};
        } else {
            joint.angularSpring = JointSpring{0.7f, 0.7f};
        }
        return true;
    default:
        return false;
    }
}

/// One rig being built: the bodies with a fixture, by bone.
struct Build {
    std::vector<PhysicsBody> bodies;
    std::vector<i32> bodyOfBone;   ///< Into `bodies`, or -1.
    std::vector<i32> ancestorOf;   ///< Per body, the ancestor body it is jointed to, or -1.
};

class Importer {
public:
    Importer(const d3n::Appearances& source, const Sources& sources, std::span<const u32> boneToNode,
             Model& model, Diagnostics& out)
        : source_(source), sources_(sources), boneToNode_(boneToNode), model_(model), out_(out),
          material_(MaterialOf(sources.physics)) {}

    void Run() {
        const bool anchored = Commit(Anchored(), "anchored", RigStart::Always);
        Commit(Collapse(), "collapse", RigStart::OnDeath);
        // A breakable's lod-0 bodies: neither builder the client is known to
        // run reaches them without an anchor (D3_PHYSICS_PLAN.md §3.5).
        const auto dynamicAtZero = std::count_if(source_.arBones.begin(), source_.arBones.end(),
                                                 [](const d3n::BoneStructure& b) { return d3n::boneIsDynamic(b, kAnchoredLod); });
        if (!anchored && dynamicAtZero != 0) {
            out_.info(DiagCode::PhysicsUnsupported,
                      std::to_string(dynamicAtZero) + " bones with lod-0 mass and no anchor: no traced builder reaches them",
                      ElementRef(ElementKind::Document, 0), ProfileId::Diablo3);
        }
        if (hullsWithoutBytes_ != 0) {
            out_.warn(DiagCode::PhysicsUnsupported,
                      std::to_string(hullsWithoutBytes_) + " polytope shapes whose cook lives in an .app not given",
                      ElementRef(ElementKind::Document, 0), ProfileId::Diablo3);
        }
        if (unbuilt_ != 0) {
            out_.info(DiagCode::PhysicsConstraintDropped,
                      std::to_string(unbuilt_) + " constraints of a type the client builds no joint for",
                      ElementRef(ElementKind::Document, 0), ProfileId::Diablo3);
        }
    }

private:
    /// @p bone's shapes at @p lod: the fixtures its body gets.
    std::vector<PhysicsShape> ShapesAt(const d3n::BoneStructure& bone, i32 lod) {
        std::vector<PhysicsShape> shapes;
        for (const d3n::CollisionShape& s : bone.arCollisionShapes) {
            if (s.nLodIndex != lod) {
                continue;
            }
            PhysicsShape shape;
            shape.material = PhysicsMaterial{material_.classDensity * s.flScaleX, material_.friction,
                                             material_.restitution};
            switch (s.eShapeType) {
            case 0:
                shape.kind = PhysicsShapeKind::Sphere;
                shape.transform = At(s.vPointA);
                shape.radius = s.flRadius;
                break;
            case 1:
                shape.kind = PhysicsShapeKind::Capsule;
                shape.points = {s.vPointA, s.vPointB};
                shape.radius = s.flRadius;
                break;
            case 2: {
                if (sources_.appearanceBytes.empty()) {
                    ++hullsWithoutBytes_;
                    continue;
                }
                // A cook the client refuses builds no fixture.
                std::optional<d3n::Polytope> cook = d3n::readPolytope(s.arPolytopeData, sources_.appearanceBytes);
                if (!cook.has_value()) {
                    continue;
                }
                shape.kind = PhysicsShapeKind::ConvexHull;
                shape.points = std::move(cook->points);
                break;
            }
            default:
                continue;
            }
            shapes.push_back(std::move(shape));
        }
        return shapes;
    }

    /// Adds a body on @p bone with its fixtures at @p lod, or none when it has
    /// none there (the bridge destroys such a body). Returns its index or -1.
    i32 AddBody(Build& build, std::size_t bone, i32 lod, BodyMotion motion) {
        std::vector<PhysicsShape> shapes = ShapesAt(source_.arBones[bone], lod);
        if (shapes.empty() || bone >= boneToNode_.size()) {
            return -1;
        }
        PhysicsBody body;
        body.node = boneToNode_[bone];
        body.motion = motion;
        body.shapes = std::move(shapes);
        body.linearDamping = material_.linearDamping;
        body.angularDamping = material_.angularDamping;
        const i32 index = static_cast<i32>(build.bodies.size());
        build.bodies.push_back(std::move(body));
        build.bodyOfBone[bone] = index;
        build.ancestorOf.push_back(-1);
        return index;
    }

    /// `sub_71003E07B0` at lod 0: anchors kinematic, and a bone joins as a
    /// dynamic body when it reaches a bodied ancestor through a constraint and
    /// the ancestor is dynamic or the constraint says to start a chain.
    Build Anchored() {
        Build build;
        const auto& bones = source_.arBones;
        build.bodyOfBone.assign(bones.size(), -1);
        for (std::size_t b = 0; b < bones.size() && build.bodies.size() < kMaxBodies; ++b) {
            const d3n::BoneStructure& bone = bones[b];
            if (bone.arCollisionShapes.empty()) {
                continue;
            }
            if (d3n::boneIsAnchor(bone)) {
                AddBody(build, b, kAnchoredLod, BodyMotion::Kinematic);
                continue;
            }
            i32 ancestor = -1;
            for (i32 p = bone.nParentIndex; p >= 0 && static_cast<std::size_t>(p) < bones.size();
                 p = bones[static_cast<std::size_t>(p)].nParentIndex) {
                if (build.bodyOfBone[static_cast<std::size_t>(p)] >= 0) {
                    ancestor = build.bodyOfBone[static_cast<std::size_t>(p)];
                    break;
                }
            }
            if (ancestor < 0 || bone.arConstraints.empty()) {
                continue;
            }
            const bool forced = (bone.arConstraints[0].dwFlags & 0x10) != 0;
            if (!forced && build.bodies[static_cast<std::size_t>(ancestor)].motion != BodyMotion::Dynamic) {
                continue;
            }
            const i32 index = AddBody(build, b, kAnchoredLod, BodyMotion::Dynamic);
            if (index >= 0) {
                build.ancestorOf[static_cast<std::size_t>(index)] = ancestor;
            }
        }
        // Only a rig something swings in is one.
        const bool swings = std::any_of(build.bodies.begin(), build.bodies.end(),
                                        [](const PhysicsBody& body) { return body.motion == BodyMotion::Dynamic; });
        if (!swings) {
            build.bodies.clear();
        }
        return build;
    }

    /// `Physics_CreateActorBoneBodies` at lod 1: every bodied bone, dynamic
    /// where a shape has mass. No joint from the bone list.
    Build Collapse() {
        Build build;
        const auto& bones = source_.arBones;
        build.bodyOfBone.assign(bones.size(), -1);
        for (std::size_t b = 0; b < bones.size() && build.bodies.size() < kMaxBodies; ++b) {
            if (bones[b].arCollisionShapes.empty()) {
                continue;
            }
            AddBody(build, b, kCollapseLod,
                    d3n::boneIsDynamic(bones[b], kCollapseLod) ? BodyMotion::Dynamic : BodyMotion::Static);
        }
        return build;
    }

    bool Commit(Build build, const char* name, RigStart start) {
        if (build.bodies.empty()) {
            return false;
        }
        PhysicsSet& physics = model_.physics;
        PhysicsRig rig;
        rig.name = name;
        rig.start = start;
        std::vector<u32> ids;
        for (PhysicsBody& body : build.bodies) {
            body.id = physics.allocateId();
            ids.push_back(body.id);
            rig.bodies.push_back(body.id);
            physics.bodies.push_back(std::move(body));
        }
        const auto& bones = source_.arBones;
        // Body A is the ancestor, B the bone: the direction the block is authored in.
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const i32 ancestor = build.ancestorOf[i];
            if (ancestor < 0) {
                continue;
            }
            for (std::size_t b = 0; b < bones.size(); ++b) {
                if (build.bodyOfBone[b] == static_cast<i32>(i)) {
                    AddJoint(bones[b].arConstraints[0], ids[static_cast<std::size_t>(ancestor)], ids[i]);
                    break;
                }
            }
        }
        const std::size_t extras = std::min(source_.arConstraints.size(), kMaxAppearanceJoints);
        for (std::size_t k = 0; k < extras; ++k) {
            const d3n::ConstraintParameters& c = source_.arConstraints[k];
            const auto bodyOf = [&](i32 bone) {
                return bone >= 0 && static_cast<std::size_t>(bone) < build.bodyOfBone.size()
                           ? build.bodyOfBone[static_cast<std::size_t>(bone)]
                           : -1;
            };
            const i32 a = bodyOf(c.nBoneIndexA);
            const i32 b = bodyOf(c.nBoneIndexB);
            if (a >= 0 && b >= 0) {
                AddJoint(c, ids[static_cast<std::size_t>(a)], ids[static_cast<std::size_t>(b)]);
            }
        }
        rig.id = physics.allocateId();
        physics.rigs.push_back(std::move(rig));
        return true;
    }

    void AddJoint(const d3n::ConstraintParameters& c, u32 a, u32 b) {
        PhysicsJoint joint;
        if (!JointOf(c, a, b, joint)) {
            ++unbuilt_;
            return;
        }
        joint.id = model_.physics.allocateId();
        model_.physics.joints.push_back(joint);
    }

    const d3n::Appearances& source_;
    const Sources& sources_;
    std::span<const u32> boneToNode_;
    Model& model_;
    Diagnostics& out_;
    Material material_;
    u32 hullsWithoutBytes_ = 0;
    u32 unbuilt_ = 0;
};

/// The client builds no cloth for a `.clt` below this mass.
constexpr f32 kMinClothMass = 1e-6f;
/// A `.clt` plane slot names `HP_cloth_plane_<n>`, n in 1..8.
constexpr i32 kMaxPlaneSlot = 8;
/// D3's rigid world pulls at 32.2; its cloth at `flGravity * 3600 - 43.2`.
constexpr f32 kHostGravity = 32.2f;

Transform TransformOf(const d3n::PRTransform& t) {
    Transform x;
    x.translation = t.vTranslation;
    x.rotation = QuatOf(t.qRotation);
    x.scale = Vector3f{1, 1, 1};
    return x;
}

/// A hardpoint's frame in its bone's: the hardpoint is authored in model space
/// and rides the bone through `tTransform1`, the inverse of its bind.
Matrix44f HardpointFrame(const d3n::Appearances& source, const d3n::Hardpoint& hardpoint) {
    const Matrix44f own = ToMatrix(TransformOf(hardpoint.tTransform));
    if (hardpoint.nBoneIndex < 0 || static_cast<std::size_t>(hardpoint.nBoneIndex) >= source.arBones.size()) {
        return own;
    }
    const d3n::PRSTransform& inverse = source.arBones[static_cast<std::size_t>(hardpoint.nBoneIndex)].tTransform1;
    Transform bind;
    bind.translation = inverse.vTranslation;
    bind.rotation = QuatOf(inverse.qRotation);
    bind.scale = Vector3f{inverse.flScale, inverse.flScale, inverse.flScale};
    return own * ToMatrix(bind);
}

} // namespace

void ImportRigs(const d3n::Appearances& source, const Sources& sources, std::span<const u32> boneToNode,
                Model& model, Diagnostics& out) {
    Importer(source, sources, boneToNode, model, out).Run();
}

const d3n::Cloth* ClothTuning(const d3n::Appearances& source, const d3n::SubObject& sub, u32 look,
                              AssetSource& assets) {
    for (const d3n::AppearanceMaterial& material : source.arMaterials) {
        if (material.szName != sub.szName || look >= material.arVariants.size()) {
            continue;
        }
        const d3n::AssetRef& ref = material.arVariants[look].snoCloth;
        const d3n::Cloth* tuning = ref.valid() ? assets.cloth(ref.id) : nullptr;
        return tuning != nullptr && tuning->flMass >= kMinClothMass ? tuning : nullptr;
    }
    return nullptr;
}

u32 AddCage(geom::MeshBuilder& builder, const d3n::SubObject& sub, u32 bound, u32 base,
            std::span<const u32> boneToNode, std::vector<u32>& bindVertex, Diagnostics& out) {
    const d3n::ClothStructure& cloth = sub.arClothData.front();
    const std::vector<MeshSection>& sections = builder.sections();
    MeshSection cage;
    cage.name = sub.szName + "#cage";
    cage.materialSlot = bound < sections.size() ? sections[bound].materialSlot : kInvalidIndex;
    cage.flags = SectionFlags::Hidden | SectionFlags::ClothSimulated;
    const u32 section = builder.addSection(std::move(cage));

    // A staple anchors its own particle; a driving group is anchored by the
    // staples its particles belong to, as the solver blends their bones.
    const std::size_t particles = cloth.arVertices.size();
    std::map<i32, std::map<u32, f32>> groups;
    for (const d3n::ClothStaple& staple : cloth.arStaples) {
        if (staple.nVertexIndex < 0 || static_cast<std::size_t>(staple.nVertexIndex) >= particles) {
            continue;
        }
        const i32 group = cloth.arVertices[static_cast<std::size_t>(staple.nVertexIndex)].nDrivingBone;
        const i32 bones[3] = {staple.nBoneIndex0, staple.nBoneIndex1, staple.nBoneIndex2};
        const f32 weights[3] = {staple.flWeight0, staple.flWeight1, staple.flWeight2};
        for (int k = 0; k < 3 && bones[k] != -1; ++k) {
            if (bones[k] >= 0 && static_cast<std::size_t>(bones[k]) < boneToNode.size()) {
                groups[group][boneToNode[static_cast<std::size_t>(bones[k])]] += weights[k];
            }
        }
    }
    const u32 first = builder.vertexCount();
    for (std::size_t p = 0; p < particles; ++p) {
        const d3n::ClothVertex& particle = cloth.arVertices[p];
        const geom::VertexId vertex = builder.addVertex(particle.vPosition);
        builder.setVertexAttr(vertex, geom::names::kClothMovable, static_cast<u8>(particle.flInvMass != 0.0f ? 1 : 0));
        std::vector<std::pair<f32, u32>> anchors;
        if (p < cloth.arStaples.size() && cloth.arStaples[p].nVertexIndex == static_cast<i32>(p)) {
            const d3n::ClothStaple& staple = cloth.arStaples[p];
            const i32 bones[3] = {staple.nBoneIndex0, staple.nBoneIndex1, staple.nBoneIndex2};
            const f32 weights[3] = {staple.flWeight0, staple.flWeight1, staple.flWeight2};
            for (int k = 0; k < 3 && bones[k] != -1; ++k) {
                if (bones[k] >= 0 && static_cast<std::size_t>(bones[k]) < boneToNode.size() && weights[k] > 0.0f) {
                    anchors.emplace_back(weights[k], boneToNode[static_cast<std::size_t>(bones[k])]);
                }
            }
        } else if (const auto group = groups.find(particle.nDrivingBone); group != groups.end()) {
            f32 total = 0.0f;
            for (const auto& [node, weight] : group->second) {
                total += weight;
            }
            for (const auto& [node, weight] : group->second) {
                if (weight > 0.0f && total > 0.0f) {
                    anchors.emplace_back(weight / total, node);
                }
            }
        }
        std::sort(anchors.rbegin(), anchors.rend());
        anchors.resize(std::min<std::size_t>(anchors.size(), 4));
        for (const auto& [weight, node] : anchors) {
            builder.addInfluence(vertex, node, weight);
        }
    }
    for (const d3n::ClothFace& face : cloth.arFaces) {
        const i32 corners[3] = {face.nVertex0, face.nVertex1, face.nVertex2};
        if (std::any_of(corners, corners + 3, [&](i32 c) { return c < 0 || static_cast<std::size_t>(c) >= particles; })) {
            out.warn(DiagCode::ClothTopologyInvalid, "a cloth face names no particle", ElementRef(), ProfileId::Diablo3);
            continue;
        }
        builder.addTriangle(geom::VertexId(first + static_cast<u32>(corners[0])),
                            geom::VertexId(first + static_cast<u32>(corners[1])),
                            geom::VertexId(first + static_cast<u32>(corners[2])), section);
    }
    // Each render vertex draws from one particle, at full weight.
    bindVertex.resize(builder.vertexCount(), geom::kInvalidId);
    if ((static_cast<u32>(sub.dwVertexFormat) & d3n::kSubObjectHasClothIndex) != 0) {
        for (std::size_t v = 0; v < sub.arVertices.size(); ++v) {
            const u32 particle = sub.arVertices[v].dwClothVertexIndex;
            if (particle < particles) {
                bindVertex[base + v] = first + particle;
            }
        }
    }
    return section;
}

void ImportCloths(const d3n::Appearances& source, std::span<const ClothSite> sites, std::span<const u32> boneToNode,
                  Model& model, Diagnostics& out) {
    PhysicsSet& physics = model.physics;
    const auto nodeOf = [&](i32 bone) {
        return bone >= 0 && static_cast<std::size_t>(bone) < boneToNode.size() ? boneToNode[static_cast<std::size_t>(bone)]
                                                                                 : kInvalidNode;
    };
    // Every capsule: a cloth that collides with its own actor uses them all,
    // and another actor's cloth can use them too.
    std::vector<u32> capsules;
    for (const d3n::CollisionCapsule& capsule : source.arCollisionCapsules) {
        ClothCollider collider;
        collider.id = physics.allocateId();
        collider.node = nodeOf(capsule.tHardpoint.nBoneIndex);
        collider.kind = ClothColliderKind::Capsule;
        collider.transform = HardpointFrame(source, capsule.tHardpoint);
        collider.radius = capsule.flRadius;
        collider.length = capsule.flLength;
        capsules.push_back(collider.id);
        physics.colliders.push_back(collider);
    }
    std::map<i32, u32> planes;
    const auto planeFor = [&](i32 slot) -> u32 {
        if (slot <= 0 || slot > kMaxPlaneSlot) {
            return 0;
        }
        if (const auto made = planes.find(slot); made != planes.end()) {
            return made->second;
        }
        char name[24];
        std::snprintf(name, sizeof name, "HP_cloth_plane_%d", static_cast<int>(slot));
        for (const d3n::Hardpoint& hardpoint : source.arHardpoints) {
            if (hardpoint.szName != name) {
                continue;
            }
            // A D3 plane faces its hardpoint's +X; a WEM plane faces its own +Z.
            Matrix44f facing = Matrix44f::identity();
            facing.data[0][0] = 0.0f;
            facing.data[0][1] = 1.0f;
            facing.data[1][1] = 0.0f;
            facing.data[1][2] = 1.0f;
            facing.data[2][0] = 1.0f;
            facing.data[2][2] = 0.0f;
            ClothCollider collider;
            collider.id = physics.allocateId();
            collider.node = nodeOf(hardpoint.nBoneIndex);
            collider.kind = ClothColliderKind::Plane;
            collider.transform = facing * HardpointFrame(source, hardpoint);
            physics.colliders.push_back(collider);
            return planes[slot] = collider.id;
        }
        // A `.clt` naming a plane its model lacks is ordinary; the client skips it.
        return planes[slot] = 0;
    };
    u32 unbound = 0;
    for (const ClothSite& site : sites) {
        if (site.tuning == nullptr || site.mesh == kInvalidIndex) {
            continue;
        }
        // A cage vertex is one a cloth face uses. A render vertex drawing from a
        // particle in no face -- one D3 keeps for its constraints alone, or one
        // the repair left without a face -- draws skinned instead.
        Mesh& mesh = model.meshes[site.mesh];
        std::vector<u8> inCage(mesh.faceSet().vertexCount, 0);
        {
            const geom::FaceSet& faces = mesh.faceSet();
            const std::span<const u32> sections = mesh.faceSections();
            std::size_t corner = 0;
            for (std::size_t f = 0; f < faces.faceValence.size(); ++f) {
                for (u32 k = 0; k < faces.faceValence[f]; ++k) {
                    const u32 v = faces.cornerVertex[corner + k];
                    if (f < sections.size() && sections[f] == site.cage && v < inCage.size()) {
                        inCage[v] = 1;
                    }
                }
                corner += faces.faceValence[f];
            }
        }
        const std::span<std::array<u32, 4>> lanes = mesh.attributes.getOrCreate<std::array<u32, 4>>(
            geom::names::kClothBindVertex, geom::Domain::Vertex, geom::AttrType::U32x4);
        const std::span<std::array<f32, 4>> weights = mesh.attributes.getOrCreate<std::array<f32, 4>>(
            geom::names::kClothBindWeight, geom::Domain::Vertex, geom::AttrType::F32x4);
        for (std::size_t v = 0; v < lanes.size(); ++v) {
            for (std::size_t k = 0; k < 4; ++k) {
                const u32 lane = lanes[v][k];
                if (lane != geom::kInvalidId && (lane >= inCage.size() || inCage[lane] == 0)) {
                    lanes[v][k] = geom::kInvalidId;
                    if (v < weights.size()) {
                        weights[v][k] = 0.0f;
                    }
                    ++unbound;
                }
            }
        }
        const d3n::Cloth& t = *site.tuning;
        Cloth cloth;
        cloth.id = physics.allocateId();
        cloth.cage = SectionRef{site.mesh, site.cage};
        cloth.bindings.push_back(ClothBinding{SectionRef{site.mesh, site.bound}});
        cloth.density = t.flMass;
        cloth.damping = t.flLinearDamping;
        cloth.friction = t.flContactDamping;
        cloth.stretchStiffness = t.flStretchStiffness0;
        cloth.bendStiffness = t.flBendStiffness;
        cloth.gravityScale = (43.2f - t.flGravity * 3600.0f) / kHostGravity;
        cloth.wind = t.nUseCustomWind != 0 ? t.vWindVelocity : Vector3f{0, 0, 0};
        if ((t.dwFlags & 4) != 0) {
            cloth.colliders = capsules;
        }
        for (const i32 slot : {t.nCollisionPlane0, t.nCollisionPlane1, t.nCollisionPlane2, t.nCollisionPlane3}) {
            if (const u32 plane = planeFor(slot); plane != 0) {
                cloth.colliders.push_back(plane);
            }
        }
        physics.cloths.push_back(std::move(cloth));
    }
    if (unbound != 0) {
        out.info(DiagCode::ClothTopologyInvalid,
                 std::to_string(unbound) + " cloth-driven vertices name a particle no cloth face uses; they draw skinned",
                 ElementRef(ElementKind::Document, 0), ProfileId::Diablo3);
    }
}

} // namespace d3_physics
} // namespace wem
} // namespace models
} // namespace whiteout
