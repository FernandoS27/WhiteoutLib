// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The stored triangulation and the one function that cuts faces
/// (EDIT_MODE_MODELLING_DESIGN.md §2.3, §2.6): G23 whole, and G22's row arms.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/checks.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/geometry/render_view.h>
#include <whiteout/models/wem/geometry/triangulation.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/skinning/points.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wem_corpus_files.h"
#include "whiteout/models/wem/chunk_tags.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

using geom::CutNote;
using geom::Domain;
using geom::EdgeId;
using geom::FaceId;
using geom::HalfedgeId;
using geom::Topology;
using geom::VertexId;

// ============================================================================
// Polygons (G23)
// ============================================================================

using Triple = std::array<u32, 3>;

std::vector<Triple> triples(std::span<const u32> flat) {
    std::vector<Triple> out;
    for (std::size_t t = 0; t + 2 < flat.size(); t += 3) {
        out.push_back({flat[t], flat[t + 1], flat[t + 2]});
    }
    return out;
}

/// Each triangle's corners sorted, then the list: two cuts compared as sets.
std::vector<Triple> asSet(std::vector<Triple> triangles) {
    for (Triple& triangle : triangles) {
        std::sort(triangle.begin(), triangle.end());
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

/// 2D points, optionally tilted into 3D so the Newell projection has work to do.
std::vector<Vector3f> placed(const std::vector<std::array<f64, 2>>& xy, bool tilt) {
    const f64 a = 0.7;
    const f64 b = 0.3;
    std::vector<Vector3f> out;
    for (const auto& [x, y] : xy) {
        if (!tilt) {
            out.push_back(Vector3f{static_cast<f32>(x), static_cast<f32>(y), 0.0f});
            continue;
        }
        // Rz(b), then Rx(a), then an offset.
        const f64 rx = x * std::cos(b) - y * std::sin(b);
        const f64 ry = x * std::sin(b) + y * std::cos(b);
        out.push_back(Vector3f{static_cast<f32>(rx + 5.0), static_cast<f32>(ry * std::cos(a) - 3.0),
                               static_cast<f32>(ry * std::sin(a) + 2.0)});
    }
    return out;
}

struct Frame {
    std::array<f64, 3> normal{}; ///< Unit Newell normal.
    f64 area = 0.0;              ///< The polygon's, |Newell| / 2.
    f64 longest = 0.0;           ///< The longest side, squared.
};

Frame frameOf(const std::vector<Vector3f>& p) {
    Frame frame;
    std::array<f64, 3> n{0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < p.size(); ++i) {
        const Vector3f& a = p[i];
        const Vector3f& b = p[(i + 1) % p.size()];
        n[0] += (static_cast<f64>(a.y) - b.y) * (static_cast<f64>(a.z) + b.z);
        n[1] += (static_cast<f64>(a.z) - b.z) * (static_cast<f64>(a.x) + b.x);
        n[2] += (static_cast<f64>(a.x) - b.x) * (static_cast<f64>(a.y) + b.y);
        const f64 dx = static_cast<f64>(b.x) - a.x;
        const f64 dy = static_cast<f64>(b.y) - a.y;
        const f64 dz = static_cast<f64>(b.z) - a.z;
        frame.longest = std::max(frame.longest, dx * dx + dy * dy + dz * dz);
    }
    const f64 length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    frame.area = length / 2.0;
    if (length > 0.0) {
        frame.normal = {n[0] / length, n[1] / length, n[2] / length};
    }
    return frame;
}

/// Triangle (a, b, c)'s area along the frame's normal: negative when it turns
/// against the loop.
f64 signedArea(const std::vector<Vector3f>& p, const Frame& frame, const Triple& t) {
    const Vector3f& a = p[t[0]];
    const Vector3f& b = p[t[1]];
    const Vector3f& c = p[t[2]];
    const f64 ux = static_cast<f64>(b.x) - a.x;
    const f64 uy = static_cast<f64>(b.y) - a.y;
    const f64 uz = static_cast<f64>(b.z) - a.z;
    const f64 vx = static_cast<f64>(c.x) - a.x;
    const f64 vy = static_cast<f64>(c.y) - a.y;
    const f64 vz = static_cast<f64>(c.z) - a.z;
    const f64 cx = uy * vz - uz * vy;
    const f64 cy = uz * vx - ux * vz;
    const f64 cz = ux * vy - uy * vx;
    return 0.5 * (cx * frame.normal[0] + cy * frame.normal[1] + cz * frame.normal[2]);
}

struct Cut {
    std::vector<u32> corners;
    CutNote note = CutNote::None;
};

/// The polygon's loop is 0..n-1, so a corner ordinal is its vertex id.
Cut cutOf(const std::vector<Vector3f>& points, std::span<const u32> row = {}) {
    std::vector<u32> loop(points.size());
    std::iota(loop.begin(), loop.end(), 0u);
    Cut out;
    out.note = geom::TriangulateFace(loop, points, row, out.corners);
    return out;
}

struct Polygon {
    const char* name;
    std::vector<std::array<f64, 2>> xy;
    bool simple = true;      ///< The area sum must equal the polygon's.
    bool positiveCut = true; ///< A cut with every triangle facing exists.
};

std::vector<std::array<f64, 2>> ring(u32 count, f64 radius, f64 inner = 0.0) {
    std::vector<std::array<f64, 2>> out;
    for (u32 i = 0; i < count; ++i) {
        const f64 angle = 2.0 * 3.14159265358979323846 * i / count;
        const f64 r = (inner > 0.0 && i % 2 == 1) ? inner : radius;
        out.push_back({r * std::cos(angle), r * std::sin(angle)});
    }
    return out;
}

std::vector<Polygon> g23Polygons() {
    std::vector<Polygon> out;
    out.push_back({"concave quad, reflex at 1", {{0, 0}, {2, 1}, {4, 0}, {2, 3}}});
    out.push_back({"concave quad, reflex at 0", {{2, 1}, {4, 0}, {2, 3}, {0, 0}}});
    out.push_back({"folded quad", {{0, 0}, {4, 0}, {0, 1}, {1, -1}}, false, false});
    out.push_back({"square", {{0, 0}, {1, 0}, {1, 1}, {0, 1}}});
    out.push_back({"kite, 1-3 shorter", {{0, 0}, {2, -0.5}, {4, 0}, {2, 0.5}}});
    out.push_back({"T-vertex quad", {{0, 0}, {1, 0}, {2, 0}, {1, 3}}});
    out.push_back({"star", ring(10, 1.0, 0.4)});
    out.push_back({"comb",
                   {{0, 0}, {7, 0}, {7, 3}, {6, 3}, {6, 1}, {5, 1}, {5, 3}, {4, 3}, {4, 1},
                    {3, 1}, {3, 3}, {2, 3}, {2, 1}, {1, 1}, {1, 3}, {0, 3}}});
    {
        // A crescent: an outer arc out, an inner arc back, every inner corner reflex.
        Polygon crescent{"reflex chain", {}};
        for (u32 i = 0; i <= 8; ++i) {
            const f64 angle = 3.14159265358979323846 * i / 8.0;
            crescent.xy.push_back({2.0 * std::cos(angle), 2.0 * std::sin(angle)});
        }
        for (u32 i = 8; i-- > 1;) {
            const f64 angle = 3.14159265358979323846 * i / 8.0;
            crescent.xy.push_back({1.0 * std::cos(angle), 1.0 * std::sin(angle) + 0.2});
        }
        out.push_back(crescent);
    }
    out.push_back({"collinear corners", {{0, 0}, {1, 0}, {2, 0}, {2, 1}, {0, 1}}});
    // The reflex corner lies exactly on the 1-4 diagonal: an ear there holds it on
    // its boundary, and clipping it leaves a triangle of no area.
    out.push_back({"a corner on a diagonal", {{0, 0}, {4, 0}, {4, 4}, {2, 2}, {0, 4}}});
    out.push_back({"coincident corners", {{0, 0}, {2, 0}, {2, 2}, {2, 2}, {0, 2}}, false, false});
    out.push_back({"sliver", {{0, 0}, {10, 0}, {10, 0.001}, {5, 0.0015}, {0, 0.001}}});
    out.push_back({"regular 12-gon", ring(12, 1.0)});
    out.push_back({"self-intersecting hexagon",
                   {{3, 3}, {4, 2}, {2, 3}, {4, 3}, {0, 5}, {2, 1}},
                   false,
                   false});
    return out;
}

// ============================================================================
// Meshes
// ============================================================================

/// A mesh straight from a face set, one section, no repair.
Mesh fromFaces(const std::vector<std::vector<u32>>& corners, const std::vector<Vector3f>& positions) {
    geom::FaceSet faces;
    faces.vertexCount = static_cast<u32>(positions.size());
    for (const std::vector<u32>& face : corners) {
        faces.addFace(face);
    }
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> written = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    std::copy(positions.begin(), positions.end(), written.begin());
    mesh.attributes.getOrCreate<u32>(geom::names::kSection, Domain::Face, geom::AttrType::U32);
    mesh.sections.emplace_back();
    return mesh;
}

/// A `w` x `h` grid of parallelograms, x sheared by 0.4 y: each cell's 1-3
/// diagonal is the shorter, so the automatic rule cuts there.
Mesh shearedQuads(u32 w, u32 h, f32 dx = 0.0f) {
    std::vector<Vector3f> positions;
    for (u32 y = 0; y <= h; ++y) {
        for (u32 x = 0; x <= w; ++x) {
            positions.push_back(Vector3f{static_cast<f32>(x) + 0.4f * static_cast<f32>(y) + dx,
                                         static_cast<f32>(y), 0.0f});
        }
    }
    const auto at = [&](u32 x, u32 y) { return y * (w + 1) + x; };
    std::vector<std::vector<u32>> faces;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            faces.push_back({at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)});
        }
    }
    Mesh mesh = fromFaces(faces, positions);
    REQUIRE(mesh.ensureConnectivity().ok());
    return mesh;
}

