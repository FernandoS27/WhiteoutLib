// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// G-M (EDIT_MODE_PHYSICS_REDESIGN.md §2.1): StarCraft II's material presets --
// 22 rows, the shipped values where the 2013 file differs, and a body found
// by the preset whose values it carries -- and the two fits the editor builds
// bodies with: a hull within a point budget, and a box over a cloud.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include <whiteout/models/m3/physics_cook.h>
#include <whiteout/models/wem/geometry/fit.h>
#include <whiteout/models/wem/geometry/hull.h>
#include <whiteout/models/wem/physics/materials.h>

using namespace whiteout;
using namespace whiteout::models::wem;
using Catch::Approx;

namespace {

PhysicsBody BodyOf(const PhysicsMaterialPreset& preset, u32 shapes) {
    PhysicsBody body;
    for (u32 s = 0; s < shapes; ++s) {
        PhysicsShape shape;
        shape.material = preset.material;
        body.shapes.push_back(shape);
    }
    body.linearDamping = preset.linearDamping;
    body.angularDamping = preset.angularDamping;
    return body;
}

/// How far @p p lies outside the hull of @p hull; 0 or less is inside.
f32 Outside(const std::vector<Vector3f>& hull, const Vector3f& p) {
    std::vector<std::array<u32, 3>> triangles;
    REQUIRE(m3::HullTriangles(hull, triangles));
    f32 out = -1e30f;
    for (const std::array<u32, 3>& t : triangles) {
        const Vector3f n = cross(hull[t[1]] - hull[t[0]], hull[t[2]] - hull[t[0]]).normalized();
        out = std::max(out, n.dot(p - hull[t[0]]));
    }
    return out;
}

} // namespace

TEST_CASE("wem the StarCraft II presets are the shipped ones", "[wem][physics][materials]") {
    const auto presets = Sc2PhysicsMaterials();
    REQUIRE(presets.size() == 22u);
    for (u32 i = 0; i < presets.size(); ++i)
        CHECK(presets[i].id == i);
    CHECK(presets[kSc2Flesh].name == std::string("Flesh"));
    CHECK(presets[kSc2Flesh].material.density == 800.0f);
    CHECK(presets[kSc2Flesh].material.friction == Approx(0.7f));
    // D3: what shipped art carries where the 2013 file says otherwise.
    CHECK(presets[7].material.restitution == Approx(0.05f)); // Sand
    CHECK(presets[15].material.restitution == 0.0f);         // Snow
    CHECK(presets[17].angularDamping == Approx(0.3f));      // Paper
    CHECK(presets[20].linearDamping == Approx(0.4f));       // Hair
    CHECK(presets[20].angularDamping == Approx(0.3f));
}

TEST_CASE("wem a body is found by the preset it carries", "[wem][physics][materials]") {
    for (const PhysicsMaterialPreset& preset : Sc2PhysicsMaterials()) {
        CAPTURE(preset.name);
        CHECK(MatchSc2Preset(BodyOf(preset, 2)) == preset.id);
    }
    const PhysicsMaterialPreset& flesh = Sc2PhysicsMaterials()[kSc2Flesh];
    // Shapes that disagree, damping that differs, or no shapes: none.
    PhysicsBody mixed = BodyOf(flesh, 2);
    mixed.shapes[1].material.density = 900.0f;
    CHECK_FALSE(MatchSc2Preset(mixed).has_value());
    PhysicsBody damped = BodyOf(flesh, 1);
    damped.angularDamping = 0.3f;
    CHECK_FALSE(MatchSc2Preset(damped).has_value());
    CHECK_FALSE(MatchSc2Preset(PhysicsBody{}).has_value());
    // Within the tolerance is the preset.
    PhysicsBody near = BodyOf(flesh, 1);
    near.shapes[0].material.friction += 5e-5f;
    CHECK(MatchSc2Preset(near) == kSc2Flesh);
}

TEST_CASE("wem a hull within a budget keeps it and holds the points", "[wem][physics][fit]") {
    // A cube's corners, listed after points inside it and on its faces.
    std::vector<Vector3f> cloud;
    for (int i = 0; i < 40; ++i) {
        const f32 t = static_cast<f32>(i) / 40.0f;
        cloud.push_back(Vector3f{t * 1.6f - 0.8f, std::sin(t * 7.0f) * 0.8f, std::cos(t * 5.0f) * 0.8f});
    }
    for (int k = 0; k < 8; ++k)
        cloud.push_back(Vector3f{(k & 1) ? 1.0f : -1.0f, (k & 2) ? 1.0f : -1.0f, (k & 4) ? 1.0f : -1.0f});
    const std::vector<Vector3f> hull = geom::HullWithin(cloud, 8);
    CHECK(hull.size() == 8u);
    for (const Vector3f& p : cloud)
        CHECK(Outside(hull, p) <= 1e-4f);
    // A smaller budget is kept, and what is left out lies near the hull kept.
    const std::vector<Vector3f> six = geom::HullWithin(cloud, 6);
    CHECK(six.size() == 6u);
    // Four points or fewer come back as they are.
    const std::vector<Vector3f> few{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    CHECK(geom::HullWithin(few, 8).size() == 3u);
}

TEST_CASE("wem a box over a slab is the slab, its thin side the thin axis", "[wem][physics][fit]") {
    // The surface of a slab 20 wide (x), 4 thick (y) and 40 long (z), sampled
    // by area, and turned 30 degrees about the long axis.
    const f32 c = std::cos(0.5235988f), s = std::sin(0.5235988f);
    const auto turn = [&](Vector3f p) { return Vector3f{p.x * c - p.y * s, p.x * s + p.y * c, p.z + 5.0f}; };
    std::vector<Vector3f> cloud;
    for (int i = 0; i <= 20; ++i)
        for (int k = 0; k <= 40; ++k) {
            const f32 x = -10.0f + static_cast<f32>(i), z = -20.0f + static_cast<f32>(k);
            cloud.push_back(turn({x, 2.0f, z}));
            cloud.push_back(turn({x, -2.0f, z}));
        }
    for (int j = 0; j <= 4; ++j)
        for (int k = 0; k <= 40; ++k) {
            const f32 y = -2.0f + static_cast<f32>(j), z = -20.0f + static_cast<f32>(k);
            cloud.push_back(turn({10.0f, y, z}));
            cloud.push_back(turn({-10.0f, y, z}));
        }
    const geom::OrientedBox box = geom::FitOrientedBox(cloud, Vector3f{0, 0, 1}, 0.75f);
    CHECK(box.halfExtents.x == Approx(10.0f).epsilon(0.02));
    CHECK(box.halfExtents.y == Approx(2.0f).epsilon(0.02));
    CHECK(box.halfExtents.z == Approx(20.0f).epsilon(0.02));
    // The thin side is the minor axis, and the given axis is the last.
    CHECK(std::abs(box.axes[1].dot(Vector3f{-s, c, 0})) > 0.999f);
    CHECK(std::abs(box.axes[2].z) > 0.999f);
    CHECK((box.centre - Vector3f{0, 0, 5}).length() < 0.2f);
}
