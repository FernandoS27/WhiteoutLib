// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// World of Warcraft physics through WEM (WEM_PHYSICS_DESIGN.md §8.1): a small
// model's records both ways, the angle inverse, and the corpus gates -- G-W1,
// every physics model `m2 -> WEM -> .wem -> WEM -> m2` comes back with its live
// fields equal to what the client builds from the source and its dead ones at
// the upgrader's values; G-WC, every shipped `PLYT` re-cooks to itself.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <whiteout/models/m2/m2.h>
#include <whiteout/models/m2/phys_file.h>
#include <whiteout/models/m3/physics_cook.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/writer.h>
#include <whiteout/utils/os_file_system.h>

#include "whiteout/models/wem/converters/m2_physics.h"
#include "test_helpers.h"
#include "wem_corpus_files.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::models::wem;
using Catch::Approx;

namespace {

bool SameBits(f32 a, f32 b) {
    return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

bool SameBits(const Vector3f& a, const Vector3f& b) {
    return SameBits(a.x, b.x) && SameBits(a.y, b.y) && SameBits(a.z, b.z);
}

bool SameBits(const m2::PhysicsFrame& a, const m2::PhysicsFrame& b) {
    return SameBits(a.axisX, b.axisX) && SameBits(a.axisY, b.axisY) && SameBits(a.axisZ, b.axisZ) &&
           SameBits(a.origin, b.origin);
}

m2::PhysicsFrame Frame(const Vector3f& origin) {
    return m2::PhysicsFrame{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, origin};
}

/// Two bones, a kinematic anchor and a dynamic body hanging off it by a
/// legacy shoulder joint, in a version-4 file.
m2::Model WowFixture() {
    m2::Model model;
    model.bones.resize(2);
    model.bones[0].parentBoneId = -1;
    model.bones[0].pivot = {0, 0, 1};
    model.bones[1].parentBoneId = 0;
    model.bones[1].pivot = {0, 0.5f, 1};
    model.bones[1].flags = static_cast<u32>(m2::BoneFlag::Kinematic);

    m2::PhysicsData data;
    data.version = 4;
    data.phyt = 1u;
    m2::PhysicsBody anchor;
    anchor.type = m2::PhysicsBodyType::Kinematic;
    anchor.boneIndex = 0;
    anchor.position = {0, 0, 1};
    anchor.shapeIndex = 0;
    anchor.shapeCount = 1;
    anchor.followFactor = 0.5f;
    anchor.attachment = m2::kPhysicsAttachmentHasChildren | m2::kPhysicsAttachmentRagdollRoot;
    m2::PhysicsBody hanging = anchor;
    hanging.type = m2::PhysicsBodyType::Dynamic;
    hanging.boneIndex = 1;
    hanging.position = {0.1f, 0.5f, 1};
    hanging.shapeIndex = 1;
    hanging.shapeCount = 2;
    hanging.gravityScale = 0.75f;
    hanging.linearDamping = 3.0f;
    hanging.angularDamping = 10.0f;
    hanging.attachment = 0; // hangs off body 0
    data.bodies = {anchor, hanging};

    m2::PhysicsShape sphere;
    sphere.shapeType = m2::PhysicsShapeType::Sphere;
    sphere.shapeIndex = 0;
    sphere.density = 2.0f;
    sphere.friction = 0.5f;
    sphere.unused14 = 7.0f; // dead
    m2::PhysicsShape capsule = sphere;
    capsule.shapeType = m2::PhysicsShapeType::Capsule;
    m2::PhysicsShape hull = sphere;
    hull.shapeType = m2::PhysicsShapeType::Polytope;
    data.shapes = {sphere, capsule, hull};
    data.sphereShapes.push_back(m2::SphereShape{{0.1f, 0.2f, 0.3f}, 0.25f});
    data.capsuleShapes.push_back(m2::CapsuleShape{{0, 0, 0}, {0.3f, -0.1f, 0.7f}, 0.1f});
    m2::PolytopeShape box;
    for (int i = 0; i < 8; ++i) {
        box.vertices.push_back({(i & 1) ? 0.2f : -0.2f, (i & 2) ? 0.1f : -0.1f, (i & 4) ? 0.4f : 0.0f});
    }
    data.polytopeShapes.push_back(box);

    m2::ShoulderJoint shoulder;
    shoulder.frameA = Frame({0, 0.5f, 0});
    shoulder.frameB = Frame({0, 0, 0});
    shoulder.lowerTwistAngle = -20.0f;
    shoulder.upperTwistAngle = 10.0f;
    shoulder.coneAngle = 45.0f;
    shoulder.motorMode = 1;
    shoulder.maxMotorTorque = 3.0f;
    data.shoulderJoints.push_back(shoulder); // a SHOJ: the spring is the upgrader's
    m2::PhysicsJoint link;
    link.bodyAIndex = 0;
    link.bodyBIndex = 1;
    link.jointType = m2::PhysicsJointType::Shoulder;
    link.jointId = 0;
    data.joints.push_back(link);
    model.physics = data;
    return model;
}

} // namespace

TEST_CASE("wem physics a degree comes back to the same radians", "[wem][physics][m2]") {
    for (f32 d = -180.0f; d <= 180.0f; d += 0.37f) {
        const f32 r = m2_physics::ToRadians(d);
        CHECK(SameBits(m2_physics::ToRadians(m2_physics::ToDegrees(r)), r));
    }
    CHECK(m2_physics::ToDegrees(m2_physics::ToRadians(45.0f)) == 45.0f);
}

TEST_CASE("wem physics a WoW model's records cross both ways", "[wem][physics][m2]") {
    const m2::Model source = WowFixture();
    M2Converter converter;
    const Result<Document> imported = converter.fromM2(source, 274);
    REQUIRE(imported.ok());
    const PhysicsSet& physics = imported->models[0].physics;
    REQUIRE(physics.bodies.size() == 2);
    REQUIRE(physics.joints.size() == 1);
    REQUIRE(physics.rigs.size() == 1);

    const PhysicsBody& anchor = physics.bodies[0];
    const PhysicsBody& hanging = physics.bodies[1];
    CHECK(anchor.motion == BodyMotion::Kinematic);
    CHECK(hanging.motion == BodyMotion::Dynamic);
    REQUIRE(anchor.wow.has_value());
    CHECK(anchor.wow->hasChildren);
    CHECK(anchor.wow->ragdollRoot);
    CHECK(anchor.wow->followFactor == 0.5f);
    CHECK(hanging.wow->parent == anchor.id);
    CHECK(hanging.gravityScale == 0.75f);
    REQUIRE(hanging.shapes.size() == 2);
    // Version 4: the capsule's density is what the client makes of it.
    const f32 expected = m2::capsuleDensity(2.0f, source.physics->capsuleShapes[0], 4);
    CHECK(expected != 2.0f);
    CHECK(SameBits(hanging.shapes[0].material.density, expected));
    CHECK(hanging.shapes[1].kind == PhysicsShapeKind::ConvexHull);

    const PhysicsJoint& joint = physics.joints[0];
    CHECK(joint.kind == JointKind::ConeTwist);
    CHECK(joint.limitEnabled);
    CHECK(joint.cone == Approx(0.7853982f));
    CHECK(joint.motor == JointMotorMode::Position);
    CHECK(joint.angularSpring.hz == 1.0f); // the SHOJ upgrade
    CHECK(joint.angularSpring.damping == 0.7f);

    const PhysicsRig& rig = physics.rigs[0];
    CHECK(rig.start == RigStart::Always);
    REQUIRE(rig.wow.has_value());
    CHECK(rig.wow->kind == WowPhysicsKind::WornItem); // PHYT 1 is 0
    CHECK(rig.bodies.size() == 2);

    const Result<m2::Model> exported = converter.toM2(*imported, ProfileId::Wow, 274);
    REQUIRE(exported.ok());
    REQUIRE(exported->physics.has_value());
    const m2::PhysicsData& out = *exported->physics;
    CHECK(out.version == 6);
    CHECK(out.phyt == 0u);
    REQUIRE(out.bodies.size() == 2);
    CHECK(out.bodies[0].attachment == (m2::kPhysicsAttachmentHasChildren | m2::kPhysicsAttachmentRagdollRoot));
    CHECK(out.bodies[1].attachment == 0);
    CHECK(out.bodies[1].position == Vector3f{0, 0.5f, 1}); // the bone's pivot
    REQUIRE(out.shapes.size() == 3);
    CHECK(out.shapes[0].unused14 == 0.0f); // dead, at the upgrader's value
    CHECK(SameBits(out.shapes[1].density, expected));
    REQUIRE(out.sphereShapes.size() == 1);
    CHECK(SameBits(out.sphereShapes[0].localPosition, source.physics->sphereShapes[0].localPosition));
    REQUIRE(out.capsuleShapes.size() == 1);
    CHECK(out.capsuleShapes[0].localPosition2.z == Approx(0.7f));
    REQUIRE(out.polytopeShapes.size() == 1);
    CHECK(out.polytopeShapes[0].vertices.size() == 8);
    CHECK(out.polytopeShapes[0].facePlanes.size() == 6);
    REQUIRE(out.shoulderJoints.size() == 1);
    CHECK(out.shoulderJoints[0].lowerTwistAngle == -20.0f);
    CHECK(out.shoulderJoints[0].coneAngle == 45.0f);
    CHECK(out.shoulderJoints[0].motorFrequencyHz == 1.0f);
    const u32 flags = static_cast<u32>(exported->globalFlags.value);
    CHECK((flags & static_cast<u32>(m2::GlobalFlag::LoadPhysicsData)) != 0);
    CHECK((flags & static_cast<u32>(m2::GlobalFlag::SuppressPhysicsFile)) != 0);
    CHECK((exported->bones[1].flags & static_cast<u32>(m2::BoneFlag::Kinematic)) != 0);
}

TEST_CASE("wem physics a switched-off WoW limit is written as an empty range", "[wem][physics][m2]") {
    m2::Model source = WowFixture();
    M2Converter converter;
    Result<Document> imported = converter.fromM2(source, 274);
    REQUIRE(imported.ok());
    PhysicsJoint& joint = imported->models[0].physics.joints[0];
    joint.limitEnabled = false;
    const Result<m2::Model> exported = converter.toM2(*imported, ProfileId::Wow, 274);
    REQUIRE(exported.ok());
    const m2::ShoulderJoint& out = exported->physics->shoulderJoints[0];
    CHECK_FALSE(out.upperTwistAngle > out.lowerTwistAngle);
    CHECK(exported.diagnostics.countOf(DiagCode::PhysicsJointFieldDropped) == 1);
}

// ============================================================================
// G-W1 and G-WC
// ============================================================================

namespace {

struct Tally {
    std::map<std::string, std::size_t> counts;
    std::map<std::string, std::size_t> known;
    std::vector<std::string> samples;
    void fail(const std::string& what, const fs::path& file) {
        if (counts[what]++ < 3) {
            samples.push_back(what + ": " + file.filename().string());
        }
    }
};

bool Near(f32 a, f32 b, f32 rel, f32 abs) {
    return std::abs(a - b) <= std::max(abs, rel * std::max(std::abs(a), std::abs(b)));
}

/// G-WC: a shipped polytope against a cook of its own vertices.
void CompareCook(const m2::PolytopeShape& shipped, const fs::path& path, Tally& tally, std::size_t& exactTables,
                 std::size_t& otherTopology) {
    const auto note = [&](const char* what) { ++tally.known[std::string("G-WC tables differ in ") + what]; };
    m3::PhysicsShape cooked;
    if (!m3::CookHull(cooked, shipped.vertices).ok) {
        tally.fail("G-WC hull did not cook", path);
        return;
    }
    if (cooked.hullVertices != shipped.vertices || cooked.hullPlanes.size() != shipped.facePlanes.size()) {
        ++otherTopology;
        return;
    }
    bool same = cooked.hullFaceFirstEdges == shipped.faceFirstEdges && cooked.hullHalfEdges.size() == shipped.edges.size();
    for (std::size_t e = 0; same && e < shipped.edges.size(); ++e) {
        const m3::ConvexHullHalfEdge& c = cooked.hullHalfEdges[e];
        const m2::PolytopeHalfEdge& s = shipped.edges[e];
        same = c.twinOffset == s.twinOffset && c.originVertex == s.originVertex && c.face == s.faceIndex &&
               c.nextInFace == s.nextEdge;
    }
    for (std::size_t f = 0; same && f < shipped.facePlanes.size(); ++f) {
        same = cooked.hullPlanes[f] == shipped.facePlanes[f];
    }
    exactTables += same ? 1 : 0;
    if (!same) {
        if (cooked.hullFaceFirstEdges != shipped.faceFirstEdges) {
            note("first edges");
        }
        if (cooked.hullHalfEdges.size() != shipped.edges.size()) {
            note("half-edge count");
        } else {
            for (std::size_t e = 0; e < shipped.edges.size(); ++e) {
                const m3::ConvexHullHalfEdge& c = cooked.hullHalfEdges[e];
                const m2::PolytopeHalfEdge& s = shipped.edges[e];
                if (c.twinOffset != s.twinOffset || c.originVertex != s.originVertex || c.face != s.faceIndex ||
                    c.nextInFace != s.nextEdge) {
                    note("half-edges");
                    break;
                }
            }
        }
        for (std::size_t f = 0; f < shipped.facePlanes.size(); ++f) {
            if (!(cooked.hullPlanes[f] == shipped.facePlanes[f])) {
                note("planes");
                break;
            }
        }
    }
    for (const Vector4f& p : shipped.facePlanes) {
        const bool matched = std::any_of(cooked.hullPlanes.begin(), cooked.hullPlanes.end(), [&](const Vector4f& q) {
            return Near(p.x, q.x, 0, 1e-4f) && Near(p.y, q.y, 0, 1e-4f) && Near(p.z, q.z, 0, 1e-4f) &&
                   Near(p.w, q.w, 1e-4f, 1e-4f);
        });
        if (!matched) {
            tally.fail("G-WC plane unmatched", path);
            break;
        }
    }
    if (!Near(cooked.hullVolume, shipped.volume, 1e-4f, 1e-6f)) {
        tally.fail("G-WC volume", path);
    }
    if (!Near(cooked.hullSurfaceArea, shipped.surfaceArea, 1e-4f, 1e-6f)) {
        tally.fail("G-WC area", path);
    }
    const f32 size = std::sqrt(shipped.surfaceArea);
    if (!Near(cooked.hullCentroid.x, shipped.centroid.x, 0, 1e-4f * size) ||
        !Near(cooked.hullCentroid.y, shipped.centroid.y, 0, 1e-4f * size) ||
        !Near(cooked.hullCentroid.z, shipped.centroid.z, 0, 1e-4f * size)) {
        tally.fail("G-WC centroid", path);
    }
}

struct Drift {
    std::size_t capsules = 0;
    std::size_t exactCapsules = 0;
    f32 worstEnd = 0.0f; ///< Relative to the capsule's length.
    std::size_t angles = 0;
    std::size_t sameDegreeBits = 0;
};

/// Whether @p path carries physics: an inline PFDC or a `.phys` beside it.
bool HasPhysics(const fs::path& path) {
    if (fs::exists(fs::path(path).replace_extension(".phys"))) {
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 8 || std::memcmp(bytes.data(), "MD21", 4) != 0) {
        return false;
    }
    for (std::size_t pos = 0; pos + 8 <= bytes.size();) {
        u32 size = 0;
        std::memcpy(&size, bytes.data() + pos + 4, 4);
        if (std::memcmp(bytes.data() + pos, "PFDC", 4) == 0 || std::memcmp(bytes.data() + pos, "PFID", 4) == 0) {
            return true;
        }
        pos += 8 + static_cast<std::size_t>(size);
    }
    return false;
}

u32 ExpectedKind(const m2::PhysicsData& p) {
    const u32 raw = p.phyt.value_or(0);
    return raw == 1 ? 0u : raw > 4 ? 3u : raw;
}

void CompareAngle(f32 a, f32 b, Drift& drift, bool& ok) {
    ++drift.angles;
    drift.sameDegreeBits += SameBits(a, b) ? 1 : 0;
    ok = ok && SameBits(m2_physics::ToRadians(a), m2_physics::ToRadians(b));
}

/// G-W1: @p b is the round trip of @p a, which is as the parser read it.
void ComparePhysics(const m2::Model& source, const m2::Model& exported, const fs::path& path, Tally& tally,
                    Drift& drift) {
    const m2::PhysicsData& a = *source.physics;
    if (!exported.physics.has_value()) {
        tally.fail("no physics written", path);
        return;
    }
    const m2::PhysicsData& b = *exported.physics;
    if (b.version != 6) {
        tally.fail("version", path);
    }
    if (b.phyt.value_or(99) != ExpectedKind(a)) {
        tally.fail("PHYT", path);
    }
    if (a.tuning.empty() != b.tuning.empty() ||
        (!a.tuning.empty() && std::memcmp(&a.tuning[0], &b.tuning[0], sizeof(m2::PhysicsTuning)) != 0)) {
        tally.fail("PHYV", path);
    }
    const bool keysA = a.allowList.has_value() && !a.allowList->keys.empty();
    if (keysA != b.allowList.has_value() || (keysA && a.allowList->keys != b.allowList->keys)) {
        tally.fail("PHAO", path);
    }
    const u32 flags = static_cast<u32>(exported.globalFlags.value);
    if ((flags & 0x20u) == 0 || (flags & 0x1000000u) == 0 || exported.physicsFileId.has_value()) {
        tally.fail("global flags / inline", path);
    }

    // --- bodies -----------------------------------------------------------------
    if (a.bodies.size() != b.bodies.size()) {
        tally.fail("body count", path);
        return;
    }
    std::size_t pivotMoved = 0;
    for (std::size_t i = 0; i < a.bodies.size(); ++i) {
        const m2::PhysicsBody& x = a.bodies[i];
        const m2::PhysicsBody& y = b.bodies[i];
        const bool kinematic = x.type == m2::PhysicsBodyType::Kinematic;
        const u16 word = kinematic ? static_cast<u16>(x.attachment & 0xC000u) : x.attachment;
        if (x.type != y.type || x.boneIndex != y.boneIndex || x.shapeIndex != y.shapeIndex ||
            x.shapeCount != y.shapeCount || !SameBits(x.gravityScale, y.gravityScale) ||
            !SameBits(x.inertiaScale, y.inertiaScale) || !SameBits(x.linearDamping, y.linearDamping) ||
            !SameBits(x.angularDamping, y.angularDamping) || !SameBits(x.followFactor, y.followFactor) ||
            word != y.attachment) {
            tally.fail("body fields", path);
        }
        if (y.boneIndex < exported.bones.size() && !SameBits(y.position, exported.bones[y.boneIndex].pivot)) {
            tally.fail("body position is not its bone's pivot", path);
        }
        pivotMoved += SameBits(x.position, y.position) ? 0 : 1;
        // A shipped dynamic bone without the kinematic flag animates over its
        // body; the round trip keeps that, as it keeps every bone flag.
        if (x.type == m2::PhysicsBodyType::Dynamic && x.boneIndex < source.bones.size() &&
            y.boneIndex < exported.bones.size()) {
            if (source.bones[x.boneIndex].flags != exported.bones[y.boneIndex].flags) {
                tally.fail("dynamic bone's flags", path);
            }
            if ((source.bones[x.boneIndex].flags & static_cast<u32>(m2::BoneFlag::Kinematic)) == 0) {
                ++tally.known["dynamic bone the file leaves animated (no 0x400)"];
            }
        }
    }
    tally.known["body position rewritten as the pivot"] += pivotMoved;

    // --- shapes -----------------------------------------------------------------
    if (a.shapes.size() != b.shapes.size() || a.boxShapes.size() != b.boxShapes.size() ||
        a.sphereShapes.size() != b.sphereShapes.size() || a.capsuleShapes.size() != b.capsuleShapes.size() ||
        a.polytopeShapes.size() != b.polytopeShapes.size()) {
        tally.fail("shape counts", path);
        return;
    }
    for (std::size_t s = 0; s < a.shapes.size(); ++s) {
        const m2::PhysicsShape& x = a.shapes[s];
        const m2::PhysicsShape& y = b.shapes[s];
        f32 density = x.density;
        if (x.shapeType == m2::PhysicsShapeType::Capsule && static_cast<std::size_t>(x.shapeIndex) < a.capsuleShapes.size()) {
            density = m2::capsuleDensity(x.density, a.capsuleShapes[static_cast<std::size_t>(x.shapeIndex)], a.version);
        }
        if (x.shapeType != y.shapeType || x.shapeIndex != y.shapeIndex || x.gameFlags != y.gameFlags ||
            !SameBits(x.friction, y.friction) || !SameBits(x.restitution, y.restitution) ||
            !SameBits(density, y.density)) {
            tally.fail("shape fields", path);
        }
        if (y.unused14 != 0.0f || y.unused18 != 1.0f || y.unused1c != 0 || y.padding06 != 0 || y.padding1e != 0) {
            tally.fail("dead shape fields not at the upgrader's values", path);
        }
    }
    for (std::size_t s = 0; s < a.boxShapes.size(); ++s) {
        if (!SameBits(a.boxShapes[s].frame, b.boxShapes[s].frame) ||
            !SameBits(a.boxShapes[s].halfExtents, b.boxShapes[s].halfExtents)) {
            tally.fail("box", path);
        }
    }
    for (std::size_t s = 0; s < a.sphereShapes.size(); ++s) {
        if (!SameBits(a.sphereShapes[s].localPosition, b.sphereShapes[s].localPosition) ||
            !SameBits(a.sphereShapes[s].radius, b.sphereShapes[s].radius)) {
            tally.fail("sphere", path);
        }
    }
    for (std::size_t s = 0; s < a.capsuleShapes.size(); ++s) {
        const m2::CapsuleShape& x = a.capsuleShapes[s];
        const m2::CapsuleShape& y = b.capsuleShapes[s];
        ++drift.capsules;
        if (!SameBits(x.radius, y.radius)) {
            tally.fail("capsule radius", path);
        }
        const bool exact = SameBits(x.localPosition1, y.localPosition1) && SameBits(x.localPosition2, y.localPosition2);
        drift.exactCapsules += exact ? 1 : 0;
        const Vector3f d{x.localPosition2.x - x.localPosition1.x, x.localPosition2.y - x.localPosition1.y,
                         x.localPosition2.z - x.localPosition1.z};
        const f32 length = std::max(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), 1e-6f);
        for (const auto& [p, q] : {std::pair{x.localPosition1, y.localPosition1}, std::pair{x.localPosition2, y.localPosition2}}) {
            const f32 e = std::max({std::abs(p.x - q.x), std::abs(p.y - q.y), std::abs(p.z - q.z)});
            drift.worstEnd = std::max(drift.worstEnd, e / length);
        }
    }
    for (std::size_t s = 0; s < a.polytopeShapes.size(); ++s) {
        // Points already on a hull keep their values, so the set comes back.
        if (a.polytopeShapes[s].vertices != b.polytopeShapes[s].vertices) {
            tally.fail("polytope vertices", path);
        }
    }