/// Stores, for every quad, the cut along its LONGER diagonal: valid, and never
/// what the automatic rule picks, so a lost row shows.
void storeLongerCuts(Mesh& mesh) {
    const geom::FaceSet& faces = mesh.faceSet();
    const std::vector<u32> slots = geom::FaceSetSlots(mesh);
    const std::span<const Vector3f> p =
        std::as_const(mesh).attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const u32 slotCount = mesh.hasConnectivity() ? std::as_const(mesh).topology().faceCount()
                                                 : static_cast<u32>(faces.faceCount());
    geom::FaceTriangulationBuilder builder(slotCount);
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        const u32 valence = faces.faceValence[f];
        const u32* v = faces.cornerVertex.data() + corner;
        corner += valence;
        if (valence != 4) {
            continue;
        }
        const auto length = [&](u32 a, u32 b) {
            const f32 x = p[a].x - p[b].x;
            const f32 y = p[a].y - p[b].y;
            const f32 z = p[a].z - p[b].z;
            return x * x + y * y + z * z;
        };
        const std::vector<u32> row = length(v[0], v[2]) >= length(v[1], v[3])
                                         ? std::vector<u32>{v[0], v[1], v[2], v[0], v[2], v[3]}
                                         : std::vector<u32>{v[1], v[2], v[3], v[1], v[3], v[0]};
        builder.set(slots[f], row);
    }
    mesh.triangulation = builder.build();
}

using PositionTriple = std::array<std::array<f32, 3>, 3>;

std::array<f32, 3> pointOf(const Vector3f& v) {
    return {v.x, v.y, v.z};
}

/// Every drawn triangle as positions, in emission order, first corner first.
std::vector<PositionTriple> drawnOrdered(const Mesh& mesh) {
    std::vector<u32> triangles;
    geom::TriangulateMesh(mesh, triangles);
    const std::span<const Vector3f> p =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    std::vector<PositionTriple> out;
    for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
        out.push_back({pointOf(p[triangles[t]]), pointOf(p[triangles[t + 1]]),
                       pointOf(p[triangles[t + 2]])});
    }
    return out;
}

