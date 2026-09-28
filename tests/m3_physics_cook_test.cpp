// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// The SC2 physics cooker and upgrade (WEM_PHYSICS_DESIGN.md §4, §5): unit cases
// for the hull and mesh tables and each converter, and the corpus gates G-C
// (re-cooking every shipped hull and mesh gives back what shipped) and G-U
// (every old physics chunk comes out at the current version with the client's
// values).

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <whiteout/models/m3/parser.h>
#include <whiteout/models/m3/physics_cook.h>
#include <whiteout/models/m3/physics_upgrade.h>
#include <whiteout/models/m3/writer.h>

#include "m3_client_chunk_table.h"
#include "test_helpers.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::m3;
using Catch::Approx;

namespace {

/// Every half-edge's twin, next and face agree, loops close, and the tables
/// satisfy Euler and contain every vertex.
void CheckPolytope(const PhysicsShape& s) {
    REQUIRE(s.hullVertexCount == s.hullVertices.size());
    REQUIRE(s.hullFaceCount == s.hullPlanes.size());
    REQUIRE(s.hullHalfEdgeCount == s.hullHalfEdges.size());
    REQUIRE(s.hullFaceFirstEdges.size() == s.hullPlanes.size());
    REQUIRE(s.hullHalfEdges.size() % 2 == 0);
    CHECK(s.hullVertexCount + s.hullFaceCount == s.hullHalfEdgeCount / 2 + 2);
    for (std::size_t e = 0; e < s.hullHalfEdges.size(); ++e) {
        const auto& h = s.hullHalfEdges[e];
        const std::size_t twin = e + static_cast<std::ptrdiff_t>(h.twinOffset);
        REQUIRE(twin < s.hullHalfEdges.size());
        CHECK(static_cast<std::size_t>(twin + s.hullHalfEdges[twin].twinOffset) == e);
        // The twin leaves where this one arrives.
        CHECK(s.hullHalfEdges[twin].originVertex == s.hullHalfEdges[h.nextInFace].originVertex);
        CHECK(s.hullHalfEdges[h.nextInFace].face == h.face);
    }
    for (std::size_t f = 0; f < s.hullFaceFirstEdges.size(); ++f) {
        std::size_t e = s.hullFaceFirstEdges[f];
        std::size_t steps = 0;
        do {
            CHECK(s.hullHalfEdges[e].face == f);
            e = s.hullHalfEdges[e].nextInFace;
        } while (e != s.hullFaceFirstEdges[f] && ++steps < 300);
        CHECK(steps < 300);
    }
    for (const Vector4f& plane : s.hullPlanes) {
        for (const Vector3f& v : s.hullVertices) {
            CHECK(plane.x * v.x + plane.y * v.y + plane.z * v.z <= plane.w + 1e-5f);
        }
    }
}

std::vector<Vector3f> BoxCorners(f32 hx, f32 hy, f32 hz) {
    std::vector<Vector3f> out;
    for (int i = 0; i < 8; ++i) {
        out.push_back({(i & 1) ? hx : -hx, (i & 2) ? hy : -hy, (i & 4) ? hz : -hz});
    }
    return out;
}

} // namespace

TEST_CASE("m3 cook a box hull is eight vertices and six quads", "[m3][physics][cook]") {
    PhysicsShape shape;
    shape.shapeType = PhysicsShapeType::ConvexHull;
    const auto corners = BoxCorners(1.0f, 2.0f, 3.0f);
    const HullCookReport report = CookHull(shape, corners);
    REQUIRE(report.ok);
    CHECK_FALSE(report.simplified);
    CheckPolytope(shape);
    CHECK(shape.hullVertexCount == 8);
    CHECK(shape.hullFaceCount == 6);
    CHECK(shape.hullHalfEdgeCount == 24);
    CHECK(shape.hullVolume == Approx(48.0f));
    CHECK(shape.hullSurfaceArea == Approx(2.0f * (2 * 4 + 2 * 6 + 4 * 6)));
    CHECK(shape.hullCentroid.x == Approx(0.0f).margin(1e-6));
    // The input points come back bit-exact, in input order.
    for (std::size_t i = 0; i < corners.size(); ++i) {
        CHECK(shape.hullVertices[i].x == corners[i].x);
        CHECK(shape.hullVertices[i].y == corners[i].y);
        CHECK(shape.hullVertices[i].z == corners[i].z);
    }
}