    // --- joints -----------------------------------------------------------------
    if (a.joints.size() != b.joints.size()) {
        tally.fail("joint count", path);
        return;
    }
    for (std::size_t j = 0; j < a.joints.size(); ++j) {
        const m2::PhysicsJoint& x = a.joints[j];
        const m2::PhysicsJoint& y = b.joints[j];
        if (x.bodyAIndex != y.bodyAIndex || x.bodyBIndex != y.bodyBIndex || x.jointType != y.jointType ||
            x.jointId != y.jointId || y.padding08 != 0) {
            tally.fail("JOIN", path);
        }
    }
    const auto limit = [&](f32 lowA, f32 upA, f32 lowB, f32 upB, bool degrees, bool& ok) {
        const bool on = upA > lowA;
        ok = ok && on == (upB > lowB);
        if (on) {
            if (degrees) {
                CompareAngle(lowA, lowB, drift, ok);
                CompareAngle(upA, upB, drift, ok);
            } else {
                ok = ok && SameBits(lowA, lowB) && SameBits(upA, upB);
            }
        }
    };
    for (std::size_t k = 0; k < std::min(a.sphericalJoints.size(), b.sphericalJoints.size()); ++k) {
        const m2::SphericalJoint& x = a.sphericalJoints[k];
        const m2::SphericalJoint& y = b.sphericalJoints[k];
        if (!SameBits(x.anchorA, y.anchorA) || !SameBits(x.anchorB, y.anchorB) ||
            !SameBits(x.frictionTorque, y.frictionTorque)) {
            tally.fail("SPHJ", path);
        }
    }
    for (std::size_t k = 0; k < std::min(a.shoulderJoints.size(), b.shoulderJoints.size()); ++k) {
        const m2::ShoulderJoint& x = a.shoulderJoints[k];
        const m2::ShoulderJoint& y = b.shoulderJoints[k];
        bool ok = SameBits(x.frameA, y.frameA) && SameBits(x.frameB, y.frameB) &&
                  SameBits(x.maxMotorTorque, y.maxMotorTorque) && (x.motorMode & 0xFFu) == y.motorMode &&
                  SameBits(x.motorFrequencyHz, y.motorFrequencyHz) && SameBits(x.motorDampingRatio, y.motorDampingRatio);
        limit(x.lowerTwistAngle, x.upperTwistAngle, y.lowerTwistAngle, y.upperTwistAngle, true, ok);
        CompareAngle(x.coneAngle, y.coneAngle, drift, ok);
        if (!ok) {
            tally.fail("SHJ2", path);
        }
    }
    for (std::size_t k = 0; k < std::min(a.weldJoints.size(), b.weldJoints.size()); ++k) {
        const m2::WeldJoint& x = a.weldJoints[k];
        const m2::WeldJoint& y = b.weldJoints[k];
        if (!SameBits(x.frameA, y.frameA) || !SameBits(x.frameB, y.frameB) ||
            !SameBits(x.angularFrequencyHz, y.angularFrequencyHz) ||
            !SameBits(x.angularDampingRatio, y.angularDampingRatio) ||
            !SameBits(x.linearFrequencyHz, y.linearFrequencyHz) ||
            !SameBits(x.linearDampingRatio, y.linearDampingRatio) || y.unused70 != 0.0f) {
            tally.fail("WLJ3", path);
        }
    }
    for (std::size_t k = 0; k < std::min(a.revoluteJoints.size(), b.revoluteJoints.size()); ++k) {
        const m2::RevoluteJoint& x = a.revoluteJoints[k];
        const m2::RevoluteJoint& y = b.revoluteJoints[k];
        bool ok = SameBits(x.frameA, y.frameA) && SameBits(x.frameB, y.frameB) &&
                  SameBits(x.maxMotorTorque, y.maxMotorTorque) && (x.motorMode & 0xFFu) == y.motorMode &&
                  SameBits(x.motorFrequencyHz, y.motorFrequencyHz) && SameBits(x.motorDampingRatio, y.motorDampingRatio);
        limit(x.lowerAngle, x.upperAngle, y.lowerAngle, y.upperAngle, true, ok);
        if (!ok) {
            tally.fail("REV2", path);
        }
    }
    for (std::size_t k = 0; k < std::min(a.prismaticJoints.size(), b.prismaticJoints.size()); ++k) {
        const m2::PrismaticJoint& x = a.prismaticJoints[k];
        const m2::PrismaticJoint& y = b.prismaticJoints[k];
        bool ok = SameBits(x.frameA, y.frameA) && SameBits(x.frameB, y.frameB) &&
                  SameBits(x.referenceTranslation, y.referenceTranslation) &&
                  SameBits(x.maxMotorForce, y.maxMotorForce) && SameBits(x.motorSpeed, y.motorSpeed) &&
                  (x.motorMode & 0xFFu) == y.motorMode && SameBits(x.motorFrequencyHz, y.motorFrequencyHz) &&
                  SameBits(x.motorDampingRatio, y.motorDampingRatio);
        limit(x.lowerLimit, x.upperLimit, y.lowerLimit, y.upperLimit, false, ok);
        if (!ok) {
            tally.fail("PRS2", path);
        }
    }
    for (std::size_t k = 0; k < std::min(a.distanceJoints.size(), b.distanceJoints.size()); ++k) {
        const m2::DistanceJoint& x = a.distanceJoints[k];
        const m2::DistanceJoint& y = b.distanceJoints[k];
        if (!SameBits(x.localAnchorA, y.localAnchorA) || !SameBits(x.localAnchorB, y.localAnchorB) ||
            !SameBits(x.distance, y.distance)) {
            tally.fail("DSTJ", path);
        }
    }