/// The same, each triangle's corners sorted and the list sorted.
std::vector<PositionTriple> drawnSet(const Mesh& mesh) {
    std::vector<PositionTriple> out = drawnOrdered(mesh);
    for (PositionTriple& triangle : out) {
        std::sort(triangle.begin(), triangle.end());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// How many polygons draw from their stored row, and how many do not.
std::pair<u32, u32> storedAndNot(const Mesh& mesh) {
    const geom::FaceSet& faces = mesh.faceSet();
    const std::vector<u32> slots = geom::FaceSetSlots(mesh);
    const std::span<const Vector3f> p =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    std::pair<u32, u32> out{0, 0};
    std::size_t corner = 0;
    std::vector<u32> cut;
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        const u32 valence = faces.faceValence[f];
        const std::span<const u32> loop(faces.cornerVertex.data() + corner, valence);
        corner += valence;
        if (valence < 4) {
            continue;
        }
        cut.clear();
        const CutNote note = geom::TriangulateFace(loop, p, mesh.triangulation.row(slots[f]), cut);
        (note == CutNote::Stored ? out.first : out.second) += 1;
    }
    return out;
}

std::string errors(const Mesh& mesh) {
    Diagnostics diagnostics;
    geom::CheckStructural(mesh, 0, diagnostics);
    geom::CheckManifold(mesh, 0, diagnostics);
    return diagnostics.hasErrors() ? diagnostics.formatHistogram() : std::string();
}

Document documentOf(const Mesh& mesh) {
    Document document;
    document.declare(ProfileId::Generic);
    document.defaultProfile = ProfileId::Generic;
    Model model;
    model.name = "triangulation";
    model.meshes.push_back(mesh);
    document.models.push_back(std::move(model));
    return document;
}

std::optional<Document> reread(std::span<const u8> bytes) {
    Parser parser;
    return parser.parse(bytes);
}

} // namespace

// ============================================================================
// G23: the one function
// ============================================================================

TEST_CASE("wem triangulation every polygon cuts into n - 2 triangles",
          "[wem][geometry][triangulation]") {
    for (const bool tilt : {false, true}) {
        for (const Polygon& polygon : g23Polygons()) {
            CAPTURE(polygon.name, tilt);
            const std::vector<Vector3f> points = placed(polygon.xy, tilt);
            const u32 n = static_cast<u32>(points.size());
            const Cut cut = cutOf(points);
            REQUIRE(cut.corners.size() == 3u * (n - 2));
            const Frame frame = frameOf(points);
            f64 sum = 0.0;
            f64 least = 1e30;
            for (const Triple& t : triples(cut.corners)) {
                CHECK(t[0] < n);
                CHECK(t[1] < n);
                CHECK(t[2] < n);
                CHECK((t[0] != t[1] && t[1] != t[2] && t[0] != t[2]));
                const f64 area = signedArea(points, frame, t);
                sum += area;
                least = std::min(least, area);
            }
            if (polygon.simple) {
                CHECK(std::abs(sum - frame.area) <= 1e-4 * frame.area);
            }
            if (polygon.positiveCut) {
                // No triangle against the normal, and none of zero area, where a
                // positive cut exists: the T-vertex and the collinear corners.
                CHECK(least > 1e-6 * frame.longest);
            }
        }
    }
}

