// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The triangle BVH (EDIT_MODE_SKIN_DESIGN.md §3.6, gate S4's arm): every query
/// against brute force, and a refit equal to a rebuild.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/bvh.h>
#include <whiteout/models/wem/geometry/ids.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

struct Soup {
    std::vector<u32> indices;
    std::vector<Vector3f> positions;
};

/// @p triangles random triangles in a unit-ish box, each a small face rather
/// than a sliver, so brute force and the tree have something to disagree about.
Soup makeSoup(u32 triangles, u32 seed) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<f32> place(-10.0f, 10.0f);
    std::uniform_real_distribution<f32> size(0.2f, 1.5f);
    Soup soup;
    for (u32 t = 0; t < triangles; ++t) {
        const Vector3f at{place(random), place(random), place(random)};
        for (u32 k = 0; k < 3; ++k) {
            soup.positions.push_back(Vector3f{at.x + size(random), at.y + size(random),
                                              at.z + size(random)});
            soup.indices.push_back(t * 3 + k);
        }
    }
    return soup;
}

Vector3f sub(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.x - b.x, a.y - b.y, a.z - b.z};
}
f32 dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vector3f cross(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// The nearest hit, tried one triangle at a time.
f32 bruteRay(const Soup& soup, const Vector3f& origin, const Vector3f& direction, u32& triangle) {
    f32 best = std::numeric_limits<f32>::max();
    triangle = geom::kInvalidId;
    for (u32 t = 0; t * 3 + 2 < soup.indices.size(); ++t) {
        const Vector3f& a = soup.positions[soup.indices[t * 3]];
        const Vector3f& b = soup.positions[soup.indices[t * 3 + 1]];
        const Vector3f& c = soup.positions[soup.indices[t * 3 + 2]];
        const Vector3f ab = sub(b, a);
        const Vector3f ac = sub(c, a);
        const Vector3f p = cross(direction, ac);
        const f32 determinant = dot(ab, p);
        if (std::abs(determinant) < 1e-12f) {
            continue;
        }
        const f32 inverse = 1.0f / determinant;
        const Vector3f ao = sub(origin, a);
        const f32 u = dot(ao, p) * inverse;
        if (u < 0.0f || u > 1.0f) {
            continue;
        }
        const Vector3f q = cross(ao, ab);
        const f32 v = dot(direction, q) * inverse;
        if (v < 0.0f || u + v > 1.0f) {
            continue;
        }
        const f32 distance = dot(ac, q) * inverse;
        if (distance >= 0.0f && distance < best) {
            best = distance;
            triangle = t;
        }
    }
    return best;
}

f32 bruteClosest(const Soup& soup, const Vector3f& from, u32& triangle) {
    f32 best = std::numeric_limits<f32>::max();
    triangle = geom::kInvalidId;
    for (u32 t = 0; t * 3 + 2 < soup.indices.size(); ++t) {
        // The tree's own closest-point answer, over every triangle: this test
        // is about the tree's search, not about the point-in-triangle sums.
        geom::TriangleBvh one;
        const std::vector<u32> indices{0, 1, 2};
        const std::vector<Vector3f> corners{soup.positions[soup.indices[t * 3]],
                                            soup.positions[soup.indices[t * 3 + 1]],
                                            soup.positions[soup.indices[t * 3 + 2]]};
        one.build(indices, corners);
        const geom::PointHit hit = one.closestPoint(from);
        if (hit.hit() && hit.distance < best) {
            best = hit.distance;
            triangle = t;
        }
    }
    return best;
}

} // namespace

TEST_CASE("S4 the bvh finds what brute force finds", "[wem][geometry][bvh]") {
    const Soup soup = makeSoup(400, 1234);
    geom::TriangleBvh bvh;
    bvh.build(soup.indices, soup.positions);
    REQUIRE(bvh.triangleCount() == 400u);

    std::mt19937 random(99);
    std::uniform_real_distribution<f32> place(-14.0f, 14.0f);
    u32 hits = 0;
    for (u32 i = 0; i < 1000; ++i) {
        const Vector3f origin{place(random), place(random), place(random)};
        const Vector3f target{place(random), place(random), place(random)};
        const Vector3f direction = sub(target, origin);
        u32 wantTriangle = geom::kInvalidId;
        const f32 want = bruteRay(soup, origin, direction, wantTriangle);
        const geom::RayHit got = bvh.raycast(origin, direction);
        CAPTURE(i);
        if (wantTriangle == geom::kInvalidId) {
            CHECK_FALSE(got.hit());
            continue;
        }
        ++hits;
        REQUIRE(got.hit());
        CHECK(std::abs(got.distance - want) < 1e-4f);
    }
    CHECK(hits > 0u);
}