    // The version-6 file reads back as written.
    const std::vector<u8> bytes = m2::writePhysics(b);
    const std::optional<m2::PhysicsData> reread = m2::parsePhysics(bytes);
    if (!reread.has_value() || m2::writePhysics(*reread) != bytes) {
        tally.fail("v6 write does not reread", path);
    }
}

} // namespace

TEST_CASE("wem physics G-W1 every WoW physics model round-trips", "[wem][physics][m2][corpus]") {
    const std::vector<fs::path> files = test::gather("WEM_M2_CORPUS_DIR", ".m2", {"WoW", "WowM2"});
    if (files.empty()) {
        SKIP("M2 corpus not found");
    }
    Tally tally;
    Drift drift;
    std::size_t models = 0, hulls = 0, exactTables = 0, otherTopology = 0;
    M2Converter converter;
    for (const fs::path& path : files) {
        if (!HasPhysics(path)) {
            continue;
        }
        utils::OsFileSystem vfs(test::pathText(path.parent_path()));
        m2::Parser parser;
        const m2::Model source = parser.parse(vfs, test::pathText(path));
        if (!source.physics.has_value()) {
            continue;
        }
        for (const m2::PolytopeShape& hull : source.physics->polytopeShapes) {
            ++hulls;
            CompareCook(hull, path, tally, exactTables, otherTopology);
        }
        if (source.bones.empty()) {
            tally.known["bodies on an external skeleton the parse did not load"] += 1;
            continue;
        }
        const Result<Document> imported = converter.fromM2(source, 274);
        if (!imported.ok()) {
            tally.fail("import failed", path);
            continue;
        }
        Writer writer;
        const std::vector<u8> bytes = writer.write(*imported);
        Parser wemParser;
        const std::optional<Document> document = wemParser.parse(std::span<const u8>(bytes));
        if (!document.has_value()) {
            tally.fail(".wem reread failed", path);
            continue;
        }
        const Result<m2::Model> exported = converter.toM2(*document, ProfileId::Wow, 274);
        if (!exported.ok()) {
            tally.fail("export failed", path);
            continue;
        }
        ++models;
        ComparePhysics(source, *exported, path, tally, drift);
    }
    std::cout << "G-W1: " << models << " models; capsules " << drift.exactCapsules << "/" << drift.capsules
              << " bit-exact, worst end " << drift.worstEnd << " of the length; angles " << drift.sameDegreeBits
              << "/" << drift.angles << " with the same degree bits\n";
    std::cout << "G-WC: " << hulls << " polytopes, " << exactTables << " with the shipped tables bit for bit, "
              << otherTopology << " with another topology\n";
    for (const auto& [what, count] : tally.counts) {
        std::cout << "  " << what << ": " << count << "\n";
    }
    for (const auto& [what, count] : tally.known) {
        std::cout << "  known, " << what << ": " << count << "\n";
    }
    for (const auto& s : tally.samples) {
        std::cout << "  e.g. " << s << "\n";
    }
    CHECK(models > 0);
    CHECK(drift.exactCapsules == drift.capsules);
    CHECK(otherTopology * 200 <= hulls);
    CHECK(tally.counts.empty());
}
