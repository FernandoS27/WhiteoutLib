// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Diablo III physics through WEM (WEM_PHYSICS_DESIGN.md §8.2).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>

#include <whiteout/models/wem/d3_converter.h>
#include <whiteout/models/wem/nodes/node.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/sno/d3/native/d3_native.h>
#include <whiteout/sno/d3/native/physics_rules.h>
#include <whiteout/sno/d3/native/types.h>

#include "wem_d3_corpus.h"

namespace fs = std::filesystem;
using namespace whiteout;
namespace d3n = whiteout::sno::d3::native;
namespace wem = whiteout::models::wem;

namespace {

bool SameBits(f32 a, f32 b) {
    return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

bool SameQuat(const Quaternion& a, const Vector4f& b) {
    return SameBits(a.x, b.x) && SameBits(a.y, b.y) && SameBits(a.z, b.z) && SameBits(a.w, b.w);
}

Quaternion Q(const Vector4f& v) {
    Quaternion q;
    q.x = v.x;
    q.y = v.y;
    q.z = v.z;
    q.w = v.w;
    return q;
}

/// Whether a quaternion comes back to the bit through a matrix.
bool ThroughMatrix(const Vector4f& raw, const Vector3f& t) {
    wem::Transform x;
    x.translation = t;
    x.rotation = Q(raw);
    x.scale = Vector3f{1, 1, 1};
    const wem::Transform back = wem::FromMatrix(wem::ToMatrix(x));
    Quaternion q = back.rotation;
    if ((q.w < 0) != (raw.w < 0)) {
        q = Quaternion{} - q;
    }
    return SameQuat(q, raw);
}

std::pair<i32, i32> Edge(i32 a, i32 b) {
    return a < b ? std::pair{a, b} : std::pair{b, a};
}

d3n::BoneStructure Bone(const char* name, i32 parent) {
    d3n::BoneStructure bone;
    bone.szName = name;
    bone.nParentIndex = parent;
    bone.tTransform0.qRotation = Vector4f{0, 0, 0, 1};
    bone.tTransform0.flScale = 1.0f;
    for (d3n::PRSTransform* t : {&bone.tTransform1, &bone.tTransform2, &bone.tTransform3, &bone.tTransform4}) {
        t->qRotation = Vector4f{0, 0, 0, 1};
        t->flScale = 1.0f;
    }
    return bone;
}

d3n::CollisionShape Capsule(i32 lod, f32 mass, i32 flags = 0) {
    d3n::CollisionShape s;
    s.eShapeType = 1;
    s.nLodIndex = lod;
    s.flScaleX = mass;
    s.dwFlags = flags;
    s.vPointA = {0, 0, 0};
    s.vPointB = {0, 0, 0.5f};
    s.flRadius = 0.1f;
    return s;
}

/// A hanging chain: an anchored root (bit 0), a child jointed to it by a
/// shoulder that starts the chain (bit 4), a grandchild by a revolute; the
/// grandchild also has a lod-1 shape, with mass.
d3n::Appearances Chain() {
    d3n::Appearances app;
    app.dwSnoId = 12345;
    app.arBones = {Bone("root", -1), Bone("upper", 0), Bone("lower", 1)};
    app.arBones[0].arCollisionShapes = {Capsule(0, 1.0f, 1)};
    app.arBones[1].arCollisionShapes = {Capsule(0, 2.0f)};
    app.arBones[2].arCollisionShapes = {Capsule(0, 1.0f), Capsule(1, 3.0f)};
    d3n::ConstraintParameters shoulder;
    shoulder.eConstraintType = 1;
    shoulder.dwFlags = 0x10 | 0xE;
    shoulder.tFrameB.qRotation = Vector4f{0, 0, 0, 1};
    shoulder.tFrameB.vTranslation = {0, 0, 0.5f};
    shoulder.tFrameC.qRotation = Vector4f{0, 0, 0, 1};
    shoulder.flConeAngle = 0.5f;
    shoulder.flTwistLower = -0.25f;
    shoulder.flTwistUpper = 0.25f;
    app.arBones[1].arConstraints = {shoulder};
    d3n::ConstraintParameters hinge = shoulder;
    hinge.eConstraintType = 0;
    hinge.dwFlags = 0x2;
    hinge.flLimitLower = -1.0f;
    hinge.flLimitUpper = 0.5f;
    app.arBones[2].arConstraints = {hinge};
    return app;
}

} // namespace

TEST_CASE("wem physics a D3 appearance's two rigs come in as the client builds them", "[wem][physics][d3]") {
    const d3n::Appearances app = Chain();
    wem::D3Converter converter;
    const wem::Result<wem::Document> imported = converter.fromAppearance(app, nullptr);
    REQUIRE(imported.ok());
    const wem::PhysicsSet& physics = imported->models[0].physics;
    REQUIRE(physics.rigs.size() == 2);
    const wem::PhysicsRig& anchored = physics.rigs[0];
    const wem::PhysicsRig& collapse = physics.rigs[1];
    CHECK(anchored.name == "anchored");
    CHECK(anchored.start == wem::RigStart::Always);
    CHECK(collapse.start == wem::RigStart::OnDeath);
    // The anchored rig: the root kinematic, both others dynamic.
    REQUIRE(anchored.bodies.size() == 3);
    CHECK(physics.body(anchored.bodies[0])->motion == wem::BodyMotion::Kinematic);
    CHECK(physics.body(anchored.bodies[1])->motion == wem::BodyMotion::Dynamic);
    CHECK(physics.body(anchored.bodies[2])->motion == wem::BodyMotion::Dynamic);
    // No `.phy`: the default class density times the shape's mass scale.
    CHECK(physics.body(anchored.bodies[1])->shapes[0].material.density == d3n::densityForClass(0) * 2.0f);
    CHECK(physics.body(anchored.bodies[1])->shapes[0].points.size() == 2); // a capsule by its ends
    // The collapse: every bodied bone with a lod-1 shape; only the grandchild has one.
    REQUIRE(collapse.bodies.size() == 1);
    CHECK(physics.body(collapse.bodies[0])->motion == wem::BodyMotion::Dynamic);
    // Two joints, both in the anchored rig; the collapse has no bone-list joints.
    REQUIRE(physics.joints.size() == 2);
    const wem::PhysicsJoint& cone = physics.joints[0];
    CHECK(cone.kind == wem::JointKind::ConeTwist);
    CHECK(cone.bodyA == anchored.bodies[0]);
    CHECK(cone.bodyB == anchored.bodies[1]);
    CHECK(cone.limitEnabled);
    CHECK(cone.cone == 0.5f);
    // The shoulder pre-rotation: identity times (-1/2,-1/2,-1/2,1/2) turns +X to +Z.
    const wem::Transform frame = wem::FromMatrix(cone.frameA);
    const Vector3f x = frame.rotation.rotate_vector(Vector3f{1, 0, 0});
    CHECK(x.z == Catch::Approx(1.0f).margin(1e-6));
    CHECK(cone.frameA.data[3][2] == 0.5f);
    const wem::PhysicsJoint& hinge = physics.joints[1];
    CHECK(hinge.kind == wem::JointKind::Revolute);
    CHECK(hinge.limitEnabled);
    CHECK(hinge.lower == -1.0f);
    CHECK_FALSE(wem::Validate(*imported, wem::ValidateLevel::Structural).hasErrors());
}

TEST_CASE("wem physics a D3 chain without its start flag does not swing", "[wem][physics][d3]") {
    d3n::Appearances app = Chain();
    app.arBones[1].arConstraints[0].dwFlags = 0xE; // no bit 4: the anchor's child stays out
    app.arBones[2].arCollisionShapes = {Capsule(0, 1.0f)};
    wem::D3Converter converter;
    const wem::Result<wem::Document> imported = converter.fromAppearance(app, nullptr);
    REQUIRE(imported.ok());
    // Nothing dynamic joins, so there is no anchored rig, and no lod-1 shape means no collapse.
    CHECK(imported->models[0].physics.empty());
}

TEST_CASE("wem physics a D3 weld's spring is the one the client makes", "[wem][physics][d3]") {
    d3n::Appearances app = Chain();
    d3n::ConstraintParameters& weld = app.arBones[1].arConstraints[0];
    weld.eConstraintType = 3;
    weld.flParam06 = 0.6f;
    wem::D3Converter converter;
    const wem::Result<wem::Document> imported = converter.fromAppearance(app, nullptr);
    REQUIRE(imported.ok());
    const wem::PhysicsJoint& joint = imported->models[0].physics.joints[0];
    CHECK(joint.kind == wem::JointKind::Weld);
    CHECK(joint.angularSpring.hz == Catch::Approx(0.4f * 15.0f + 1.0f));
    CHECK(joint.angularSpring.damping == 0.9f);
}

TEST_CASE("wem physics G-D1 every D3 appearance's rigs come in", "[wem][physics][d3][corpus]") {
    const fs::path root = test::d3::corpusRoot();
    if (!fs::is_directory(root / "Appearances")) {
        SKIP("D3 corpus not found");
    }
    test::d3::CorpusProvider provider(root);
    wem::AssetSource assets(provider);
    wem::D3Converter converter;
    std::map<std::string, std::size_t> n;
    std::vector<std::string> samples;
    const auto fail = [&](const std::string& what, const fs::path& path) {
        if (n["FAIL " + what]++ < 3) {
            samples.push_back(what + ": " + path.filename().string());
        }
    };
    for (const auto& entry : fs::directory_iterator(root / "Appearances")) {
        const std::vector<u8> bytes = test::d3::readWhole(entry.path());
        const std::optional<d3n::Appearances> app = d3n::parseAppearances(bytes);
        if (!app.has_value()) {
            continue;
        }
        const bool anyShape = std::any_of(app->arBones.begin(), app->arBones.end(),
                                          [](const d3n::BoneStructure& b) { return !b.arCollisionShapes.empty(); });
        bool anyCloth = false;
        for (const d3n::GeoSet* set : {&app->tGeoSet0, &app->tGeoSet1}) {
            for (const d3n::SubObject& sub : set->arSubObjects) {
                anyCloth = anyCloth || !sub.arClothData.empty();
            }
        }
        if (!anyShape && !anyCloth) {
            continue;
        }
        n["appearances with shapes"] += anyShape ? 1 : 0;
        n["appearances with cloth blocks"] += anyCloth ? 1 : 0;
        const wem::Result<wem::Document> imported = converter.fromAppearance(*app, &assets);
        if (!imported.ok()) {
            fail("import", entry.path());
            continue;
        }
        const wem::Model& model = imported->models[0];
        const wem::PhysicsSet& physics = model.physics;
        const wem::Diagnostics report = wem::Validate(*imported, wem::ValidateLevel::Structural);
        for (const wem::Diagnostic& d : report.bySeverity(wem::Severity::Error)) {
            if (d.code == wem::DiagCode::PhysicsReferenceInvalid || d.code == wem::DiagCode::PhysicsShapeDegenerate ||
                d.code == wem::DiagCode::ClothTopologyInvalid) {
                fail("validate: " + d.message, entry.path());
            }
        }
        // The collapse, re-derived: a body for every bone with a fixture at lod 1.
        std::size_t expectCollapse = 0, expectDynamic = 0;
        for (const d3n::BoneStructure& bone : app->arBones) {
            if (expectCollapse >= 64) {
                break;
            }
            bool fixture = false;
            for (const d3n::CollisionShape& s : bone.arCollisionShapes) {
                if (s.nLodIndex != 1) {
                    continue;
                }
                fixture = fixture || s.eShapeType == 0 || s.eShapeType == 1 ||
                          (s.eShapeType == 2 && d3n::readPolytope(s.arPolytopeData, bytes).has_value());
            }
            if (fixture) {
                ++expectCollapse;
                expectDynamic += d3n::boneIsDynamic(bone, 1) ? 1 : 0;
            }
        }
        const wem::PhysicsRig* collapse = nullptr;
        const wem::PhysicsRig* anchored = nullptr;
        for (const wem::PhysicsRig& rig : physics.rigs) {
            (rig.name == "collapse" ? collapse : anchored) = &rig;
        }
        const std::size_t gotCollapse = collapse != nullptr ? collapse->bodies.size() : 0;
        if (gotCollapse != expectCollapse) {
            fail("collapse body count", entry.path());
        }
        if (collapse != nullptr) {
            ++n["collapse rigs"];
            std::size_t dynamic = 0;
            for (const u32 id : collapse->bodies) {
                dynamic += physics.body(id)->motion == wem::BodyMotion::Dynamic ? 1 : 0;
            }
            n["collapse rigs with a dynamic body"] += dynamic > 0 ? 1 : 0;
            if (dynamic != expectDynamic) {
                fail("collapse dynamic count", entry.path());
            }
        }
        if (anchored != nullptr) {
            ++n["anchored rigs"];
            const bool hasAnchor = std::any_of(app->arBones.begin(), app->arBones.end(),
                                               [](const d3n::BoneStructure& b) { return d3n::boneIsAnchor(b); });
            if (!hasAnchor) {
                fail("anchored rig without an anchor", entry.path());
            }
        }
        // Every body's shapes carry the class density times their mass scale;
        // with no `.phy` here, the default class.
        for (const wem::PhysicsBody& body : physics.bodies) {
            ++n["bodies"];
            n["shapes"] += body.shapes.size();
            for (const wem::PhysicsShape& shape : body.shapes) {
                n["hulls"] += shape.kind == wem::PhysicsShapeKind::ConvexHull ? 1 : 0;
                if (shape.material.friction != 0.3f) {
                    fail("default friction", entry.path());
                }
            }
        }
        n["joints"] += physics.joints.size();

        // Cloth, re-derived: a cloth per sub-object whose look names a `.clt`
        // with mass, its cage one vertex per particle, its pins the staples.
        const wem::ProfileMaterialSet* set = model.setFor(wem::ProfileId::Diablo3);
        const u32 look = set != nullptr ? set->defaultLook : 0;
        std::vector<const d3n::ClothStructure*> expected;
        for (const d3n::GeoSet* geoSet : {&app->tGeoSet0, &app->tGeoSet1}) {
            for (const d3n::SubObject& sub : geoSet->arSubObjects) {
                if (sub.arClothData.empty()) {
                    continue;
                }
                for (const d3n::AppearanceMaterial& material : app->arMaterials) {
                    if (material.szName != sub.szName || look >= material.arVariants.size()) {
                        continue;
                    }
                    const d3n::AssetRef& ref = material.arVariants[look].snoCloth;
                    const d3n::Cloth* clt = ref.valid() ? assets.cloth(ref.id) : nullptr;
                    if (clt != nullptr && clt->flMass >= 1e-6f) {
                        expected.push_back(&sub.arClothData.front());
                    }
                    break;
                }
            }
        }
        if (physics.cloths.size() != expected.size()) {
            fail("cloth count", entry.path());
        }
        for (std::size_t c = 0; c < physics.cloths.size() && c < expected.size(); ++c) {
            const wem::Cloth& cloth = physics.cloths[c];
            const wem::Mesh& mesh = model.meshes[cloth.cage.mesh];
            std::set<u32> particles;
            const wem::geom::FaceSet& faces = mesh.faceSet();
            const std::span<const u32> sections = mesh.faceSections();
            std::size_t corner = 0;
            for (std::size_t f = 0; f < faces.faceValence.size(); ++f) {
                const u32 valence = faces.faceValence[f];
                if (f < sections.size() && sections[f] == cloth.cage.section) {
                    for (u32 k = 0; k < valence; ++k) {
                        particles.insert(faces.cornerVertex[corner + k]);
                    }
                }
                corner += valence;
            }
            const auto movable = mesh.attributes.get<u8>(wem::geom::names::kClothMovable, wem::geom::Domain::Vertex);
            std::size_t pinned = 0;
            for (const u32 v : particles) {
                pinned += v < movable.size() && movable[v] == 0 ? 1 : 0;
            }
            // The mesh repair splits a non-manifold particle, and drops one no
            // face uses: the general WEM mesh loss (WEM_PHYSICS_DESIGN.md §10.3).
            n["cages the repair changed"] += particles.size() != expected[c]->arVertices.size() ? 1 : 0;
            n["cages over 256 particles"] += particles.size() > 256 ? 1 : 0;
            n["cloth particles (file)"] += expected[c]->arVertices.size();
            n["cloth particles (cage)"] += particles.size();
            n["cloth pins (file)"] += expected[c]->arStaples.size();
            n["cloth pins (cage)"] += pinned;
            ++n["cloths"];
        }
        n["capsule colliders"] += app->arCollisionCapsules.size();
        std::size_t capsules = 0;
        for (const wem::ClothCollider& collider : physics.colliders) {
            capsules += collider.kind == wem::ClothColliderKind::Capsule ? 1 : 0;
            n["plane colliders"] += collider.kind == wem::ClothColliderKind::Plane ? 1 : 0;
        }
        if (capsules != app->arCollisionCapsules.size()) {
            fail("capsule colliders", entry.path());
        }
    }
    for (const auto& [k, v] : n) {
        std::printf("G-D1 %-40s %zu\n", k.c_str(), v);
    }
    for (const auto& s : samples) {
        std::printf("  e.g. %s\n", s.c_str());
    }
    CHECK(n["appearances with shapes"] > 0);
    for (const auto& [k, v] : n) {
        CHECK_FALSE(k.rfind("FAIL", 0) == 0);
    }
}

TEST_CASE("wem physics a D3 actor's rigs take its .phy", "[wem][physics][d3][corpus]") {
    const fs::path root = test::d3::corpusRoot();
    if (!fs::is_directory(root / "Actor") || !fs::is_directory(root / "Physics")) {
        SKIP("D3 corpus not found");
    }
    test::d3::CorpusProvider provider(root);
    wem::AssetSource assets(provider);
    wem::D3Converter converter;
    wem::D3ImportOptions options;
    options.importAnimation = false;
    options.attachmentDepth = 0;
    std::size_t actors = 0, bodies = 0, mismatched = 0;
    for (const auto& entry : fs::directory_iterator(root / "Actor")) {
        const std::vector<u8> bytes = test::d3::readWhole(entry.path());
        const std::optional<d3n::Actor> actor = d3n::parseActor(bytes);
        if (!actor.has_value() || !actor->snoPhysics.valid()) {
            continue;
        }
        const d3n::Physics* phy = assets.physics(actor->snoPhysics.id);
        const d3n::Appearances* app = actor->snoAppearance.valid() ? assets.appearance(actor->snoAppearance.id) : nullptr;
        if (phy == nullptr || app == nullptr ||
            std::none_of(app->arBones.begin(), app->arBones.end(),
                         [](const d3n::BoneStructure& b) { return !b.arCollisionShapes.empty(); })) {
            continue;
        }
        const wem::Result<wem::Document> imported = converter.fromActor(*actor, assets, options);
        if (!imported.ok()) {
            continue;
        }
        ++actors;
        for (const wem::PhysicsBody& body : imported->models[0].physics.bodies) {
            ++bodies;
            bool same = body.linearDamping == phy->flLinearDamping && body.angularDamping == phy->flAngularDamping;
            for (const wem::PhysicsShape& shape : body.shapes) {
                same = same && shape.material.friction == phy->flFriction &&
                       shape.material.restitution == phy->flRestitution;
            }
            mismatched += same ? 0 : 1;
        }
        if (actors >= 400) {
            break;
        }
    }
    std::printf("D3 actors with a .phy: %zu, %zu bodies\n", actors, bodies);
    CHECK(actors > 0);
    CHECK(bodies > 0);
    CHECK(mismatched == 0);
}

TEST_CASE("wem physics d3 census", "[.d3census]") {
    const fs::path root = test::d3::corpusRoot() / "Appearances";
    if (!fs::is_directory(root)) {
        SKIP("D3 corpus not found");
    }
    std::map<std::string, std::size_t> n;
    std::map<std::string, std::map<std::string, std::size_t>> hist;
    const auto note = [&](const std::string& what, const std::string& value) { ++hist[what][value]; };
    const auto num = [](f32 v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.6g", v);
        return std::string(b);
    };
    for (const auto& entry : fs::directory_iterator(root)) {
        const std::vector<u8> bytes = test::d3::readWhole(entry.path());
        const std::optional<d3n::Appearances> app = d3n::parseAppearances(bytes);
        if (!app.has_value()) {
            continue;
        }
        ++n["appearances"];
        for (const d3n::BoneStructure& bone : app->arBones) {
            for (const d3n::CollisionShape& s : bone.arCollisionShapes) {
                ++n["shapes"];
                note("shape type/lod", std::to_string(s.eShapeType) + "/" + std::to_string(s.nLodIndex));
                note("shape flags", std::to_string(s.dwFlags));
                note("shape unknown08", std::to_string(s.dwUnknown08));
                note("scaleY", num(s.flScaleY));
                note("scaleZ", num(s.flScaleZ));
            }
            n["bones with 2+ constraints"] += bone.arConstraints.size() > 1 ? 1 : 0;
            for (std::size_t k = 0; k < bone.arConstraints.size(); ++k) {
                const d3n::ConstraintParameters& c = bone.arConstraints[k];
                ++n["bone constraints"];
                note("constraint type", std::to_string(c.eConstraintType));
                note("constraint flags", std::to_string(c.dwFlags));
                n["frameB raw exact"] += ThroughMatrix(c.tFrameB.qRotation, c.tFrameB.vTranslation) ? 1 : 0;
                n["frameC raw exact"] += ThroughMatrix(c.tFrameC.qRotation, c.tFrameC.vTranslation) ? 1 : 0;
                const f32 qn = std::sqrt(c.tFrameB.qRotation.x * c.tFrameB.qRotation.x +
                                         c.tFrameB.qRotation.y * c.tFrameB.qRotation.y +
                                         c.tFrameB.qRotation.z * c.tFrameB.qRotation.z +
                                         c.tFrameB.qRotation.w * c.tFrameB.qRotation.w);
                n["frameB unit to 1e-6"] += std::abs(qn - 1.0f) < 1e-6f ? 1 : 0;
                if (c.eConstraintType == 3) {
                    note("weld p06/p07", std::string(c.flParam06 > 0 ? "p06" : "-") + (c.flParam07 > 0 ? "p07" : "-"));
                }
                note("p05 (break force)", c.flParam05 == 0 ? "0" : "set");
                note("p09/p10", c.flParam09 == 0 && c.flParam10 == 0 ? "0" : "set");
                note("p11 (break torque)", c.flParam11 == 0 ? "0" : "set");
                note("p12", c.flParam12 == 0 ? "0" : "set");
                note("break effect", c.szBreakEffect.empty() ? "none" : "set");
            }
        }
        n["appearance constraints"] += app->arConstraints.size();
        for (const d3n::ConstraintParameters& c : app->arConstraints) {
            note("app constraint type", std::to_string(c.eConstraintType));
        }
        for (const d3n::GeoSet* set : {&app->tGeoSet0, &app->tGeoSet1}) {
            for (const d3n::SubObject& sub : set->arSubObjects) {
                n["subobject shapes"] += sub.arCollisionShapes.size();
                for (const d3n::ClothStructure& cs : sub.arClothData) {
                    ++n["cloth blocks"];
                    const std::size_t nv = cs.arVertices.size();
                    // Edges of the faces, in a first-seen order.
                    std::vector<std::pair<i32, i32>> edges;
                    std::set<std::pair<i32, i32>> edgeSet;
                    std::map<std::pair<i32, i32>, std::vector<i32>> opposite;
                    for (const d3n::ClothFace& f : cs.arFaces) {
                        const i32 v[3] = {f.nVertex0, f.nVertex1, f.nVertex2};
                        for (int k = 0; k < 3; ++k) {
                            const auto e = Edge(v[k], v[(k + 1) % 3]);
                            if (edgeSet.insert(e).second) {
                                edges.push_back(e);
                            }
                            opposite[e].push_back(v[(k + 2) % 3]);
                        }
                        const Vector3f& a = cs.arVertices[static_cast<std::size_t>(v[0])].vPosition;
                        const Vector3f& b = cs.arVertices[static_cast<std::size_t>(v[1])].vPosition;
                        const Vector3f& c = cs.arVertices[static_cast<std::size_t>(v[2])].vPosition;
                        const Vector3f u{b.x - a.x, b.y - a.y, b.z - a.z};
                        const Vector3f w{c.x - a.x, c.y - a.y, c.z - a.z};
                        const Vector3f x{u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x};
                        const f32 area = 0.5f * std::sqrt(x.x * x.x + x.y * x.y + x.z * x.z);
                        n["face area exact"] += SameBits(area, f.flRestArea) ? 1 : 0;
                        n["face area 1e-5"] += std::abs(area - f.flRestArea) <= 1e-5f * std::max(1.0f, area) ? 1 : 0;
                        ++n["faces"];
                    }
                    std::set<std::pair<i32, i32>> stretch;
                    std::vector<std::pair<i32, i32>> stretchOrder;
                    for (const d3n::ClothConstraint& c : cs.arStretchConstraints) {
                        stretch.insert(Edge(c.nVertex0, c.nVertex1));
                        stretchOrder.push_back(Edge(c.nVertex0, c.nVertex1));
                        const Vector3f& a = cs.arVertices[static_cast<std::size_t>(c.nVertex0)].vPosition;
                        const Vector3f& b = cs.arVertices[static_cast<std::size_t>(c.nVertex1)].vPosition;
                        const f32 dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
                        const f32 d2 = dx * dx + dy * dy + dz * dz;
                        n["stretch rest exact"] += SameBits(d2, c.flRestLengthSq) ? 1 : 0;
                        n["stretch rest 1e-5"] += std::abs(d2 - c.flRestLengthSq) <= 1e-5f * std::max(1.0f, d2) ? 1 : 0;
                        ++n["stretch"];
                        note("stretch blend", num(c.flStiffnessBlend));
                        const f32 m0 = cs.arVertices[static_cast<std::size_t>(c.nVertex0)].flInvMass;
                        const f32 m1 = cs.arVertices[static_cast<std::size_t>(c.nVertex1)].flInvMass;
                        n["stretch weights = invMass"] += (SameBits(c.flWeight0, m0) && SameBits(c.flWeight1, m1)) ? 1 : 0;
                        n["stretch weights = invMass/sum"] +=
                            (m0 + m1 > 0 && std::abs(c.flWeight0 - m0 / (m0 + m1)) < 1e-6f) ? 1 : 0;
                    }
                    for (const auto& e : edgeSet) {
                        if (stretch.count(e) == 0) {
                            const bool pinned0 = cs.arVertices[static_cast<std::size_t>(e.first)].flInvMass == 0.0f;
                            const bool pinned1 = cs.arVertices[static_cast<std::size_t>(e.second)].flInvMass == 0.0f;
                            note("edge without stretch: pinned ends", std::to_string(int(pinned0) + int(pinned1)));
                        }
                    }
                    n["cloth stretch == face edges (set)"] += stretch == edgeSet ? 1 : 0;
                    n["cloth stretch == face edges (order)"] += stretchOrder == edges ? 1 : 0;
                    n["cloth stretch subset of edges"] +=
                        std::includes(edgeSet.begin(), edgeSet.end(), stretch.begin(), stretch.end()) ? 1 : 0;
                    std::set<std::pair<i32, i32>> bendPred;
                    for (const auto& [e, opp] : opposite) {
                        if (opp.size() == 2) {
                            bendPred.insert(Edge(opp[0], opp[1]));
                        }
                    }
                    // Two hops over the face edges.
                    std::map<i32, std::set<i32>> ring;
                    for (const auto& e : edgeSet) {
                        ring[e.first].insert(e.second);
                        ring[e.second].insert(e.first);
                    }
                    std::set<std::pair<i32, i32>> bend;
                    for (const d3n::ClothConstraint& c : cs.arBendConstraints) {
                        const auto e = Edge(c.nVertex0, c.nVertex1);
                        bend.insert(e);
                        const Vector3f& a = cs.arVertices[static_cast<std::size_t>(c.nVertex0)].vPosition;
                        const Vector3f& b = cs.arVertices[static_cast<std::size_t>(c.nVertex1)].vPosition;
                        const f32 dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
                        const f32 d2 = dx * dx + dy * dy + dz * dz;
                        n["bend rest exact"] += SameBits(d2, c.flRestLengthSq) ? 1 : 0;
                        n["bend rest exact (zyx)"] += SameBits(dz * dz + dy * dy + dx * dx, c.flRestLengthSq) ? 1 : 0;
                        const f64 ddx = f64(b.x) - a.x, ddy = f64(b.y) - a.y, ddz = f64(b.z) - a.z;
                        n["bend rest exact (double)"] += SameBits(f32(ddx * ddx + ddy * ddy + ddz * ddz), c.flRestLengthSq) ? 1 : 0;
                        const f32 len = std::sqrt(d2);
                        n["bend rest exact (sqrt, squared)"] += SameBits(len * len, c.flRestLengthSq) ? 1 : 0;
                        note("bend rest / d2", num(c.flRestLengthSq / (d2 > 0 ? d2 : 1)));
                        const bool opp = bendPred.count(e) != 0;
                        bool twoHop = false;
                        for (const i32 mid : ring[e.first]) {
                            twoHop = twoHop || ring[mid].count(e.second) != 0;
                        }
                        const bool edge = edgeSet.count(e) != 0;
                        note("bend pair is", std::string(opp ? "opposite " : "") + (twoHop ? "two-hop " : "") + (edge ? "edge" : ""));
                        const bool p0 = cs.arVertices[static_cast<std::size_t>(c.nVertex0)].flInvMass == 0.0f;
                        const bool p1 = cs.arVertices[static_cast<std::size_t>(c.nVertex1)].flInvMass == 0.0f;
                        note("bend pinned ends", std::to_string(int(p0) + int(p1)));
                        ++n["bend"];
                        note("bend blend", num(c.flStiffnessBlend));
                    }
                    for (const auto& e : bendPred) {
                        if (bend.count(e) == 0) {
                            const bool p0 = cs.arVertices[static_cast<std::size_t>(e.first)].flInvMass == 0.0f;
                            const bool p1 = cs.arVertices[static_cast<std::size_t>(e.second)].flInvMass == 0.0f;
                            note("opposite pair without bend: pinned ends", std::to_string(int(p0) + int(p1)));
                        }
                    }
                    n["cloth bend == opposite pairs (set)"] += bend == bendPred ? 1 : 0;
                    // Pins, proxies, pin distance by hops over the stretch edges.
                    std::vector<std::vector<i32>> adjacent(nv);
                    for (const auto& e : stretch) {
                        adjacent[static_cast<std::size_t>(e.first)].push_back(e.second);
                        adjacent[static_cast<std::size_t>(e.second)].push_back(e.first);
                    }
                    std::vector<i32> hops(nv, -1);
                    std::vector<i32> queue;
                    for (std::size_t v = 0; v < nv; ++v) {
                        if (cs.arVertices[v].flInvMass == 0.0f) {
                            hops[v] = 0;
                            queue.push_back(static_cast<i32>(v));
                        }
                    }
                    for (std::size_t q = 0; q < queue.size(); ++q) {
                        for (const i32 w : adjacent[static_cast<std::size_t>(queue[q])]) {
                            if (hops[static_cast<std::size_t>(w)] < 0) {
                                hops[static_cast<std::size_t>(w)] = hops[static_cast<std::size_t>(queue[q])] + 1;
                                queue.push_back(w);
                            }
                        }
                    }
                    std::size_t pinOk = 0, proxySelf = 0, meshFirst = 0;
                    std::vector<i32> firstMesh(nv, -1);
                    for (std::size_t mv = 0; mv < sub.arVertices.size(); ++mv) {
                        const u32 cv = sub.arVertices[mv].dwClothVertexIndex;
                        if (cv < nv && firstMesh[cv] < 0) {
                            firstMesh[cv] = static_cast<i32>(mv);
                        }
                    }
                    for (std::size_t v = 0; v < nv; ++v) {
                        const d3n::ClothVertex& cv = cs.arVertices[v];
                        pinOk += cv.nPinDistance == hops[v] ? 1 : 0;
                        proxySelf += cv.nCollisionProxyVertex == static_cast<i32>(v) ? 1 : 0;
                        meshFirst += cv.nMeshVertexIndex == firstMesh[v] ? 1 : 0;
                        note("invMass", num(cv.flInvMass));
                        note("proxy - self", std::to_string(cv.nCollisionProxyVertex - static_cast<i32>(v)));
                        note("mesh vertex vs first user", cv.nMeshVertexIndex == firstMesh[v] ? "first"
                                                          : cv.nMeshVertexIndex < 0 ? "negative"
                                                                                    : "other");
                        note("driving bone", std::to_string(cv.nDrivingBone));
                        note("pinDistance - hops", std::to_string(cv.nPinDistance - hops[v]));
                    }
                    n["cloth vertices"] += nv;
                    n["pin distance = hops"] += pinOk;
                    n["proxy = self"] += proxySelf;
                    n["mesh vertex = first user"] += meshFirst;
                    bool staplesInOrder = true;
                    for (std::size_t s = 0; s < cs.arStaples.size(); ++s) {
                        staplesInOrder = staplesInOrder && cs.arStaples[s].nVertexIndex == static_cast<i32>(s);
                    }
                    n["staples index-ordered from 0"] += staplesInOrder ? 1 : 0;
                    note("driving bone count", std::to_string(cs.dwDrivingBoneCount));
                    note("external force scale", num(cs.flExternalForceScale));
                }
            }
        }
    }
    for (const auto& [k, v] : n) {
        std::printf("%-40s %zu\n", k.c_str(), v);
    }
    for (const auto& [k, values] : hist) {
        std::printf("%s:", k.c_str());
        std::size_t shown = 0;
        std::vector<std::pair<std::size_t, std::string>> sorted;
        for (const auto& [value, count] : values) {
            sorted.emplace_back(count, value);
        }
        std::sort(sorted.rbegin(), sorted.rend());
        for (const auto& [count, value] : sorted) {
            if (shown++ >= 10) {
                break;
            }
            std::printf(" %s=%zu", value.c_str(), count);
        }
        std::printf("%s\n", values.size() > 10 ? " ..." : "");
    }
}