TEST_CASE("wem triangulation the quad rule", "[wem][geometry][triangulation]") {
    const auto setOf = [](const std::vector<std::array<f64, 2>>& xy) {
        return asSet(triples(cutOf(placed(xy, true)).corners));
    };
    const std::vector<Triple> across02 = asSet({{0, 1, 2}, {0, 2, 3}});
    const std::vector<Triple> across13 = asSet({{1, 2, 3}, {1, 3, 0}});

    SECTION("a square ties, and takes 0-2") {
        const Cut cut = cutOf(placed({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, true));
        CHECK(asSet(triples(cut.corners)) == across02);
        CHECK(cut.note == CutNote::None);
    }
    SECTION("the shorter of two valid diagonals") {
        CHECK(setOf({{0, 0}, {2, -0.5}, {4, 0}, {2, 0.5}}) == across13);
        CHECK(setOf({{0, 0}, {0.5, -2}, {1, 0}, {0.5, 2}}) == across02);
    }
    SECTION("a concave quad takes its one valid diagonal, however long") {
        CHECK(setOf({{0, 0}, {2, 1}, {4, 0}, {2, 3}}) == across13);
        CHECK(setOf({{2, 1}, {4, 0}, {2, 3}, {0, 0}}) == across02);
    }
    SECTION("a T-vertex quad never takes the diagonal through the T") {
        // 0-2 is the shorter diagonal, and runs through corner 1.
        CHECK(setOf({{0, 0}, {1, 0}, {2, 0}, {1, 3}}) == across13);
    }
    SECTION("a folded quad takes 0-2, noted") {
        const Cut cut = cutOf(placed({{0, 0}, {4, 0}, {0, 1}, {1, -1}}, true));
        CHECK(asSet(triples(cut.corners)) == across02);
        CHECK(cut.note == CutNote::Folded);
    }
    SECTION("the emission order is 0-1-2, 0-2-3 or 1-2-3, 1-3-0") {
        CHECK(cutOf(placed({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, false)).corners ==
              std::vector<u32>{0, 1, 2, 0, 2, 3});
        CHECK(cutOf(placed({{0, 0}, {2, -0.5}, {4, 0}, {2, 0.5}}, false)).corners ==
              std::vector<u32>{1, 2, 3, 1, 3, 0});
    }
}

TEST_CASE("wem triangulation a triangle and a line", "[wem][geometry][triangulation]") {
    CHECK(cutOf(placed({{0, 0}, {1, 0}, {0, 1}}, true)).corners == std::vector<u32>{0, 1, 2});
    // No Newell normal: the fan from the first corner, noted.
    const Cut line = cutOf(placed({{0, 0}, {1, 0}, {2, 0}, {3, 0}, {1.5, 0}}, true));
    CHECK(line.note == CutNote::Degenerate);
    CHECK(line.corners == std::vector<u32>{0, 1, 2, 0, 2, 3, 0, 3, 4});
}

namespace {

/// The smallest angle of triangle @p t, in the xy plane.
f64 minAngle2d(const std::vector<Vector3f>& p, const Triple& t) {
    f64 least = 4.0;
    for (u32 k = 0; k < 3; ++k) {
        const Vector3f& a = p[t[k]];
        const Vector3f& b = p[t[(k + 1) % 3]];
        const Vector3f& c = p[t[(k + 2) % 3]];
        const f64 ux = static_cast<f64>(b.x) - a.x;
        const f64 uy = static_cast<f64>(b.y) - a.y;
        const f64 wx = static_cast<f64>(c.x) - a.x;
        const f64 wy = static_cast<f64>(c.y) - a.y;
        least = std::min(least, std::atan2(std::abs(ux * wy - uy * wx), ux * wx + uy * wy));
    }
    return least;
}

f64 cross2d(const Vector3f& a, const Vector3f& b, const Vector3f& c) {
    return (static_cast<f64>(b.x) - a.x) * (static_cast<f64>(c.y) - a.y) -
           (static_cast<f64>(b.y) - a.y) * (static_cast<f64>(c.x) - a.x);
}

/// Interior diagonals of a planar (xy) cut whose flip would gain more than the
/// tie tolerance in the smaller minimum angle: Lawson left them undone.
u32 improvable(const std::vector<Vector3f>& p, const std::vector<u32>& corners) {
    const std::vector<Triple> cut = triples(corners);
    const u32 n = static_cast<u32>(p.size());
    u32 count = 0;
    for (std::size_t i = 0; i < cut.size(); ++i) {
        for (u32 side = 0; side < 3; ++side) {
            const u32 a = cut[i][side];
            const u32 b = cut[i][(side + 1) % 3];
            const u32 c = cut[i][(side + 2) % 3];
            if ((a + 1) % n == b) {
                continue;
            }
            for (std::size_t j = i + 1; j < cut.size(); ++j) {
                for (u32 s = 0; s < 3; ++s) {
                    if (cut[j][s] != b || cut[j][(s + 1) % 3] != a) {
                        continue;
                    }
                    const u32 d = cut[j][(s + 2) % 3];
                    const bool convex = cross2d(p[a], p[d], p[b]) > 0 && cross2d(p[d], p[b], p[c]) > 0 &&
                                        cross2d(p[b], p[c], p[a]) > 0 && cross2d(p[c], p[a], p[d]) > 0;
                    const f64 before = std::min(minAngle2d(p, {a, b, c}), minAngle2d(p, {b, a, d}));
                    const f64 after = std::min(minAngle2d(p, {c, a, d}), minAngle2d(p, {d, b, c}));
                    count += convex && after > before + 1e-6 ? 1u : 0u;
                }
            }
        }
    }
    return count;
}

} // namespace

TEST_CASE("wem triangulation a cocircular polygon's cut is stable",
          "[wem][geometry][triangulation]") {
    const std::vector<Vector3f> regular = placed(ring(12, 1.0), false);
    const Cut once = cutOf(regular);
    CHECK(improvable(regular, once.corners) == 0u);

    SECTION("a drag at the corners' own rounding does not flip it") {
        for (u32 k = 1; k <= 4; ++k) {
            std::vector<Vector3f> nudged = regular;
            for (u32 i = 0; i < nudged.size(); ++i) {
                const f32 step = ((i * 7 + k * 3) % 5 == 0 ? 1.0f : -1.0f) * 6e-8f * static_cast<f32>(k);
                nudged[i].x += step;
                nudged[i].y -= step * 0.5f;
            }
            CAPTURE(k);
            CHECK(cutOf(nudged).corners == once.corners);
        }
    }
    SECTION("a real gain is still taken, past the ties before it") {
        // Corner 5 pushed out: flipping the diagonal it sits under gains, and
        // every earlier pair is a tie that must not flip back and forth first.
        std::vector<std::array<f64, 2>> xy = ring(12, 1.0);
        xy[5] = {xy[5][0] * 1.08, xy[5][1] * 1.08};
        const std::vector<Vector3f> pushed = placed(xy, false);
        const Cut cut = cutOf(pushed);
        REQUIRE(cut.corners.size() == 30u);
        CHECK(improvable(pushed, cut.corners) == 0u);
    }
}

TEST_CASE("wem triangulation the stored row is used when valid", "[wem][geometry][triangulation]") {
    // A convex pentagon, and a fan from corner 2 the automatic rule does not pick.
    const std::vector<Vector3f> points = placed({{0, 0}, {2, 0}, {3, 1.5}, {1, 3}, {-1, 1.5}}, true);
    const std::vector<u32> row{2, 3, 4, 2, 4, 0, 2, 0, 1};
    REQUIRE(asSet(triples(cutOf(points).corners)) != asSet(triples(row)));
    const Cut stored = cutOf(points, row);
    CHECK(stored.note == CutNote::Stored);
    CHECK(stored.corners == row); // ordinals are the ids here
}

// ============================================================================
// §2.3's validity: each failure falls back and notes
// ============================================================================

TEST_CASE("wem triangulation each invalid row falls back and notes",
          "[wem][geometry][triangulation]") {
    // A convex pentagon (0..4) and a triangle (5, 6, 7) beside it.
    const std::vector<Vector3f> positions{{0, 0, 0}, {2, 0, 0},  {3, 1.5f, 0}, {1, 3, 0},
                                          {-1, 1.5f, 0}, {5, 0, 0}, {6, 0, 0}, {5, 1, 0}};
    struct Bad {
        const char* name;
        std::vector<u32> row;
    };
    const std::vector<Bad> bad{
        {"too few triangles", {0, 1, 2, 0, 2, 3}},
        {"a repeated corner", {0, 1, 1, 0, 2, 3, 0, 3, 4}},
        {"a vertex of another face", {0, 1, 5, 0, 2, 3, 0, 3, 4}},
        {"every triangle against the loop", {0, 2, 1, 0, 3, 2, 0, 4, 3}},
        {"a diagonal only one way", {0, 1, 2, 0, 2, 3, 0, 2, 4}},
        {"a loop edge run backwards", {1, 0, 2, 0, 2, 3, 0, 3, 4}},
    };
    for (const Bad& entry : bad) {
        CAPTURE(entry.name);
        Mesh mesh = fromFaces({{0, 1, 2, 3, 4}, {5, 6, 7}}, positions);
        mesh.triangulation.setRow(0, entry.row, 2);
        const std::vector<u32> loop{0, 1, 2, 3, 4};
        CHECK_FALSE(geom::RowValid(loop, positions, entry.row));
        std::vector<u32> cut;
        CHECK(geom::TriangulateFace(loop, positions, entry.row, cut) != CutNote::Stored);
        CHECK(cut.size() == 9u);
        Diagnostics diagnostics;
        geom::CheckStructural(mesh, 0, diagnostics);
        CHECK(diagnostics.countOf(DiagCode::StaleTriangulation) == 1u);
        CHECK_FALSE(diagnostics.hasErrors());
    }

    SECTION("a zero-area triangle") {
        const std::vector<Vector3f> flat{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}};
        const std::vector<u32> loop{0, 1, 2, 3, 4};
        CHECK_FALSE(geom::RowValid(loop, flat, std::vector<u32>{0, 1, 2, 0, 2, 3, 0, 3, 4}));
        CHECK(geom::RowValid(loop, flat, std::vector<u32>{1, 2, 3, 1, 3, 4, 1, 4, 0}));
    }
    SECTION("a row on a triangle is a Structural error") {
        Mesh mesh = fromFaces({{0, 1, 2, 3, 4}, {5, 6, 7}}, positions);
        mesh.triangulation.setRow(1, std::vector<u32>{5, 6, 7}, 2);
        Diagnostics diagnostics;
        geom::CheckStructural(mesh, 0, diagnostics);
        CHECK(diagnostics.countOf(DiagCode::TriangulationMalformed) == 1u);
    }
    SECTION("a mis-shaped table is a Structural error") {
        Mesh mesh = fromFaces({{0, 1, 2, 3, 4}, {5, 6, 7}}, positions);
        mesh.triangulation.offsets = {0, 9};
        mesh.triangulation.vertices = {0, 1, 2, 0, 2, 3, 0, 3, 4};
        Diagnostics diagnostics;
        geom::CheckStructural(mesh, 0, diagnostics);
        CHECK(diagnostics.countOf(DiagCode::TriangulationMalformed) == 1u);
    }
}

TEST_CASE("wem triangulation Validate names duplicate edges and mistyped reserved layers",
          "[wem][geometry][triangulation]") {
    SECTION("two edges between one pair of vertices") {
        Mesh mesh = fromFaces({{0, 1, 2}, {4, 3, 5}},
                              {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}});
        REQUIRE(mesh.ensureConnectivity().ok());
        // Re-point 3 -> 4's edge at 1 -> 0 by hand: the edit a missing check allowed.
        Topology& topology = mesh.topology();
        const HalfedgeId h = topology.findHalfedge(VertexId(4), VertexId(3));
        REQUIRE(h.valid());
        topology.setTo(h, VertexId(0));
        topology.setTo(Topology::opposite(h), VertexId(1));
        Diagnostics diagnostics;
        geom::CheckManifold(mesh, 0, diagnostics);
        CHECK(diagnostics.countOf(DiagCode::DuplicateEdge) == 1u);
    }
    SECTION("a reserved name on the wrong domain, or with the wrong type") {
        Mesh mesh = shearedQuads(1, 1);
        mesh.attributes.create(geom::names::kNormal, Domain::Vertex, geom::AttrType::F32x3);
        mesh.attributes.getOrCreate<f32>(geom::names::kCrease, Domain::Edge, geom::AttrType::F32);
        mesh.attributes.layer(geom::names::kCrease, Domain::Edge)->type = geom::AttrType::U32;
        Diagnostics diagnostics;
        geom::CheckStructural(mesh, 0, diagnostics);
        CHECK(diagnostics.countOf(DiagCode::ReservedLayerMistyped) == 2u);
    }
    SECTION("an n-gon is no longer a profile error") {
        Document document = documentOf(shearedQuads(2, 1));
        document.declare(ProfileId::Wc3Reforged);
        const Diagnostics diagnostics = Validate(document, ValidateLevel::Profile);
        CHECK(diagnostics.countOf(DiagCode::NgonUnsupported) == 0u);
    }
}