TEST_CASE("m3 cook drops interior and edge points from a hull", "[m3][physics][cook]") {
    PhysicsShape shape;
    auto points = BoxCorners(1.0f, 1.0f, 1.0f);
    points.push_back({0.0f, 0.0f, 0.0f});  // inside
    points.push_back({0.0f, 0.0f, 1.0f});  // inside a face
    points.push_back({1.0f, 0.0f, 1.0f});  // on an edge
    points.push_back({1.0f, 1.0f, 1.0f});  // a duplicate
    REQUIRE(CookHull(shape, points).ok);
    CheckPolytope(shape);
    CHECK(shape.hullVertexCount == 8);
    CHECK(shape.hullFaceCount == 6);
}

TEST_CASE("m3 cook a flat or tiny point set gives no hull", "[m3][physics][cook]") {
    PhysicsShape shape;
    std::vector<Vector3f> flat{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    CHECK_FALSE(CookHull(shape, flat).ok);
    CHECK(shape.hullVertices.empty());
    std::vector<Vector3f> three{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    CHECK_FALSE(CookHull(shape, three).ok);
}

TEST_CASE("m3 cook a round point cloud fits the u8 limits", "[m3][physics][cook]") {
    std::vector<Vector3f> sphere;
    for (int i = 0; i < 2000; ++i) {
        const f32 z = 1.0f - 2.0f * (static_cast<f32>(i) + 0.5f) / 2000.0f;
        const f32 r = std::sqrt(1.0f - z * z);
        const f32 phi = static_cast<f32>(i) * 2.39996323f;
        sphere.push_back({r * std::cos(phi), r * std::sin(phi), z});
    }
    PhysicsShape shape;
    const HullCookReport report = CookHull(shape, sphere);
    REQUIRE(report.ok);
    CHECK(report.simplified);
    CheckPolytope(shape);
    CHECK(shape.hullVertexCount <= kMaxHullVertices);
    CHECK(shape.hullHalfEdgeCount <= kMaxHullHalfEdges);
    CHECK(shape.hullVolume == Approx(4.18879f).epsilon(0.2));
}

TEST_CASE("m3 cook the client's load merges faces within 20 degrees", "[m3][physics][cook]") {
    // A box with a slightly bevelled top: the two top faces are 5 degrees apart.
    std::vector<Vector3f> points = BoxCorners(1.0f, 1.0f, 1.0f);
    points.push_back({0.0f, -1.0f, 1.0f + std::tan(0.0872665f)});
    points.push_back({0.0f, 1.0f, 1.0f + std::tan(0.0872665f)});
    PhysicsShape exact, client;
    REQUIRE(CookHull(exact, points, HullCook::Exact).ok);
    REQUIRE(CookHull(client, points, HullCook::ClientLoad).ok);
    CheckPolytope(client);
    CHECK(exact.hullFaceCount == 7);
    CHECK(client.hullFaceCount == 6);
}

TEST_CASE("m3 cook a mesh tree is the client's for a small mesh", "[m3][physics][cook]") {
    const std::vector<Vector3f> vertices{{0, 0, 0}, {2, 0, 0}, {0, 4, 0}, {0, 0, 6}};
    const std::vector<u32> triangles{0, 1, 2, 0, 2, 3, 0, 3, 1, 1, 3, 2};
    const MeshTree tree = ComputeMeshTree(vertices, triangles);
    CHECK(tree.height == 1);
    CHECK(tree.center.x == 1.0f);
    CHECK(tree.center.y == 2.0f);
    CHECK(tree.center.z == 3.0f);
    CHECK(tree.extent.x == ((0.1f - 0.0f) + 2.0f) * 0.5f);
    CHECK(tree.vertexCount == 4);

    PhysicsShape shape;
    CookMesh(shape, vertices, triangles);
    CHECK(shape.meshFaceIndices32.size() == 4);
    CHECK(shape.meshFaceIndices16.empty());
    CHECK(shape.meshBvhNodes.empty());
    CHECK(shape.meshTreeDepth == 1);
    // A closed tetrahedron: every edge has a neighbour.
    for (const auto& f : shape.meshFaceIndices32) {
        CHECK(f[3] != 0xFFFFFFFFu);
        CHECK(f[6] == 0u);
    }
}

TEST_CASE("m3 cook a degenerate triangle is left out of the tree", "[m3][physics][cook]") {
    const std::vector<Vector3f> vertices{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 0, 0}};
    const std::vector<u32> triangles{0, 1, 2, 0, 1, 3};
    const MeshTree tree = ComputeMeshTree(vertices, triangles);
    CHECK(tree.vertexCount == 3);
    CHECK(tree.center.x == 0.5f);
}

TEST_CASE("m3 upgrade a Havok body takes Domino's defaults", "[m3][physics][upgrade]") {
    Model model;
    RigidBody body;
    body.setVersion(2);
    body.density = 5.0f;
    body.friction = 22.0f;
    body.parentBoneIndex = 3;
    PhysicsShape hull;
    hull.setVersion(1);
    hull.shapeType = PhysicsShapeType::ConvexHull;
    hull.sourcePoints = BoxCorners(1.0f, 1.0f, 1.0f);
    hull.transform = Matrix44f::identity();
    hull.transform.data[3][0] = 5.0f; // the matrix is baked into the points
    body.rigidBodyShape.push_back(hull);
    model.rigidBodies.push_back(body);

    const auto issues = UpgradePhysics(model);
    CHECK(issues.empty());
    const RigidBody& b = model.rigidBodies[0];
    CHECK(b.getVersion() == 4);
    CHECK(b.density == 1000.0f);
    CHECK(b.friction == 0.3f);
    CHECK(b.restitution == 0.0f);
    CHECK(b.inertiaScale == 1.0f);
    CHECK(b.physicsType == 24);
    CHECK(b.simulationType == 0);
    CHECK(b.dynamicState.animId == 0xFFFFFFFFu);
    CHECK(b.dynamicState.initValue == 0u);
    CHECK(b.dynamicState.flags == 0u);
    CHECK(b.parentBoneIndex == 3);
    const PhysicsShape& s = b.rigidBodyShape[0];
    CHECK(s.getVersion() == 3);
    CHECK(s.sourcePoints.empty());
    CHECK(s.hullVertexCount == 8);
    CHECK(s.hullCentroid.x == Approx(5.0f));
    CHECK(s.transform.data[3][0] == 0.0f);
    CheckPolytope(s);
}

TEST_CASE("m3 upgrade a v3 body gets an unsampled switch", "[m3][physics][upgrade]") {
    Model model;
    RigidBody body;
    body.setVersion(3);
    body.density = 700.0f;
    model.rigidBodies.push_back(body);
    UpgradePhysics(model);
    CHECK(model.rigidBodies[0].density == 700.0f);
    CHECK(model.rigidBodies[0].dynamicState.animId == 0xFFFFFFFFu);
    CHECK(model.rigidBodies[0].dynamicBlendOut == 0.0f);
}

TEST_CASE("m3 upgrade old cloth takes the client's defaults", "[m3][physics][upgrade]") {
    Model model;
    ClothPhysics cloth;
    cloth.setVersion(2);
    cloth.density = 3.0f;
    model.clothPhysics.push_back(cloth);
    UpgradePhysics(model);
    const ClothPhysics& c = model.clothPhysics[0];
    CHECK(c.getVersion() == 4);
    CHECK(c.density == 3.0f);
    CHECK(c.shearStiffness == 0.1f);
    CHECK(c.dragFactor == 1.0f);
    CHECK(c.liftFactor == 0.5f);
    CHECK(c.sphereStiffness == 0.1f);
    CHECK(c.skinStiffness == 0.1f);
    CHECK(c.localChannels == 1u);
    CHECK(c.active.animId == 0xFFFFFFFFu);
    CHECK(c.active.initValue == 1u);
}

TEST_CASE("m3 upgrade old force fields act on particles and bodies", "[m3][physics][upgrade]") {
    Model model;
    Force v1;
    v1.setVersion(1);
    v1.flags = ForceFlag::Falloff;
    Force v0;
    v0.setVersion(0);
    v0.forceShape = static_cast<ForceShape>(2);
    Force v0b;
    v0b.setVersion(0);
    v0b.forceType = ForceType::Radial;
    v0b.forceShape = static_cast<ForceShape>(4);
    model.forces = {v1, v0, v0b};
    Warp refused;
    refused.setVersion(0);
    Warp kept;
    kept.setVersion(1);
    model.warps = {refused, kept};
    const auto issues = UpgradePhysics(model);
    CHECK(model.forces[0].flags == (ForceFlag::Falloff | ForceFlag::AffectsParticles | ForceFlag::AffectsBodies));
    CHECK(model.forces[1].forceType == ForceType::Vortex);
    CHECK(model.forces[1].forceShape == ForceShape::Cylinder);
    CHECK(model.forces[2].forceShape == ForceShape::Hemisphere);
    CHECK(model.forces[2].getVersion() == 2);
    CHECK(model.warps.size() == 1);
    CHECK(issues.size() == 1);
}

TEST_CASE("m3 upgrade a written model reads back current and unchanged", "[m3][physics][upgrade]") {
    Model model;
    model.forceVersion(29);
    Bone bone;
    model.bones.push_back(bone);
    RigidBody body;
    body.setVersion(2);
    PhysicsShape box;
    box.setVersion(1);
    box.shapeType = PhysicsShapeType::Box;
    box.shapeDimensions = {1.0f, 2.0f, 3.0f};
    body.rigidBodyShape.push_back(box);
    PhysicsShape hull;
    hull.shapeType = PhysicsShapeType::ConvexHull;
    CookHull(hull, BoxCorners(1.0f, 1.0f, 1.0f));
    body.rigidBodyShape.push_back(hull);
    model.rigidBodies.push_back(body);
    UpgradePhysics(model);

    Writer writer;
    const std::vector<u8> bytes = writer.write(model);
    Parser parser;
    const Model back = parser.parse(std::span<const u8>(bytes));
    REQUIRE(back.rigidBodies.size() == 1);
    const RigidBody& b = back.rigidBodies[0];
    CHECK(b.getVersion() == 4);
    CHECK(b.density == 1000.0f);
    REQUIRE(b.rigidBodyShape.size() == 2);
    CHECK(b.rigidBodyShape[0].getVersion() == 3);
    CHECK(b.rigidBodyShape[0].shapeDimensions.y == 2.0f);
    CHECK(b.rigidBodyShape[1].hullVertices == model.rigidBodies[0].rigidBodyShape[1].hullVertices);
    CHECK(b.rigidBodyShape[1].hullVolume == model.rigidBodies[0].rigidBodyShape[1].hullVolume);
}

// ============================================================================
// Corpus gates
// ============================================================================

namespace {

/// The physics-bearing corpus models: `M3_PHYSICS_LIST` (one path per line,
/// anything after a tab ignored), else every `.m3` under the corpus.
std::vector<fs::path> PhysicsCorpus() {
    std::vector<fs::path> files;
    if (const char* list = std::getenv("M3_PHYSICS_LIST"); list && fs::exists(list)) {
        std::ifstream in(list);
        for (std::string line; std::getline(in, line);) {
            const auto tab = line.find('\t');
            if (tab != std::string::npos) {
                line.resize(tab);
            }
            if (!line.empty()) {
                files.emplace_back(line);
            }
        }
        return files;
    }
    const std::string base = test::findCorpusBase("Corpus");
    if (base.empty()) {
        return files;
    }
    for (auto sub : {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"}) {
        const fs::path dir = fs::path(base) / sub;
        if (!fs::is_directory(dir)) {
            continue;
        }
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".m3") {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool Near(f32 a, f32 b, f32 rel, f32 abs) {
    return std::abs(a - b) <= std::max(abs, rel * std::max(std::abs(a), std::abs(b)));
}

struct Tally {
    std::map<std::string, std::size_t> counts;
    std::vector<std::string> samples;
    void fail(const std::string& what, const fs::path& file) {
        if (counts[what]++ < 5) {
            samples.push_back(what + ": " + file.filename().string());
        }
    }
};

} // namespace

TEST_CASE("m3 cook G-C every shipped hull and mesh re-cooks to itself", "[m3][physics][cook][corpus]") {
    const auto files = PhysicsCorpus();
    if (files.empty()) {
        SKIP("M3 corpus not found");
    }
    std::size_t hulls = 0, nearDegenerate = 0, meshes = 0, clientMeshes = 0, legacyMeshes = 0, legacyHeightDiffers = 0;
    Tally tally;
    for (const auto& path : files) {
        Parser parser;
        const Model model = parser.parse(path.string());
        for (const RigidBody& body : model.rigidBodies) {
            for (const PhysicsShape& shipped : body.rigidBodyShape) {
                if (shipped.shapeType == PhysicsShapeType::ConvexHull && !shipped.hullVertices.empty()) {
                    ++hulls;
                    const std::size_t n = std::min<std::size_t>(shipped.hullVertexCount, shipped.hullVertices.size());
                    const std::vector<Vector3f> points(shipped.hullVertices.begin(),
                                                       shipped.hullVertices.begin() + static_cast<std::ptrdiff_t>(n));
                    PhysicsShape cooked;
                    if (!CookHull(cooked, points).ok) {
                        tally.fail("hull did not cook", path);
                        continue;
                    }
                    // A near-degenerate shipped hull (two faces a hair apart, two
                    // vertices 5e-7 apart) can come back a face or a vertex
                    // different; the solid has to stay the same.
                    const bool sameTopology =
                        cooked.hullVertices == points && cooked.hullFaceCount == shipped.hullFaceCount;
                    if (!sameTopology) {
                        ++nearDegenerate;
                    }
                    for (std::size_t f = 0; sameTopology && f < std::min<std::size_t>(shipped.hullFaceCount, shipped.hullPlanes.size()); ++f) {
                        const Vector4f& p = shipped.hullPlanes[f];
                        const bool matched = std::any_of(cooked.hullPlanes.begin(), cooked.hullPlanes.end(), [&](const Vector4f& q) {
                            return Near(p.x, q.x, 0, 1e-4f) && Near(p.y, q.y, 0, 1e-4f) && Near(p.z, q.z, 0, 1e-4f) &&
                                   Near(p.w, q.w, 1e-4f, 1e-4f);
                        });
                        if (!matched) {
                            tally.fail("hull plane unmatched", path);
                            break;
                        }
                    }
                    if (!Near(cooked.hullVolume, shipped.hullVolume, 1e-4f, 1e-6f)) {
                        tally.fail("hull volume", path);
                    }
                    if (!Near(cooked.hullSurfaceArea, shipped.hullSurfaceArea, 1e-4f, 1e-6f)) {
                        tally.fail("hull area", path);
                    }
                    const f32 size = std::sqrt(shipped.hullSurfaceArea);
                    if (!Near(cooked.hullCentroid.x, shipped.hullCentroid.x, 0, 1e-4f * size) ||
                        !Near(cooked.hullCentroid.y, shipped.hullCentroid.y, 0, 1e-4f * size) ||
                        !Near(cooked.hullCentroid.z, shipped.hullCentroid.z, 0, 1e-4f * size)) {
                        tally.fail("hull centroid", path);
                    }
                }
                if (shipped.shapeType == PhysicsShapeType::Mesh && !shipped.meshVertexPositions.empty()) {
                    ++meshes;
                    // Stored about the centre; the shape-frame vertex adds it back.
                    const Vector3f c = shipped.meshBoundsCenter;
                    std::vector<Vector3f> vertices;
                    for (const Vector4f& v : shipped.meshVertexPositions) {
                        vertices.push_back({v.x + c.x, v.y + c.y, v.z + c.z});
                    }
                    std::vector<u32> triangles;
                    if (!shipped.meshFaceIndices16.empty()) {
                        const std::size_t count = std::min<std::size_t>(shipped.meshFaceIndex16Count, shipped.meshFaceIndices16.size());
                        for (std::size_t t = 0; t < count; ++t) {
                            for (int k = 0; k < 3; ++k) {
                                triangles.push_back(shipped.meshFaceIndices16[t][static_cast<std::size_t>(k)]);
                            }
                        }
                    } else {
                        const std::size_t count = std::min<std::size_t>(shipped.meshFaceIndex32Count, shipped.meshFaceIndices32.size());
                        for (std::size_t t = 0; t < count; ++t) {
                            for (int k = 0; k < 3; ++k) {
                                triangles.push_back(shipped.meshFaceIndices32[t][static_cast<std::size_t>(k)]);
                            }
                        }
                    }
                    const MeshTree tree = ComputeMeshTree(vertices, triangles);
                    const auto near = [](const Vector3f& a, const Vector3f& b, f32 tol) {
                        return Near(a.x, b.x, 1e-6f, tol) && Near(a.y, b.y, 1e-6f, tol) && Near(a.z, b.z, 1e-6f, tol);
                    };
                    // The rounding of `stored + centre` moves the bounds by an ulp.
                    const f32 ulp = 4e-6f * std::max({std::abs(c.x), std::abs(c.y), std::abs(c.z), 1.0f});
                    if (!near(tree.center, shipped.meshBoundsCenter, ulp)) {
                        tally.fail("mesh centre", path);
                    }
                    if (!near(tree.extent, shipped.meshBoundsExtent, ulp)) {
                        tally.fail("mesh extent", path);
                    }
                    // Two exporters shipped: the client's builder (extent / 32767)
                    // and an older one (extent / 32766) whose trees are not the
                    // client's. Only the first is this builder's to reproduce.
                    const Vector3f e = shipped.meshBoundsExtent;
                    const f32 step = std::bit_cast<f32>(0x38000100u);
                    const bool clientBuilt = shipped.meshTolerance.x == e.x * step &&
                                             shipped.meshTolerance.y == e.y * step &&
                                             shipped.meshTolerance.z == e.z * step;
                    ++(clientBuilt ? clientMeshes : legacyMeshes);
                    if (clientBuilt && tree.height != shipped.meshTreeDepth) {
                        tally.fail("mesh height (client-built)", path);
                    }
                    if (!clientBuilt && tree.height != shipped.meshTreeDepth) {
                        ++legacyHeightDiffers;
                    }
                }
            }
        }
    }
    std::cout << "G-C: " << hulls << " hulls (" << nearDegenerate << " with another topology), " << meshes << " meshes (" << clientMeshes << " client-built, "
              << legacyMeshes << " older, " << legacyHeightDiffers << " of those with another height) over "
              << files.size() << " files\n";
    for (const auto& [what, count] : tally.counts) {
        std::cout << "  " << what << ": " << count << "\n";
    }
    for (const auto& s : tally.samples) {
        std::cout << "  e.g. " << s << "\n";
    }
    CHECK(hulls > 0);
    CHECK(nearDegenerate * 200 <= hulls);
    CHECK(tally.counts.empty());
}

TEST_CASE("m3 upgrade G-U every physics chunk reads at its current version", "[m3][physics][upgrade][corpus]") {
    const auto files = PhysicsCorpus();
    if (files.empty()) {
        SKIP("M3 corpus not found");
    }
    Tally tally;
    std::size_t bodies = 0, cloths = 0, forces = 0, roundTrips = 0;
    std::set<std::string> unknownTags;
    for (const auto& path : files) {
        Parser parser;
        const Model model = parser.parse(path.string());
        for (const RigidBody& body : model.rigidBodies) {
            ++bodies;
            if (body.getVersion() != kCurrentRigidBodyVersion) {
                tally.fail("PHRB not current", path);
            }
            for (const PhysicsShape& shape : body.rigidBodyShape) {
                if (shape.getVersion() != kCurrentPhysicsShapeVersion) {
                    tally.fail("PHSH not current", path);
                }
                if (!shape.sourcePoints.empty()) {
                    tally.fail("PHSH source points left uncooked", path);
                }
                if (shape.shapeType == PhysicsShapeType::ConvexHull && shape.hullVertices.empty()) {
                    tally.fail("hull empty", path);
                }
                if (shape.shapeType == PhysicsShapeType::ConvexHull && !shape.hullVertices.empty() &&
                    (shape.hullVertexCount > kMaxHullVertices || shape.hullHalfEdgeCount > kMaxHullHalfEdges)) {
                    tally.fail("hull over the limits", path);
                }
            }
        }
        for (const ClothPhysics& cloth : model.clothPhysics) {
            ++cloths;
            if (cloth.getVersion() != kCurrentClothVersion) {
                tally.fail("PHCL not current", path);
            }
        }
        for (const Force& force : model.forces) {
            ++forces;
            if (force.getVersion() != kCurrentForceVersion) {
                tally.fail("FOR_ not current", path);
            }
        }
        if (model.rigidBodies.empty() && model.clothPhysics.empty()) {
            continue;
        }
        // Written back, it reads the same, and G-V: the client's own chunk
        // table has nothing to say against it.
        Writer writer;
        const std::vector<u8> bytes = writer.write(model);
        const m3client::Report gv = m3client::Check(bytes, model.getVersion() <= 29);
        for (const std::string& problem : gv.problems) {
            tally.fail("G-V " + problem, path);
        }
        unknownTags.insert(gv.unknown.begin(), gv.unknown.end());
        Parser reparser;
        const Model back = reparser.parse(std::span<const u8>(bytes));
        ++roundTrips;
        if (back.rigidBodies.size() != model.rigidBodies.size() || back.clothPhysics.size() != model.clothPhysics.size()) {
            tally.fail("round trip counts", path);
            continue;
        }
        for (std::size_t b = 0; b < model.rigidBodies.size(); ++b) {
            const RigidBody& x = model.rigidBodies[b];
            const RigidBody& y = back.rigidBodies[b];
            if (x.density != y.density || x.friction != y.friction || x.inertiaScale != y.inertiaScale ||
                x.dynamicState.animId != y.dynamicState.animId || x.flags != y.flags ||
                x.rigidBodyShape.size() != y.rigidBodyShape.size()) {
                tally.fail("round trip body", path);
                continue;
            }
            for (std::size_t s = 0; s < x.rigidBodyShape.size(); ++s) {
                if (x.rigidBodyShape[s].hullVertices != y.rigidBodyShape[s].hullVertices ||
                    x.rigidBodyShape[s].meshTreeDepth != y.rigidBodyShape[s].meshTreeDepth) {
                    tally.fail("round trip shape", path);
                }
            }
        }
        for (std::size_t c = 0; c < model.clothPhysics.size(); ++c) {
            if (model.clothPhysics[c].skinStiffness != back.clothPhysics[c].skinStiffness ||
                model.clothPhysics[c].active.initValue != back.clothPhysics[c].active.initValue) {
                tally.fail("round trip cloth", path);
            }
        }
    }
    std::cout << "G-U: " << bodies << " bodies, " << cloths << " cloths, " << forces << " forces, "
              << roundTrips << " round trips over " << files.size() << " files\n";
    for (const std::string& tag : unknownTags) {
        std::cout << "  G-V: no client row for " << tag << "\n";
    }
    for (const auto& [what, count] : tally.counts) {
        std::cout << "  " << what << ": " << count << "\n";
    }
    for (const auto& s : tally.samples) {
        std::cout << "  e.g. " << s << "\n";
    }
    CHECK(bodies > 0);
    CHECK(tally.counts.empty());
}

TEST_CASE("m3 cook probe", "[.probe]") {
    const char* env = std::getenv("M3_PROBE");
    if (env == nullptr) {
        SKIP("M3_PROBE unset");
    }
    Parser parser;
    const Model model = parser.parse(env);
    for (const RigidBody& b : model.rigidBodies) {
        for (const PhysicsShape& s : b.rigidBodyShape) {
            if (s.shapeType == PhysicsShapeType::ConvexHull) {
                PhysicsShape cooked;
                CookHull(cooked, s.hullVertices);
                if (cooked.hullFaceCount == s.hullFaceCount && cooked.hullVertexCount == s.hullVertexCount) {
                    continue;
                }
                std::printf("hull v=%u f=%u e=%u, cooked v=%u f=%u e=%u\n", s.hullVertexCount, s.hullFaceCount,
                            s.hullHalfEdgeCount, cooked.hullVertexCount, cooked.hullFaceCount, cooked.hullHalfEdgeCount);
                for (const auto& p : s.hullPlanes) {
                    std::printf("  shipped plane (%.6f %.6f %.6f %.6f)\n", p.x, p.y, p.z, p.w);
                }
                for (const auto& p : cooked.hullPlanes) {
                    std::printf("  cooked  plane (%.6f %.6f %.6f %.6f)\n", p.x, p.y, p.z, p.w);
                }
                for (const auto& v : s.hullVertices) {
                    std::printf("  v (%.7g %.7g %.7g)\n", v.x, v.y, v.z);
                }
                continue;
            }
            if (s.shapeType != PhysicsShapeType::Mesh) {
                continue;
            }
            std::vector<Vector3f> v;
            for (const auto& q : s.meshVertexPositions) {
                v.push_back({q.x, q.y, q.z});
            }
            std::vector<u32> t;
            if (!s.meshFaceIndices16.empty()) {
                for (std::size_t i = 0; i < std::min<std::size_t>(s.meshFaceIndex16Count, s.meshFaceIndices16.size()); ++i)
                    for (int k = 0; k < 3; ++k) t.push_back(s.meshFaceIndices16[i][static_cast<std::size_t>(k)]);
            } else {
                for (std::size_t i = 0; i < std::min<std::size_t>(s.meshFaceIndex32Count, s.meshFaceIndices32.size()); ++i)
                    for (int k = 0; k < 3; ++k) t.push_back(s.meshFaceIndices32[i][static_cast<std::size_t>(k)]);
            }
            const MeshTree tr = ComputeMeshTree(v, t);
            f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (const auto& q : v) {
                const f32 c[3] = {q.x, q.y, q.z};
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], c[k]);
                    hi[k] = std::max(hi[k], c[k]);
                }
            }
            std::printf("v=%zu t=%zu mt16=%zu nodes=%zu vcount=%u w0=%g\n", v.size(), t.size() / 3,
                        s.meshFaceIndices16.size(), s.meshBvhNodes.size(), s.meshVertexCount,
                        s.meshVertexPositions.empty() ? 0.0 : s.meshVertexPositions[0].w);
            std::printf(" shipped c=(%.9g %.9g %.9g) e=(%.9g %.9g %.9g) tol=(%.9g %.9g %.9g) h=%u\n",
                        s.meshBoundsCenter.x, s.meshBoundsCenter.y, s.meshBoundsCenter.z, s.meshBoundsExtent.x,
                        s.meshBoundsExtent.y, s.meshBoundsExtent.z, s.meshTolerance.x, s.meshTolerance.y,
                        s.meshTolerance.z, s.meshTreeDepth);
            std::printf(" cooked  c=(%.9g %.9g %.9g) e=(%.9g %.9g %.9g) tol=(%.9g %.9g %.9g) h=%u\n", tr.center.x,
                        tr.center.y, tr.center.z, tr.extent.x, tr.extent.y, tr.extent.z, tr.tolerance.x,
                        tr.tolerance.y, tr.tolerance.z, tr.height);
            std::printf(" all vertices lo=(%.9g %.9g %.9g) hi=(%.9g %.9g %.9g)\n", lo[0], lo[1], lo[2], hi[0],
                        hi[1], hi[2]);
        }
    }
}

TEST_CASE("m3 physics census", "[.census]") {
    const Matrix44f kIdentity = Matrix44f::identity();
    const auto files = PhysicsCorpus();
    std::map<std::string, std::size_t> hist;
    const auto bump = [&](const std::string& key) { ++hist[key]; };
    for (const auto& path : files) {
        Parser parser;
        const Model model = parser.parse(path.string());
        for (const ClothPhysics& c : model.clothPhysics) {
            if (!c.simEnabled.empty()) {
                bump("cage localChannels=" + std::to_string(c.localChannels));
                continue;
            }
            char buf[512];
            std::snprintf(buf, sizeof buf,
                          "collider-only cage=%u sbc=%u sb=%zu d=%g t=%g st=%g h=%g b=%g dm=%g f=%g g=%g e=%g w=%g sh=%g dr=%g l=%g sp=%g fl=%u act=(%u,%u,%u,%u,%u,%d) usc=%u so=%g se=%g ss=%g lc=%u wind=(%g,%g,%g) prox=%zu",
                          c.cageRegion, c.skinBoneCount, c.skinBones.size(), c.density, c.tracking,
                          c.stretchStiffness, c.horizontalStiffness, c.bendingStiffness, c.damping,
                          c.friction, c.gravity, c.explosionScale, c.windScale, c.shearStiffness,
                          c.dragFactor, c.liftFactor, c.sphereStiffness, c.flatten, c.active.interpType,
                          c.active.flags, c.active.animId, c.active.initValue, c.active.nullValue,
                          c.active.unused, c.useSkinCollision, c.skinOffset, c.skinExponent,
                          c.skinStiffness, c.localChannels, c.localWind.x, c.localWind.y, c.localWind.z,
                          c.proxies.size());
            bump(buf);
        }
        for (const Force& f : model.forces) {
            bump("FOR_ flags=" + std::to_string(static_cast<u32>(f.flags)));
            bump("FOR_ unknown=" + std::to_string(f.unknown));
            bump("FOR_ type/shape=" + std::to_string(static_cast<u32>(f.forceType)) + "/" +
                 std::to_string(static_cast<u32>(f.forceShape)));
        }
        for (const Warp& w : model.warps) {
            bump("WRP_ type=" + std::to_string(w.warpType) + " unknown=" + std::to_string(w.unknown));
        }
        for (const PhysicsJoint& j : model.physicsJoints) {
            bump("PHYJ enableFriction=" + std::to_string(j.enableFriction) +
                 " enableLimits=" + std::to_string(j.enableLimits));
            bool a = false, b = false;
            for (const RigidBody& rb : model.rigidBodies) {
                a = a || rb.parentBoneIndex == j.boneIndex1;
                b = b || rb.parentBoneIndex == j.boneIndex2;
            }
            if (!a || !b) {
                bump("PHYJ bone without body");
            }
        }
        for (const RigidBody& rb : model.rigidBodies) {
            for (const PhysicsShape& s : rb.rigidBodyShape) {
                const bool identity = std::memcmp(&s.transform, &kIdentity, sizeof kIdentity) == 0;
                if ((s.shapeType == PhysicsShapeType::ConvexHull || s.shapeType == PhysicsShapeType::Mesh) && !identity) {
                    bump("hull/mesh with non-identity matrix");
                }
            }
        }
    }
    for (const auto& [k, v] : hist) {
        std::printf("%6zu  %s\n", v, k.c_str());
    }
}