TEST_CASE("S4 the bvh finds the closest point", "[wem][geometry][bvh]") {
    const Soup soup = makeSoup(120, 77);
    geom::TriangleBvh bvh;
    bvh.build(soup.indices, soup.positions);

    std::mt19937 random(5);
    std::uniform_real_distribution<f32> place(-14.0f, 14.0f);
    for (u32 i = 0; i < 200; ++i) {
        const Vector3f from{place(random), place(random), place(random)};
        u32 wantTriangle = geom::kInvalidId;
        const f32 want = bruteClosest(soup, from, wantTriangle);
        const geom::PointHit got = bvh.closestPoint(from);
        CAPTURE(i);
        REQUIRE(got.hit());
        CHECK(std::abs(got.distance - want) < 1e-3f);
    }
}

TEST_CASE("S4 a refit equals a rebuild", "[wem][geometry][bvh]") {
    Soup soup = makeSoup(300, 4242);
    geom::TriangleBvh refitted;
    refitted.build(soup.indices, soup.positions);

    // The pose moves every vertex; the topology does not change, which is
    // exactly when a refit is right.
    std::mt19937 random(8);
    std::uniform_real_distribution<f32> shift(-2.0f, 2.0f);
    for (Vector3f& p : soup.positions) {
        p.x += shift(random);
        p.y += shift(random);
        p.z += shift(random);
    }
    refitted.refit(soup.positions);

    geom::TriangleBvh rebuilt;
    rebuilt.build(soup.indices, soup.positions);

    std::mt19937 rays(11);
    std::uniform_real_distribution<f32> place(-16.0f, 16.0f);
    u32 hits = 0;
    for (u32 i = 0; i < 500; ++i) {
        const Vector3f origin{place(rays), place(rays), place(rays)};
        const Vector3f direction =
            sub(Vector3f{place(rays), place(rays), place(rays)}, origin);
        const geom::RayHit a = refitted.raycast(origin, direction);
        const geom::RayHit b = rebuilt.raycast(origin, direction);
        CAPTURE(i);
        CHECK(a.hit() == b.hit());
        if (a.hit()) {
            ++hits;
            CHECK(std::abs(a.distance - b.distance) < 1e-4f);
        }
        const Vector3f from{place(rays), place(rays), place(rays)};
        const geom::PointHit pa = refitted.closestPoint(from);
        const geom::PointHit pb = rebuilt.closestPoint(from);
        REQUIRE(pa.hit());
        REQUIRE(pb.hit());
        CHECK(std::abs(pa.distance - pb.distance) < 1e-4f);
    }
    CHECK(hits > 0u);
}

TEST_CASE("S4 a grazing ray is a miss, not a NaN", "[wem][geometry][bvh]") {
    // One triangle in the z = 0 plane, and a ray that runs along it.
    const std::vector<Vector3f> positions{Vector3f{0, 0, 0}, Vector3f{4, 0, 0}, Vector3f{0, 4, 0}};
    const std::vector<u32> indices{0, 1, 2};
    geom::TriangleBvh bvh;
    bvh.build(indices, positions);

    const geom::RayHit grazing = bvh.raycast(Vector3f{-1, 1, 0}, Vector3f{1, 0, 0});
    CHECK_FALSE(grazing.hit());
    CHECK(std::isfinite(grazing.distance));

    // And a ray straight at it hits, with the barycentrics inside the triangle.
    const geom::RayHit through = bvh.raycast(Vector3f{1, 1, 5}, Vector3f{0, 0, -1});
    REQUIRE(through.hit());
    CHECK(std::abs(through.distance - 5.0f) < 1e-4f);
    CHECK(through.u >= 0.0f);
    CHECK(through.v >= 0.0f);
    CHECK(through.u + through.v <= 1.0f);

    // An empty tree answers nothing rather than reading past itself.
    geom::TriangleBvh empty;
    CHECK_FALSE(empty.raycast(Vector3f{0, 0, 0}, Vector3f{0, 0, 1}).hit());
    CHECK_FALSE(empty.closestPoint(Vector3f{0, 0, 0}).hit());
}

TEST_CASE("S4 the bvh builds and refits a mesh-sized soup in time", "[wem][geometry][bvh]") {
    // §3.6's estimate: about 30,000 triangles, a millisecond or two to build and
    // a fraction of that to refit. Timed rather than asserted tightly -- this is
    // a report, and the numbers go in the design.
    const Soup soup = makeSoup(30000, 2026);
    geom::TriangleBvh bvh;
    const auto beforeBuild = std::chrono::steady_clock::now();
    bvh.build(soup.indices, soup.positions);
    const auto afterBuild = std::chrono::steady_clock::now();
    bvh.refit(soup.positions);
    const auto afterRefit = std::chrono::steady_clock::now();
    const f64 buildMs =
        std::chrono::duration<f64, std::milli>(afterBuild - beforeBuild).count();
    const f64 refitMs = std::chrono::duration<f64, std::milli>(afterRefit - afterBuild).count();
    std::cout << "S4 bvh: 30,000 triangles built in " << buildMs << " ms, refitted in " << refitMs
              << " ms\n";
    CHECK(bvh.triangleCount() == 30000u);
    CHECK(refitMs < buildMs * 2.0);
}