// ============================================================================
// G22: rows survive
// ============================================================================

TEST_CASE("wem triangulation rows survive GarbageCollect", "[wem][geometry][triangulation]") {
    Mesh mesh = shearedQuads(3, 3);
    storeLongerCuts(mesh);
    // A join leaves a deleted face behind; its union row is a hexagon's.
    const Topology& topology = std::as_const(mesh).topology();
    const HalfedgeId shared = topology.findHalfedge(VertexId(5), VertexId(6));
    REQUIRE(shared.valid());
    REQUIRE(geom::DissolveEdge(mesh, Topology::edge(shared)));
    const auto before = drawnSet(mesh);
    REQUIRE(storedAndNot(mesh) == std::pair<u32, u32>{8, 0});
    geom::GarbageCollect(mesh);
    CHECK(drawnSet(mesh) == before);
    CHECK(storedAndNot(mesh) == std::pair<u32, u32>{8, 0});
    CHECK(errors(mesh) == "");
}

TEST_CASE("wem triangulation rows survive a weld", "[wem][geometry][triangulation]") {
    // Four quads, each with vertices of its own: a soup the weld joins.
    std::vector<Vector3f> positions;
    std::vector<std::vector<u32>> faces;
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 2; ++x) {
            const u32 base = static_cast<u32>(positions.size());
            for (const auto& [cx, cy] : std::array<std::array<u32, 2>, 4>{
                     {{x, y}, {x + 1, y}, {x + 1, y + 1}, {x, y + 1}}}) {
                positions.push_back(Vector3f{static_cast<f32>(cx) + 0.4f * static_cast<f32>(cy),
                                             static_cast<f32>(cy), 0.0f});
            }
            faces.push_back({base, base + 1, base + 2, base + 3});
        }
    }
    Mesh mesh = fromFaces(faces, positions);
    REQUIRE(mesh.ensureConnectivity().ok());
    storeLongerCuts(mesh);
    const auto before = drawnSet(mesh);
    const geom::WeldResult welded = geom::WeldVertices(mesh, 1e-4f, false);
    REQUIRE(welded.verticesMerged == 7u);
    CHECK(mesh.vertexCount() == 9u);
    CHECK(drawnSet(mesh) == before);
    CHECK(storedAndNot(mesh) == std::pair<u32, u32>{4, 0});
}

TEST_CASE("wem triangulation rows survive a merge and a split", "[wem][geometry][triangulation]") {
    Mesh first = shearedQuads(2, 1);
    Mesh second = shearedQuads(1, 2, 10.0f);
    storeLongerCuts(first);
    storeLongerCuts(second);

    const std::array<Mesh, 2> inputs{first, second};
    Mesh merged = geom::MergeMeshes(inputs);
    std::vector<PositionTriple> expected = drawnSet(first);
    const std::vector<PositionTriple> more = drawnSet(second);
    expected.insert(expected.end(), more.begin(), more.end());
    std::sort(expected.begin(), expected.end());
    CHECK(drawnSet(merged) == expected);
    CHECK(storedAndNot(merged) == std::pair<u32, u32>{4, 0});

    // Merged, the two inputs are two sections: the split gives them back.
    REQUIRE(merged.sections.size() == 2u);
    const std::vector<Mesh> parts = geom::SplitMesh(merged);
    REQUIRE(parts.size() == 2u);
    CHECK(drawnSet(parts[0]) == drawnSet(first));
    CHECK(drawnSet(parts[1]) == drawnSet(second));
    CHECK(storedAndNot(parts[0]) == std::pair<u32, u32>{2, 0});
    CHECK(storedAndNot(parts[1]) == std::pair<u32, u32>{2, 0});
}

TEST_CASE("wem triangulation rows turn with a reversed winding", "[wem][geometry][triangulation]") {
    // Three sheared quads in a row, the middle one wound backwards.
    const std::vector<Vector3f> positions{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0},
                                          {0.4f, 1, 0}, {1.4f, 1, 0}, {2.4f, 1, 0}, {3.4f, 1, 0}};
    // The middle face runs 1, 5, 6, 2: clockwise, against both neighbours, so
    // the face set does not build until the winding is unified.
    Mesh mesh = fromFaces({{0, 1, 5, 4}, {1, 5, 6, 2}, {2, 3, 7, 6}}, positions);
    storeLongerCuts(mesh);
    REQUIRE(storedAndNot(mesh) == std::pair<u32, u32>{3, 0});
    const auto before = drawnSet(mesh);
    REQUIRE(geom::UnifyWinding(mesh) == 1u);
    CHECK(drawnSet(mesh) == before);
    CHECK(storedAndNot(mesh) == std::pair<u32, u32>{3, 0});
    CHECK(errors(mesh) == "");
}

TEST_CASE("wem triangulation a relink renames one face's row only",
          "[wem][geometry][triangulation]") {
    geom::FaceTriangulation table;
    table.setRow(0, std::vector<u32>{0, 1, 2, 0, 2, 3}, 2);
    table.setRow(1, std::vector<u32>{1, 4, 5, 1, 5, 2}, 2);
    // Face 0 relinked onto 6 and 7, duplicates of 1 and 2; face 1 keeps them.
    table.rewriteRow(0, std::vector<u32>{1, 2}, std::vector<u32>{6, 7});
    CHECK(std::vector<u32>(table.row(0).begin(), table.row(0).end()) ==
          std::vector<u32>{0, 6, 7, 0, 7, 3});
    CHECK(std::vector<u32>(table.row(1).begin(), table.row(1).end()) ==
          std::vector<u32>{1, 4, 5, 1, 5, 2});

    SECTION("and a collapse renames its vertex in the faces around it") {
        Mesh mesh = shearedQuads(2, 2);
        storeLongerCuts(mesh);
        // Collapse 4 (the centre) into 1: every quad around 4 but the two
        // beside the edge keeps its cycle, and names 1 where it named 4.
        const HalfedgeId h = std::as_const(mesh).topology().findHalfedge(VertexId(4), VertexId(1));
        REQUIRE(h.valid());
        REQUIRE(geom::IsCollapseLegal(mesh, h));
        REQUIRE(geom::CollapseEdge(mesh, h));
        for (const u32 v : mesh.triangulation.vertices) {
            CHECK(v != 4u);
        }
        CHECK(errors(mesh) == "");
    }
}

TEST_CASE("wem triangulation rows survive a .wem round trip", "[wem][geometry][triangulation]") {
    Mesh mesh = shearedQuads(3, 2);
    storeLongerCuts(mesh);
    Writer writer;
    const std::vector<u8> bytes = writer.write(documentOf(mesh));
    const std::optional<Document> read = reread(bytes);
    REQUIRE(read.has_value());
    const Mesh& back = read->models.at(0).meshes.at(0);
    CHECK(back.triangulation.offsets == mesh.triangulation.offsets);
    CHECK(back.triangulation.vertices == mesh.triangulation.vertices);
    CHECK(drawnOrdered(back) == drawnOrdered(mesh));
    CHECK(errors(back) == "");
}

TEST_CASE("wem triangulation a v1 MESH reads with no table", "[wem][geometry][triangulation]") {
    // Every MESH chunk stamped version 1, as a writer from before the table
    // would have: one mesh, so the table is all that follows the bounds.
    Mesh mesh = shearedQuads(2, 2);
    storeLongerCuts(mesh);
    Writer writer;
    std::vector<u8> bytes = writer.write(documentOf(mesh));
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    u32 stamped = 0;
    for (u32 i = 0; i < header.indexCount; ++i) {
        IndexEntry entry{};
        u8* at = bytes.data() + header.indexOffset + i * sizeof(IndexEntry);
        std::memcpy(&entry, at, sizeof(entry));
        if (entry.tag != ChunkTagTraits<Mesh>::value) {
            continue;
        }
        CHECK(entry.version == 2u);
        entry.version = 1;
        std::memcpy(at, &entry, sizeof(entry));
        ++stamped;
    }
    REQUIRE(stamped == 1u);
    const std::optional<Document> read = reread(bytes);
    REQUIRE(read.has_value());
    const Mesh& back = read->models.at(0).meshes.at(0);
    CHECK(back.triangulation.empty());
    CHECK(back.faceSet().cornerVertex == mesh.faceSet().cornerVertex);
    CHECK(back.bounds.minimum.x == mesh.bounds.minimum.x);
    CHECK(errors(back) == "");
}

// ============================================================================
// Maintenance: joins, MaterialiseRows
// ============================================================================

TEST_CASE("wem triangulation a join stores the union, lower face first",
          "[wem][geometry][triangulation]") {
    // Two triangles, (0, 1, 3) and (0, 3, 2); keep the higher-numbered one.
    Mesh mesh = fromFaces({{0, 1, 3}, {0, 3, 2}}, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1.2f, 1, 0}});
    REQUIRE(mesh.ensureConnectivity().ok());
    const HalfedgeId diagonal = std::as_const(mesh).topology().findHalfedge(VertexId(0), VertexId(3));
    REQUIRE(geom::DissolveEdge(mesh, Topology::edge(diagonal), FaceId(1)));
    const std::span<const u32> row = mesh.triangulation.row(1);
    CHECK(std::vector<u32>(row.begin(), row.end()) == std::vector<u32>{0, 1, 3, 0, 3, 2});
    CHECK(mesh.triangulation.row(0).empty());
    CHECK(storedAndNot(mesh) == std::pair<u32, u32>{1, 0});
    CHECK(errors(mesh) == "");
}

TEST_CASE("wem triangulation MaterialiseRows replaces a row a move made invalid",
          "[wem][geometry][triangulation]") {
    Mesh mesh = shearedQuads(2, 1);
    SECTION("an empty row is written") {
        CHECK(geom::MaterialiseRows(mesh) == 2u);
        CHECK(storedAndNot(mesh) == std::pair<u32, u32>{2, 0});
        CHECK(geom::MaterialiseRows(mesh) == 0u);
    }
    SECTION("a stale row is replaced") {
        storeLongerCuts(mesh);
        // Face 0 is (0, 1, 4, 3) cut 0-4. Pull 1 across the 0-4 diagonal: the
        // stored (0, 1, 4) turns against the loop, while 1-3 still cuts.
        const std::span<Vector3f> p =
            mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex);
        p[1] = Vector3f{1.3f, 0.95f, 0.0f};
        REQUIRE(storedAndNot(mesh) == std::pair<u32, u32>{1, 1});
        CHECK(geom::MaterialiseRows(mesh) == 1u);
        CHECK(storedAndNot(mesh) == std::pair<u32, u32>{2, 0});
    }
}

// ============================================================================
// Consumers: the render view, the export map, the triangle lists
// ============================================================================

TEST_CASE("wem triangulation the render view draws the stored rows",
          "[wem][geometry][triangulation]") {
    Mesh mesh = shearedQuads(2, 2);
    storeLongerCuts(mesh);
    geom::RenderMeshDesc desc = geom::RenderMeshDesc::Standard();
    desc.splitBySection = false;
    const geom::RenderMesh render = geom::BuildRenderMesh(mesh, desc);
    std::vector<u32> expected;
    std::vector<u32> faceOf;
    geom::TriangulateMesh(mesh, expected, &faceOf);
    std::vector<u32> drawn;
    for (const u32 index : render.indices) {
        drawn.push_back(render.vertexToWemVertex[index]);
    }
    // One list for the picture, the pick, Heat and the transfer.
    CHECK(drawn == expected);
    CHECK(render.triangleFace == faceOf);
    CHECK(storedAndNot(mesh) == std::pair<u32, u32>{4, 0});

    SECTION("and the fan only when asked") {
        // No rows: the rule cuts every cell 1-3, the fan 0-2.
        const Mesh automatic = shearedQuads(2, 2);
        geom::RenderMeshDesc fanDesc = desc;
        fanDesc.triangulation = geom::TriangulationPolicy::FanFromFirstHalfedge;
        const geom::RenderMesh fan = geom::BuildRenderMesh(automatic, fanDesc);
        CHECK(fan.indices != geom::BuildRenderMesh(automatic, desc).indices);
        CHECK(fan.triangleFace == faceOf);
    }
}

TEST_CASE("wem triangulation MdxGeosetElements answers per geoset triangle",
          "[wem][geometry][triangulation]") {
    Mesh mesh = shearedQuads(2, 2);
    storeLongerCuts(mesh);
    const Document document = documentOf(mesh);
    const std::vector<DrawnElements> elements =
        MdxGeosetElements(document, 0, ProfileId::Wc3Reforged);
    REQUIRE(elements.size() == 1u);
    const DrawnElements& geoset = elements[0];
    std::vector<u32> expected;
    std::vector<u32> faceOf;
    geom::TriangulateMesh(mesh, expected, &faceOf);
    REQUIRE(geoset.triangles.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(geoset.vertices[geoset.triangles[i]] == expected[i]);
    }
    CHECK(geoset.triangleFaces == faceOf);
    REQUIRE(geoset.halfedges.size() == geoset.vertices.size());
    for (std::size_t v = 0; v < geoset.vertices.size(); ++v) {
        CHECK(std::as_const(mesh).topology().from(geoset.halfedges[v]).value() == geoset.vertices[v]);
    }
    const std::vector<std::vector<u32>> vertices = MdxGeosetVertices(document, 0, ProfileId::Wc3Reforged);
    REQUIRE(vertices.size() == 1u);
    CHECK(vertices[0] == geoset.vertices);
}

TEST_CASE("wem triangulation joining triangles into quads leaves the point ring alone",
          "[wem][geometry][triangulation]") {
    // The ring holds a polygon's drawn diagonals too, so Skin's Grow and Smooth
    // see the same neighbours after a join as before it.
    std::vector<Vector3f> positions;
    std::vector<std::vector<u32>> triangles;
    for (u32 y = 0; y <= 3; ++y) {
        for (u32 x = 0; x <= 3; ++x) {
            positions.push_back(Vector3f{static_cast<f32>(x) + 0.4f * static_cast<f32>(y),
                                         static_cast<f32>(y), 0.0f});
        }
    }
    for (u32 y = 0; y < 3; ++y) {
        for (u32 x = 0; x < 3; ++x) {
            const u32 a = y * 4 + x;
            triangles.push_back({a, a + 1, a + 5});
            triangles.push_back({a, a + 5, a + 4});
        }
    }
    Mesh mesh = fromFaces(triangles, positions);
    REQUIRE(mesh.ensureConnectivity().ok());
    const skinning::PointTable before = skinning::BuildPointTable(mesh);
    u32 joined = 0;
    for (u32 y = 0; y < 3; ++y) {
        for (u32 x = 0; x < 3; ++x) {
            const u32 a = y * 4 + x;
            const HalfedgeId h = std::as_const(mesh).topology().findHalfedge(VertexId(a), VertexId(a + 5));
            REQUIRE(h.valid());
            joined += geom::DissolveEdge(mesh, Topology::edge(h)) ? 1u : 0u;
        }
    }
    REQUIRE(joined == 9u);
    REQUIRE(storedAndNot(mesh) == std::pair<u32, u32>{9, 0});
    const skinning::PointTable after = skinning::BuildPointTable(mesh);
    CHECK(after.ringOffsets == before.ringOffsets);
    CHECK(after.ring == before.ring);
}

// ============================================================================
// G22: joins keep the file (the corpus arm)
// ============================================================================

namespace {

/// Each drawn triangle as the bytes of its three corners, in drawing order:
/// the position and every Halfedge layer at the corner.
std::vector<std::vector<u8>> drawnCorners(const Mesh& mesh) {
    std::vector<u32> triangles;
    std::vector<u32> faceOf;
    geom::TriangulateMesh(mesh, triangles, &faceOf);
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> p =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    std::vector<std::vector<u8>> out;
    for (std::size_t t = 0; t < faceOf.size(); ++t) {
        std::vector<u8>& key = out.emplace_back();
        for (u32 k = 0; k < 3; ++k) {
            const u32 vertex = triangles[3 * t + k];
            const u8* at = reinterpret_cast<const u8*>(&p[vertex]);
            key.insert(key.end(), at, at + sizeof(Vector3f));
            for (const HalfedgeId h : topology.fh(FaceId(faceOf[t]))) {
                if (topology.from(h).value() != vertex) {
                    continue;
                }
                for (const geom::AttrLayer& layer : mesh.attributes.layers()) {
                    if (layer.domain != Domain::Halfedge) {
                        continue;
                    }
                    const std::size_t stride = geom::AttrTypeSize(layer.type);
                    const u8* value = layer.data.data() + stride * h.index();
                    key.insert(key.end(), value, value + stride);
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool sameCorner(const Mesh& mesh, HalfedgeId a, HalfedgeId b) {
    for (const geom::AttrLayer& layer : mesh.attributes.layers()) {
        if (layer.domain != Domain::Halfedge) {
            continue;
        }
        const std::size_t stride = geom::AttrTypeSize(layer.type);
        if (std::memcmp(layer.data.data() + stride * a.index(), layer.data.data() + stride * b.index(),
                        stride) != 0) {
            return false;
        }
    }
    return true;
}

/// Greedy triangle pairs across edges that meet §3.2's delimits and whose
/// union row is valid, each joined keeping the lower-numbered face.
u32 joinTriangles(Mesh& mesh) {
    const Mesh& readable = mesh;
    const u32 edges = readable.topology().edgeCount();
    std::vector<u8> taken(readable.topology().faceCount(), 0);
    const std::span<const u32> smoothing =
        readable.attributes.get<const u32>(geom::names::kSmoothGroup, Domain::Face);
    const std::span<const Vector3f> p =
        readable.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    u32 joined = 0;
    for (u32 e = 0; e < edges; ++e) {
        const Topology& topology = readable.topology();
        if (topology.isDeleted(EdgeId(e)) || topology.isBoundary(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h0 = Topology::halfedge(EdgeId(e), 0);
        const HalfedgeId h1 = Topology::halfedge(EdgeId(e), 1);
        const FaceId fa = topology.face(h0);
        const FaceId fb = topology.face(h1);
        if (taken[fa.index()] != 0 || taken[fb.index()] != 0 || topology.valence(fa) != 3 ||
            topology.valence(fb) != 3) {
            continue;
        }
        const std::span<const u32> sections = readable.faceSections();
        if (sections[fa.index()] != sections[fb.index()] ||
            (!smoothing.empty() && smoothing[fa.index()] != smoothing[fb.index()])) {
            continue;
        }
        // One corner per vertex survives the join: both sides must agree.
        if (!sameCorner(readable, h0, topology.next(h1)) ||
            !sameCorner(readable, topology.next(h0), h1)) {
            continue;
        }
        const FaceId lower = fa.value() < fb.value() ? fa : fb;
        const FaceId higher = fa.value() < fb.value() ? fb : fa;
        std::vector<u32> row;
        for (const FaceId f : {lower, higher}) {
            for (const HalfedgeId h : topology.fh(f)) {
                row.push_back(topology.from(h).value());
            }
        }
        const std::vector<u32> loop{topology.from(topology.next(h0)).value(),
                                    topology.from(topology.prev(h0)).value(),
                                    topology.from(topology.next(h1)).value(),
                                    topology.from(topology.prev(h1)).value()};
        if (!geom::RowValid(loop, p, row)) {
            continue;
        }
        if (geom::DissolveEdge(mesh, EdgeId(e), lower)) {
            taken[fa.index()] = 1;
            taken[fb.index()] = 1;
            ++joined;
        }
    }
    return joined;
}

} // namespace

TEST_CASE("wem triangulation joins over corpus meshes keep the file",
          "[wem][geometry][triangulation][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 40);
    const MdxConverter converter;
    u32 meshes = 0;
    u32 joins = 0;
    u32 triangles = 0;
    std::vector<std::string> failing;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        mdx::Parser parser;
        const mdx::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        const Result<Document> document = converter.fromMdx(source);
        if (!document.ok() || document->models.empty()) {
            continue;
        }
        for (const Mesh& original : document->models[0].meshes) {
            Mesh mesh = original;
            if (!mesh.ensureConnectivity().ok()) {
                continue;
            }
            const std::vector<std::vector<u8>> before = drawnCorners(mesh);
            const u32 joined = joinTriangles(mesh);
            if (joined == 0) {
                continue;
            }
            ++meshes;
            joins += joined;
            triangles += static_cast<u32>(before.size());
            std::string broken;
            if (drawnCorners(mesh) != before) {
                broken = "the drawn triangles moved";
            } else if (storedAndNot(mesh).second != 0) {
                broken = "a joined face does not draw from its row";
            } else if (!errors(mesh).empty()) {
                broken = errors(mesh);
            }
            if (!broken.empty() && failing.size() < 10) {
                failing.push_back(test::pathText(files[i].filename()) + " '" + original.name +
                                  "': " + broken);
            }
        }
    }
    for (const std::string& line : failing) {
        std::cout << "  " << line << "\n";
    }
    std::cout << "joins corpus arm: " << meshes << " meshes, " << joins << " joins over "
              << triangles << " triangles\n";
    CHECK(meshes > 0u);
    CHECK(failing.empty());
}
