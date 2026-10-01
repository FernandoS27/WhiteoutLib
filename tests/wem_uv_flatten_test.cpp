// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G-U1: one flattener (EDIT_MODE_UV_DESIGN.md §6; EDIT_MODE_UV_PLAN.md §6).
/// LSCM keeps angles and not areas, so every claim here is about angles, about
/// what is held, and about what it refuses.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/uv/flatten.h>
#include <whiteout/models/wem/geometry/uv/islands.h>
#include <whiteout/models/wem/geometry/uv/layout.h>
#include <whiteout/models/wem/geometry/uv/seams.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include "wem_corpus_files.h"
#include "wem_geometry_ingest.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

using geom::Domain;
using geom::EdgeId;
using geom::FaceId;
using geom::HalfedgeId;
using geom::Topology;
using geom::VertexId;
namespace uv = geom::uv;

/// A sheet of `sx` by `sy` quads, its positions given by @p place and its UVs
/// starting as the sheet's own `(x, y)`.
template <class Place>
Mesh sheet(u32 sx, u32 sy, Place&& place) {
    const u32 ny = sy + 1;
    geom::FaceSet set;
    set.vertexCount = (sx + 1) * ny;
    std::vector<Vector3f> positions;
    std::vector<Vector2f> flat;
    for (u32 x = 0; x <= sx; ++x) {
        for (u32 y = 0; y < ny; ++y) {
            positions.push_back(place(x, y));
            flat.push_back(Vector2f{static_cast<f32>(x), static_cast<f32>(y)});
        }
    }
    const auto at = [&](u32 x, u32 y) { return x * ny + y; };
    for (u32 x = 0; x < sx; ++x) {
        for (u32 y = 0; y < sy; ++y) {
            const u32 corners[4] = {at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)};
            set.addFace(std::span<const u32>(corners, 4));
        }
    }
    Mesh mesh;
    mesh.setFaceSet(set);
    const std::span<Vector3f> out = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    std::copy(positions.begin(), positions.end(), out.begin());
    REQUIRE(mesh.ensureConnectivity().ok());
    mesh.faceSections();
    mesh.sections.emplace_back();
    const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
        geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    const Topology& topology = std::as_const(mesh).topology();
    for (u32 h = 0; h < topology.halfedgeCount(); ++h) {
        const HalfedgeId corner(h);
        if (topology.face(corner).valid()) {
            uvs[h] = flat[topology.from(corner).index()];
        }
    }
    mesh.attributes.getOrCreate<u8>(geom::names::uvPin(0), Domain::Halfedge, geom::AttrType::Bool);
    mesh.recomputeBounds();
    return mesh;
}

/// An open tube, `around` quads round and `rings` high, with no cut: one
/// island with two border loops, which no disc map lays without a fold.
Mesh tube(u32 around, u32 rings) {
    constexpr f32 kPi = 3.14159265358979323846f;
    geom::FaceSet set;
    set.vertexCount = around * (rings + 1);
    std::vector<Vector3f> positions;
    for (u32 y = 0; y <= rings; ++y) {
        for (u32 x = 0; x < around; ++x) {
            const f32 angle = 2.0f * kPi * static_cast<f32>(x) / static_cast<f32>(around);
            positions.push_back(Vector3f{std::cos(angle), std::sin(angle), static_cast<f32>(y) * 0.5f});
        }
    }
    const auto at = [&](u32 x, u32 y) { return y * around + x % around; };
    for (u32 y = 0; y < rings; ++y) {
        for (u32 x = 0; x < around; ++x) {
            const u32 corners[4] = {at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)};
            set.addFace(std::span<const u32>(corners, 4));
        }
    }
    Mesh mesh;
    mesh.setFaceSet(set);
    const std::span<Vector3f> out = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    std::copy(positions.begin(), positions.end(), out.begin());
    REQUIRE(mesh.ensureConnectivity().ok());
    mesh.faceSections();
    mesh.sections.emplace_back();
    mesh.attributes.getOrCreate<Vector2f>(geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    mesh.attributes.getOrCreate<u8>(geom::names::uvPin(0), Domain::Halfedge, geom::AttrType::Bool);
    mesh.recomputeBounds();
    return mesh;
}

/// Which way round @p island lies in set 0: its faces' vote.
int HandOf(const Mesh& mesh, const uv::UvIslands& islands, u32 island) {
    const std::span<const Vector2f> uvs = mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    const Topology& topology = mesh.topology();
    i64 votes = 0;
    for (const u32 face : islands.facesOf(island)) {
        f64 area = 0.0;
        for (const HalfedgeId h : topology.fh(FaceId(face))) {
            const Vector2f a = uvs[h.index()];
            const Vector2f b = uvs[topology.next(h).index()];
            area += static_cast<f64>(a.x) * b.y - static_cast<f64>(b.x) * a.y;
        }
        votes += area > 0.0 ? 1 : (area < 0.0 ? -1 : 0);
    }
    return votes >= 0 ? 1 : -1;
}

Mesh flatSheet(u32 sx, u32 sy) {
    return sheet(sx, sy, [](u32 x, u32 y) {
        return Vector3f{static_cast<f32>(x), static_cast<f32>(y), 0.0f};
    });
}

std::vector<Vector2f> uvBytes(const Mesh& mesh) {
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    return std::vector<Vector2f>(uvs.begin(), uvs.end());
}

/// The map is a similarity of the surface when every pair of wedges keeps the
/// same ratio of UV distance to world distance.
f32 similarityError(const Mesh& mesh, const uv::UvIslands& islands, u32 island) {
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    const std::vector<u32> wedges = uv::IslandWedges(islands, mesh, island);
    f32 lowest = 1e30f;
    f32 highest = 0.0f;
    for (std::size_t i = 0; i < wedges.size(); ++i) {
        const Vector3f pi = positions[islands.wedgeVertex[wedges[i]]];
        const Vector2f qi = uvs[islands.cornersOf(wedges[i])[0]];
        for (std::size_t j = i + 1; j < wedges.size(); ++j) {
            const Vector3f pj = positions[islands.wedgeVertex[wedges[j]]];
            const Vector2f qj = uvs[islands.cornersOf(wedges[j])[0]];
            const f32 world = (pj - pi).length();
            if (world <= 1e-5f) {
                continue;
            }
            const f32 plane =
                std::sqrt((qj.x - qi.x) * (qj.x - qi.x) + (qj.y - qi.y) * (qj.y - qi.y));
            const f32 ratio = plane / world;
            lowest = std::min(lowest, ratio);
            highest = std::max(highest, ratio);
        }
    }
    return highest > 0.0f ? (highest - lowest) / highest : 1.0f;
}


/// For a surface that really does lie flat, every *edge* keeps the same ratio
/// of UV length to world length. Over pairs it would not: a chord across a
/// curve is shorter than the way round it, and the unrolled sheet keeps the way
/// round.
f32 edgeSimilarityError(const Mesh& mesh, const uv::UvIslands& islands, u32 island) {
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    const Topology& topology = mesh.topology();
    f32 lowest = 1e30f;
    f32 highest = 0.0f;
    for (const u32 face : islands.facesOf(island)) {
        for (const HalfedgeId h : topology.fh(FaceId(face))) {
            const HalfedgeId next = topology.next(h);
            const f32 world =
                (positions[topology.from(next).index()] - positions[topology.from(h).index()])
                    .length();
            if (world <= 1e-5f) {
                continue;
            }
            const Vector2f a = uvs[h.index()];
            const Vector2f b = uvs[next.index()];
            const f32 plane = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
            const f32 ratio = plane / world;
            lowest = std::min(lowest, ratio);
            highest = std::max(highest, ratio);
        }
    }
    return highest > 0.0f ? (highest - lowest) / highest : 1.0f;
}

} // namespace

TEST_CASE("UV flatten: a flat sheet keeps the map it has", "[wem][uv][flatten]") {
    Mesh mesh = flatSheet(4, 3);
    const std::vector<Vector2f> before = uvBytes(mesh);
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 1u);

    const uv::FlattenResult result = uv::Lscm(mesh, islands, 0, 0);
    REQUIRE(result.ok());
    CHECK(result.flipped == 0u);
    CHECK(result.degenerate == 0u);

    // A plane is already conformal, and the two automatic pins hold it where it
    // was: the answer is the map it came with.
    const std::vector<Vector2f> after = uvBytes(mesh);
    REQUIRE(before.size() == after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK(std::abs(before[i].x - after[i].x) < 1e-4f);
        CHECK(std::abs(before[i].y - after[i].y) < 1e-4f);
    }
    CHECK(similarityError(mesh, islands, 0) < 1e-4f);
}

TEST_CASE("UV flatten: a scrambled sheet comes back a similarity", "[wem][uv][flatten]") {
    Mesh mesh = flatSheet(4, 3);
    {
        // Thrown about, so the solve has somewhere to come from.
        const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
            geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
        for (std::size_t i = 0; i < uvs.size(); ++i) {
            uvs[i] = Vector2f{static_cast<f32>((i * 37) % 11) * 0.3f,
                              static_cast<f32>((i * 17) % 7) * 0.4f};
        }
    }
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const uv::FlattenResult result = uv::Lscm(mesh, islands, 0, 0);
    REQUIRE(result.ok());
    CHECK(result.flipped == 0u);
    // It really solved: the residual is down, not the iteration cap reached.
    CHECK(result.residual < 1e-4f);
    CHECK(result.iterations > 0u);
    CHECK(similarityError(mesh, islands, 0) < 1e-3f);
}

TEST_CASE("UV flatten: a solve cut short at its cap says it did not settle", "[wem][uv][flatten]") {
    // What the workspace's notice names (EDIT_MODE_UV_AUDIT.md U6.2): the
    // residual it reports is the one it stops on, so the two agree.
    // Scrambled by vertex, so it stays one island of 169 wedges.
    Mesh mesh = flatSheet(12, 12);
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    const Topology& topology = std::as_const(mesh).topology();
    for (u32 h = 0; h < topology.halfedgeCount(); ++h) {
        const u32 v = topology.from(HalfedgeId(h)).value();
        uvs[h] = Vector2f{static_cast<f32>((v * 37) % 11) * 0.3f, static_cast<f32>((v * 17) % 7) * 0.4f};
    }
    Mesh full = mesh;
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 1u);
    uv::LscmOptions shortOptions;
    shortOptions.iterationsPerUnknown = 0; // the floor of 32 alone
    const uv::FlattenResult cut = uv::Lscm(mesh, islands, 0, 0, shortOptions);
    REQUIRE(cut.ok());
    CHECK(cut.iterations == 32u);
    CHECK(cut.capped);
    CHECK(cut.residual > shortOptions.tolerance);
    const uv::FlattenResult settled = uv::Lscm(full, islands, 0, 0);
    REQUIRE(settled.ok());
    CHECK_FALSE(settled.capped);
    CHECK(settled.residual <= uv::LscmOptions{}.tolerance);
    CHECK(settled.iterations > 32u);
    // Solved again from its own answer: nothing left to shed, and no cap.
    const uv::FlattenResult again = uv::Lscm(full, islands, 0, 0);
    REQUIRE(again.ok());
    CHECK_FALSE(again.capped);
}

TEST_CASE("UV flatten: a bent sheet flattens to a rectangle", "[wem][uv][flatten]") {
    // A cylinder's wall, cut open: developable, so a conformal map of it is
    // isometric up to scale and the answer is the rectangle it was rolled from.
    constexpr u32 kAround = 12;
    constexpr f32 kRadius = 2.0f;
    constexpr f32 kPi = 3.14159265358979323846f;
    Mesh mesh = sheet(kAround, 3, [](u32 x, u32 y) {
        const f32 angle = (static_cast<f32>(x) / static_cast<f32>(kAround)) * 1.5f * kPi;
        return Vector3f{kRadius * std::cos(angle), kRadius * std::sin(angle),
                        static_cast<f32>(y)};
    });
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 1u);
    const uv::FlattenResult result = uv::Lscm(mesh, islands, 0, 0);
    REQUIRE(result.ok());
    CHECK(result.flipped == 0u);
    // Every edge keeps its length ratio: the wall really does lie flat.
    CHECK(edgeSimilarityError(mesh, islands, 0) < 5e-3f);
}

TEST_CASE("UV minimum stretch: a cap of a sphere comes out less stretched than LSCM makes it",
          "[wem][uv][flatten][stretch]") {
    // EDIT_MODE_UV_AUDIT.md U6. Not developable, so a conformal map keeps the
    // angles and lets the area go -- the rim grows -- where SLIM trades a
    // little of each; and neither turns anything over.
    constexpr f32 kPi = 3.14159265358979323846f;
    const auto cap = [&](u32 x, u32 y) {
        const f32 a = (static_cast<f32>(x) / 10.0f - 0.5f) * 0.6f * kPi;
        const f32 b = (static_cast<f32>(y) / 10.0f - 0.5f) * 0.6f * kPi;
        return Vector3f{std::cos(b) * std::sin(a), std::sin(b), std::cos(a) * std::cos(b)};
    };
    Mesh conformal = sheet(10, 10, cap);
    Mesh stretch = conformal;
    const uv::UvIslands islands = uv::BuildUvIslands(conformal, 0);
    REQUIRE(islands.count == 1u);
    const uv::FlattenResult a = uv::Lscm(conformal, islands, 0, 0);
    const uv::FlattenResult b = uv::MinimumStretch(stretch, islands, 0, 0);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.flipped == 0u);
    CHECK(b.flipped == 0u);
    // The same least-squares solve, then SLIM's rounds on top of it.
    CHECK(b.iterations == a.iterations);
    CHECK(a.rounds == 0u);
    CHECK(b.rounds > 0u);
    const auto mean = [&](const Mesh& mesh) {
        const std::vector<f32> s = uv::FaceStretch(mesh, islands, 0);
        f64 sum = 0.0;
        for (const f32 v : s) {
            sum += v;
        }
        return sum / static_cast<f64>(s.size());
    };
    const auto worst = [&](const Mesh& mesh) {
        const std::vector<f32> s = uv::FaceStretch(mesh, islands, 0);
        return *std::max_element(s.begin(), s.end());
    };
    INFO("mean " << mean(conformal) << " -> " << mean(stretch) << ", worst " << worst(conformal) << " -> "
                 << worst(stretch));
    CHECK(mean(stretch) < mean(conformal));
    CHECK(worst(stretch) < worst(conformal));
    // Its size and place are LSCM's: the landing a caller chose survives.
    const uv::UvBounds ba = uv::BoundsOf(conformal, islands, 0, 0);
    const uv::UvBounds bb = uv::BoundsOf(stretch, islands, 0, 0);
    CHECK(std::abs((ba.low.x + ba.high.x) - (bb.low.x + bb.high.x)) < 0.05f * ba.width());
    CHECK(std::abs((ba.low.y + ba.high.y) - (bb.low.y + bb.high.y)) < 0.05f * ba.height());
    CHECK(std::abs(bb.width() * bb.height() / (ba.width() * ba.height()) - 1.0f) < 0.15f);
}

TEST_CASE("UV minimum stretch: a tube LSCM folds starts from Tutte and folds nothing",
          "[wem][uv][flatten][stretch]") {
    // Two border loops, the footman's three folds (EDIT_MODE_UV_AUDIT.md §7):
    // the outer loop goes on a circle and the other is solved as inside.
    Mesh conformal = tube(12, 16);
    const uv::UvIslands islands = uv::BuildUvIslands(conformal, 0);
    REQUIRE(islands.count == 1u);
    REQUIRE(islands.loops[0] == 2u);
    Mesh stretch = conformal;
    const uv::FlattenResult a = uv::Lscm(conformal, islands, 0, 0);
    const uv::FlattenResult b = uv::MinimumStretch(stretch, islands, 0, 0);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    // The fixture is only worth its name while LSCM folds it.
    REQUIRE(a.flipped > 0u);
    CHECK(b.flipped == 0u);
    CHECK(b.rounds > 0u);
    // And the way round LSCM laid it.
    CHECK(HandOf(stretch, islands, 0) == HandOf(conformal, islands, 0));
}

TEST_CASE("UV minimum stretch: a mirrored map stays mirrored through Tutte", "[wem][uv][flatten][stretch]") {
    // Refitted over a mirrored map, LSCM lays the tube mirrored; the circle
    // Tutte starts from runs one way, and must be turned to match.
    Mesh start = tube(12, 16);
    const uv::UvIslands islands = uv::BuildUvIslands(start, 0);
    REQUIRE(uv::Lscm(start, islands, 0, 0).ok());
    for (Vector2f& p : start.attributes.getOrCreate<Vector2f>(geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2)) {
        p.x = -p.x;
    }
    Mesh conformal = start;
    Mesh stretch = start;
    const uv::FlattenResult a = uv::Lscm(conformal, islands, 0, 0);
    const uv::FlattenResult b = uv::MinimumStretch(stretch, islands, 0, 0);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(a.flipped > 0u);
    CHECK(b.flipped == 0u);
    CHECK(HandOf(conformal, islands, 0) == HandOf(start, islands, 0));
    CHECK(HandOf(stretch, islands, 0) == HandOf(conformal, islands, 0));
}

TEST_CASE("UV minimum stretch: a fold pins hold keeps LSCM's map to the bit", "[wem][uv][flatten][stretch]") {
    // With pins of the user's there is no Tutte start (the circle would move
    // them), and a start that folds is no start for SLIM: Conformal's answer.
    Mesh conformal = tube(12, 16);
    const uv::UvIslands islands = uv::BuildUvIslands(conformal, 0);
    const std::vector<u32> wedges = uv::IslandWedges(islands, conformal, 0);
    REQUIRE(wedges.size() > 2);
    const std::span<u8> pins =
        conformal.attributes.getOrCreate<u8>(geom::names::uvPin(0), Domain::Halfedge, geom::AttrType::Bool);
    const std::span<Vector2f> uvs =
        conformal.attributes.getOrCreate<Vector2f>(geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    for (const u32 corner : islands.cornersOf(wedges.front())) {
        pins[corner] = 1;
    }
    for (const u32 corner : islands.cornersOf(wedges[wedges.size() / 2])) {
        pins[corner] = 1;
        uvs[corner] = Vector2f{1.0f, 0.0f};
    }
    Mesh stretch = conformal;
    const uv::FlattenResult a = uv::Lscm(conformal, islands, 0, 0);
    const uv::FlattenResult b = uv::MinimumStretch(stretch, islands, 0, 0);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(a.flipped > 0u);
    CHECK(b.flipped == a.flipped);
    CHECK(b.rounds == 0u);
    CHECK(uvBytes(stretch) == uvBytes(conformal));
}

TEST_CASE("UV minimum stretch: a pin is held to the bit", "[wem][uv][flatten][stretch]") {
    constexpr f32 kPi = 3.14159265358979323846f;
    Mesh mesh = sheet(6, 6, [&](u32 x, u32 y) {
        const f32 a = (static_cast<f32>(x) / 6.0f - 0.5f) * 0.5f * kPi;
        return Vector3f{std::sin(a), static_cast<f32>(y) * 0.3f, std::cos(a) + 0.1f * static_cast<f32>(y * y)};
    });
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const std::span<u8> pins = mesh.attributes.getOrCreate<u8>(geom::names::uvPin(0), Domain::Halfedge, geom::AttrType::Bool);
    // A corner, not a border halfedge: the first one with a face.
    u32 first = 0;
    while (!mesh.topology().face(HalfedgeId(first)).valid()) {
        ++first;
    }
    const u32 wedge = islands.wedgeOf(HalfedgeId(first));
    std::vector<u32> corners(islands.cornersOf(wedge).begin(), islands.cornersOf(wedge).end());
    for (const u32 c : corners) {
        pins[c] = 1;
    }
    const std::vector<Vector2f> before = uvBytes(mesh);
    REQUIRE(uv::MinimumStretch(mesh, islands, 0, 0).ok());
    const std::vector<Vector2f> after = uvBytes(mesh);
    for (const u32 c : corners) {
        CHECK(std::memcmp(&before[c], &after[c], sizeof(Vector2f)) == 0);
    }
}

TEST_CASE("UV flatten: a pin is held to the bit", "[wem][uv][flatten]") {
    Mesh mesh = flatSheet(3, 3);
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const std::vector<u32> wedges = uv::IslandWedges(islands, mesh, 0);
    REQUIRE(wedges.size() > 4);

    const Vector2f placed[3] = {{-3.0f, 7.0f}, {5.5f, -1.25f}, {0.75f, 0.5f}};
    {
        const std::span<u8> pins = mesh.attributes.getOrCreate<u8>(
            geom::names::uvPin(0), Domain::Halfedge, geom::AttrType::Bool);
        const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
            geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
        for (u32 k = 0; k < 3; ++k) {
            for (const u32 corner : islands.cornersOf(wedges[k])) {
                pins[corner] = 1;
                uvs[corner] = placed[k];
            }
        }
    }
    const uv::FlattenResult result = uv::Lscm(mesh, islands, 0, 0);
    REQUIRE(result.ok());

    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    for (u32 k = 0; k < 3; ++k) {
        for (const u32 corner : islands.cornersOf(wedges[k])) {
            CHECK(std::memcmp(&uvs[corner], &placed[k], sizeof(Vector2f)) == 0);
        }
    }
}

TEST_CASE("UV flatten: a closed island is refused, not solved", "[wem][uv][flatten]") {
    geom::PrimitiveParams params;
    Mesh box = geom::MakeBox(params);
    REQUIRE(box.ensureConnectivity().ok());
    {
        const std::span<Vector2f> uvs = box.attributes.getOrCreate<Vector2f>(
            geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
        std::fill(uvs.begin(), uvs.end(), Vector2f{0.0f, 0.0f});
    }
    const uv::UvIslands islands = uv::BuildUvIslands(box, 0);
    REQUIRE(islands.count == 1u);
    REQUIRE(islands.closed[0] == 1u);
    const std::vector<Vector2f> before = uvBytes(box);
    const uv::FlattenResult result = uv::Lscm(box, islands, 0, 0);
    CHECK(result.refusal == uv::FlattenResult::Refusal::Closed);
    // Refused means untouched.
    CHECK(uvBytes(box) == before);
}

TEST_CASE("UV flatten: a triangle with no area is counted and skipped",
          "[wem][uv][flatten]") {
    Mesh mesh = flatSheet(3, 3);
    {
        // Two vertices of one quad brought together: the quad's first triangle
        // has no area left.
        const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
            geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
        positions[1] = positions[0];
    }
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const uv::FlattenResult result = uv::Lscm(mesh, islands, 0, 0);
    CHECK(result.ok());
    CHECK(result.degenerate >= 1u);
}

TEST_CASE("UV flatten: two runs are the same bytes", "[wem][uv][flatten]") {
    const auto solve = []() {
        Mesh mesh = flatSheet(5, 4);
        {
            const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
                geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
            for (std::size_t i = 0; i < uvs.size(); ++i) {
                uvs[i] = Vector2f{static_cast<f32>((i * 13) % 9) * 0.2f,
                                  static_cast<f32>((i * 29) % 5) * 0.6f};
            }
        }
        const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
        uv::Lscm(mesh, islands, 0, 0);
        return uvBytes(mesh);
    };
    const std::vector<Vector2f> first = solve();
    const std::vector<Vector2f> second = solve();
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(std::memcmp(&first[i], &second[i], sizeof(Vector2f)) == 0);
    }
}

TEST_CASE("UV flatten: the automatic pins are the two farthest apart",
          "[wem][uv][flatten]") {
    // A long thin strip: the pair the solve holds is its two ends, and holding
    // them is what fixes place, turn and scale.
    Mesh mesh = flatSheet(8, 1);
    {
        const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
            geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
        std::fill(uvs.begin(), uvs.end(), Vector2f{0.0f, 0.0f});
    }
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    uv::LscmOptions options;
    options.holdCurrent = false;
    const uv::FlattenResult result = uv::Lscm(mesh, islands, 0, 0, options);
    REQUIRE(result.ok());
    // Nothing to hold it to but the unit segment, so the strip comes out about
    // a unit long and a quarter of that high.
    const uv::UvBounds bounds = uv::BoundsOf(mesh, islands, 0, 0);
    CHECK(bounds.width() > 0.5f);
    CHECK(bounds.height() > 0.0f);
    CHECK(bounds.height() < bounds.width());
}

TEST_CASE("UV stretch: a squashed sheet reads above one", "[wem][uv][flatten]") {
    Mesh mesh = flatSheet(4, 1);
    {
        // The first column squeezed in UV: it has to cover the same surface
        // with less room, which is what stretch means.
        const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
            geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
        for (Vector2f& value : uvs) {
            if (value.x <= 1.0f) {
                value.x *= 0.2f;
            }
        }
    }
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const std::vector<f32> stretch = uv::FaceStretch(mesh, islands, 0);
    REQUIRE(stretch.size() == mesh.faceCount());
    CHECK(stretch[0] > 1.3f);
    CHECK(stretch[3] < 1.1f);
    // An even map reads one everywhere.
    Mesh even = flatSheet(4, 1);
    const uv::UvIslands evenIslands = uv::BuildUvIslands(even, 0);
    for (const f32 value : uv::FaceStretch(even, evenIslands, 0)) {
        CHECK(std::abs(value - 1.0f) < 1e-3f);
    }
}

TEST_CASE("UV layout: density, the median and the strip", "[wem][uv][layout]") {
    Mesh mesh = flatSheet(4, 4);
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    // The sheet's UVs are its world units, so a 1024 map is 1024 texels per
    // unit.
    CHECK(std::abs(uv::TexelDensity(mesh, islands, 0, 0, 1024) - 1024.0f) < 1.0f);
    CHECK(std::abs(uv::MedianDensity(mesh, islands, 0, 1024, false) - 1024.0f) < 1.0f);
    // Locked only: the sheet is locked (no `uvFree0` at all), so it counts.
    CHECK(uv::MedianDensity(mesh, islands, 0, 1024, true) > 0.0f);

    uv::PlaceInStrip(mesh, islands, 0, 0, 512.0f, 1024);
    const uv::UvBounds bounds = uv::BoundsOf(mesh, islands, 0, 0);
    CHECK(bounds.low.x >= 1.0f);
    CHECK(bounds.low.y >= 0.0f);
    // Half the density is half the size.
    CHECK(std::abs(bounds.width() - 2.0f) < 1e-3f);
    CHECK(std::abs(uv::TexelDensity(mesh, islands, 0, 0, 1024) - 512.0f) < 1.0f);
}

namespace {

/// A mesh of quads over a grid of vertices, with the cells @p keep says to
/// keep: what an L-shaped island is made of.
template <class Place, class Keep>
Mesh grid(u32 sx, u32 sy, Place&& place, Keep&& keep) {
    const u32 ny = sy + 1;
    const auto at = [&](u32 x, u32 y) { return x * ny + y; };
    geom::FaceSet set;
    set.vertexCount = (sx + 1) * ny;
    for (u32 x = 0; x < sx; ++x) {
        for (u32 y = 0; y < sy; ++y) {
            if (!keep(x, y)) {
                continue;
            }
            const u32 corners[4] = {at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)};
            set.addFace(std::span<const u32>(corners, 4));
        }
    }
    Mesh mesh;
    mesh.setFaceSet(set);
    const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    for (u32 x = 0; x <= sx; ++x) {
        for (u32 y = 0; y < ny; ++y) {
            positions[at(x, y)] = place(x, y);
        }
    }
    REQUIRE(mesh.ensureConnectivity().ok());
    mesh.faceSections();
    mesh.sections.emplace_back();
    const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
        geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    const Topology& topology = std::as_const(mesh).topology();
    for (u32 h = 0; h < topology.halfedgeCount(); ++h) {
        const HalfedgeId corner(h);
        if (!topology.face(corner).valid()) {
            continue;
        }
        const u32 v = topology.from(corner).index();
        uvs[h] = Vector2f{static_cast<f32>(v / ny), static_cast<f32>(v % ny)};
    }
    mesh.attributes.getOrCreate<u8>(geom::names::uvPin(0), Domain::Halfedge, geom::AttrType::Bool);
    mesh.recomputeBounds();
    return mesh;
}

f32 meanStretch(const Mesh& mesh, const uv::UvIslands& islands) {
    const std::vector<f32> stretch = uv::FaceStretch(mesh, islands, 0);
    f32 sum = 0.0f;
    u32 count = 0;
    for (const f32 value : stretch) {
        if (value > 0.0f) {
            sum += value;
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<f32>(count) : 0.0f;
}

HalfedgeId cornerFrom(const Mesh& mesh, u32 a, u32 b) {
    const HalfedgeId h = mesh.topology().findHalfedge(VertexId(a), VertexId(b));
    REQUIRE(h.valid());
    return mesh.topology().face(h).valid() ? h : Topology::opposite(h);
}

} // namespace

TEST_CASE("UV relax: stretch falls and nothing turns over", "[wem][uv][relax]") {
    Mesh mesh = flatSheet(5, 5);
    {
        // The inside pulled about, the boundary left where it was: a patch that
        // is badly laid out inside a frame that is not.
        const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
            geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
        const Topology& topology = std::as_const(mesh).topology();
        for (u32 h = 0; h < topology.halfedgeCount(); ++h) {
            const HalfedgeId corner(h);
            if (!topology.face(corner).valid()) {
                continue;
            }
            const u32 v = topology.from(corner).index();
            const u32 x = v / 6;
            const u32 y = v % 6;
            if (x > 0 && x < 5 && y > 0 && y < 5) {
                uvs[h].x += (x % 2 == 0 ? 0.35f : -0.35f);
                uvs[h].y += (y % 2 == 0 ? -0.3f : 0.3f);
            }
        }
    }
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const std::vector<u32> wedges = uv::IslandWedges(islands, mesh, 0);

    uv::RelaxOptions options;
    options.passes = 1;
    f32 previous = meanStretch(mesh, islands);
    REQUIRE(previous > 1.05f);
    for (u32 pass = 0; pass < 6; ++pass) {
        const uv::FlattenResult result =
            uv::Relax(mesh, islands, std::span<const u32>(wedges.data(), wedges.size()), 0,
                      options);
        REQUIRE(result.ok());
        CHECK(result.flipped == 0u);
        const f32 now = meanStretch(mesh, islands);
        CHECK(now <= previous);
        previous = now;
    }
    // And it really went somewhere: an even map reads one.
    CHECK(previous < 1.2f);
}

TEST_CASE("UV relax: the boundary stands unless it is asked for", "[wem][uv][relax]") {
    Mesh mesh = flatSheet(4, 4);
    {
        const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
            geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
        for (Vector2f& value : uvs) {
            value.x *= 1.6f;
        }
    }
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const std::vector<u32> wedges = uv::IslandWedges(islands, mesh, 0);
    const uv::UvBounds before = uv::BoundsOf(mesh, islands, 0, 0);
    uv::Relax(mesh, islands, std::span<const u32>(wedges.data(), wedges.size()), 0, {});
    const uv::UvBounds after = uv::BoundsOf(mesh, islands, 0, 0);
    CHECK(std::abs(before.width() - after.width()) < 1e-4f);
    CHECK(std::abs(before.height() - after.height()) < 1e-4f);
}

TEST_CASE("UV straighten: a wavy run comes out a line", "[wem][uv][straighten]") {
    Mesh mesh = flatSheet(6, 2);
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const auto at = [](u32 x, u32 y) { return x * 3 + y; };
    {
        // The middle row made wavy in the map alone; the surface is still flat.
        const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
            geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
        const Topology& topology = std::as_const(mesh).topology();
        for (u32 h = 0; h < topology.halfedgeCount(); ++h) {
            const HalfedgeId corner(h);
            if (!topology.face(corner).valid()) {
                continue;
            }
            const u32 v = topology.from(corner).index();
            if (v % 3 == 1) {
                uvs[h].y += (v / 3) % 2 == 0 ? 0.3f : -0.3f;
            }
        }
    }
    std::vector<HalfedgeId> run;
    for (u32 x = 0; x < 6; ++x) {
        run.push_back(cornerFrom(mesh, at(x, 1), at(x + 1, 1)));
    }
    const uv::FlattenResult result =
        uv::Straighten(mesh, islands, std::span<const HalfedgeId>(run.data(), run.size()), 0);
    REQUIRE(result.ok());

    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    // Every wedge of the run is on one horizontal line -- the snap, because the
    // run was within ten degrees of one.
    f32 line = uvs[islands.cornersOf(islands.wedgeOf(run[0]))[0]].y;
    for (const HalfedgeId h : run) {
        for (const HalfedgeId end : {h, mesh.topology().next(h)}) {
            const u32 wedge = islands.wedgeOf(end);
            REQUIRE(wedge != geom::kInvalidId);
            CHECK(std::abs(uvs[islands.cornersOf(wedge)[0]].y - line) < 1e-4f);
        }
    }
}

TEST_CASE("UV rectangle: a bent strip lies out as its own arc lengths, where it was",
          "[wem][uv][rectangle]") {
    // Bent across the length, so the long sides are longer than the strip is
    // wide and the answer is not a square.
    Mesh mesh = grid(
        6, 1,
        [](u32 x, u32 y) {
            return Vector3f{static_cast<f32>(x), static_cast<f32>(y),
                            0.35f * static_cast<f32>((x % 2 == 0) ? 1 : 0)};
        },
        [](u32, u32) { return true; });
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 1u);

    const uv::RectangleResult result = uv::Rectangle(mesh, islands, 0, 0);
    REQUIRE(result.corners == 4u);
    REQUIRE(result.solve.ok());

    const uv::UvBounds bounds = uv::BoundsOf(mesh, islands, 0, 0);
    // Which way round it lands is the boundary loop's business, so the claim is
    // about the two sides and not about which is x: their ratio is the zigzag's
    // arc length to the strip's width. Then it is fitted back over the map it
    // had, [0, 6] x [0, 1]: that area, that centre, as any re-solve lands.
    const f32 shorter = std::min(bounds.width(), bounds.height());
    const f32 longer = std::max(bounds.width(), bounds.height());
    CHECK(std::abs(longer / shorter - 6.0f * std::sqrt(1.0f + 0.35f * 0.35f)) < 1e-2f);
    CHECK(std::abs(longer * shorter - 6.0f) < 1e-2f);
    CHECK(std::abs((bounds.low.x + bounds.high.x) * 0.5f - 3.0f) < 1e-3f);
    CHECK(std::abs((bounds.low.y + bounds.high.y) * 0.5f - 0.5f) < 1e-3f);

    // And the boundary really is the rectangle: every boundary wedge is on one
    // of its four sides.
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    for (const HalfedgeId h : islands.boundaryOf(0)) {
        const Vector2f value = uvs[islands.cornersOf(islands.wedgeOf(h))[0]];
        const bool onSide = std::abs(value.x - bounds.low.x) < 1e-4f ||
                            std::abs(value.x - bounds.high.x) < 1e-4f ||
                            std::abs(value.y - bounds.low.y) < 1e-4f ||
                            std::abs(value.y - bounds.high.y) < 1e-4f;
        CHECK(onSide);
    }
}

TEST_CASE("UV rectangle: six corners are refused with six", "[wem][uv][rectangle]") {
    // An L: the cell at the far corner left out, so the boundary turns six
    // times and no rectangle is the answer.
    Mesh mesh = grid(
        2, 2,
        [](u32 x, u32 y) {
            return Vector3f{static_cast<f32>(x), static_cast<f32>(y), 0.0f};
        },
        [](u32 x, u32 y) { return !(x == 1 && y == 1); });
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 1u);
    const uv::RectangleResult result = uv::Rectangle(mesh, islands, 0, 0);
    CHECK(result.corners == 6u);
    CHECK_FALSE(result.ok());
    CHECK(result.solve.refusal == uv::FlattenResult::Refusal::TooFewPins);
}

TEST_CASE("UV box: a cube projects into six squares with nothing stretched",
          "[wem][uv][flatten]") {
    geom::PrimitiveParams params;
    params.size = Vector3f{0.5f, 0.5f, 0.5f};
    Mesh mesh = geom::MakeBox(params);
    // Into a set with no map, so nothing of the box's own unwrap is read.
    uv::EnsureUvSet(mesh, 1);
    std::vector<FaceId> faces;
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        faces.push_back(FaceId(f));
    }
    CHECK(uv::Project(mesh, faces, 1, uv::ProjectShape::Box, uv::ProjectFrame{}).ok());
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 1);
    REQUIRE(islands.count == 6u);
    const std::vector<f32> stretch = uv::FaceStretch(mesh, islands, 1);
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(1), Domain::Halfedge);
    for (u32 island = 0; island < islands.count; ++island) {
        const uv::UvBounds bounds = uv::BoundsOf(mesh, islands, island, 1);
        CHECK(std::abs(bounds.width() - 1.0f) < 1e-5f);
        CHECK(std::abs(bounds.height() - 1.0f) < 1e-5f);
        for (const u32 f : islands.facesOf(island)) {
            CHECK(std::abs(stretch[f] - 1.0f) < 1e-4f);
            // Seen from outside, none is mirrored: with v down the image, as a
            // texture reads, a face wound counter-clockwise from outside has a
            // negative shoelace in (u, v).
            f32 area = 0.0f;
            std::vector<Vector2f> ring;
            for (const HalfedgeId h : mesh.topology().fh(FaceId(f))) {
                ring.push_back(uvs[h.index()]);
            }
            for (std::size_t i = 0; i < ring.size(); ++i) {
                const Vector2f& a = ring[i];
                const Vector2f& b = ring[(i + 1) % ring.size()];
                area += a.x * b.y - b.x * a.y;
            }
            CHECK(area < 0.0f);
        }
    }
}

namespace {

/// Newell's normal of face @p f.
Vector3f faceNormal(const Mesh& mesh, u32 f) {
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    Vector3f n{0.0f, 0.0f, 0.0f};
    for (const HalfedgeId h : mesh.topology().fh(FaceId(f))) {
        n = n + cross(positions[mesh.topology().from(h).index()],
                      positions[mesh.topology().to(h).index()]);
    }
    return n;
}

/// Twice face @p f's signed area in set @p set.
f32 shoelace(const Mesh& mesh, u32 f, u32 set) {
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(set), Domain::Halfedge);
    std::vector<Vector2f> ring;
    for (const HalfedgeId h : mesh.topology().fh(FaceId(f))) {
        ring.push_back(uvs[h.index()]);
    }
    f32 area = 0.0f;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        area += ring[i].x * ring[(i + 1) % ring.size()].y - ring[(i + 1) % ring.size()].x * ring[i].y;
    }
    return area;
}

/// Face @p f's corners' UVs against @p along: the v of its highest corner
/// and of its lowest, so "up is at the image's top" reads `top < bottom`.
void vAlong(const Mesh& mesh, u32 f, u32 set, const Vector3f& along, f32& top, f32& bottom) {
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(set), Domain::Halfedge);
    f32 high = -1e30f;
    f32 low = 1e30f;
    for (const HalfedgeId h : mesh.topology().fh(FaceId(f))) {
        const f32 at = positions[mesh.topology().from(h).index()].dot(along);
        if (at > high) {
            high = at;
            top = uvs[h.index()].y;
        }
        if (at < low) {
            low = at;
            bottom = uvs[h.index()].y;
        }
    }
}

const Vector3f kUp{0.0f, 0.0f, 1.0f};
const Vector3f kFront{1.0f, 0.0f, 0.0f};

} // namespace

TEST_CASE("UV project: Box's sides read upright and unmirrored from outside",
          "[wem][uv][flatten][project]") {
    geom::PrimitiveParams params;
    params.size = Vector3f{0.5f, 0.5f, 0.5f};
    Mesh mesh = geom::MakeBox(params);
    uv::EnsureUvSet(mesh, 1);
    std::vector<FaceId> faces;
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        faces.push_back(FaceId(f));
    }
    // The model's own axes, looked at from its front.
    const uv::ProjectFrame frame = uv::UprightFrame(Vector3f{0, 0, 0}, kFront * -1.0f, kUp, kFront);
    REQUIRE(uv::Project(mesh, faces, 1, uv::ProjectShape::Box, frame).ok());
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        const Vector3f n = faceNormal(mesh, f);
        INFO("face " << f << " normal " << n.x << " " << n.y << " " << n.z);
        CHECK(shoelace(mesh, f, 1) < 0.0f);
        f32 top = 0.0f;
        f32 bottom = 0.0f;
        // A side's up is the world's; a top's and a bottom's is the front.
        vAlong(mesh, f, 1, std::abs(n.z) > 0.5f * n.length() ? kFront : kUp, top, bottom);
        CHECK(top < bottom);
    }
}

TEST_CASE("UV project: a planar map reads as a camera outside sees it", "[wem][uv][flatten][project]") {
    // A wall facing the front: +X, one unit wide along y and two high.
    Mesh mesh = sheet(1, 2, [](u32 x, u32 y) {
        return Vector3f{0.0f, static_cast<f32>(x) - 0.5f, static_cast<f32>(y)};
    });
    uv::EnsureUvSet(mesh, 1);
    const Vector3f n = faceNormal(mesh, 0);
    // Seen from outside whichever way the sheet was wound.
    const Vector3f look = n.x > 0.0f ? kFront * -1.0f : kFront;
    const uv::ProjectFrame frame = uv::UprightFrame(Vector3f{0, 0, 0}, look, kUp, kFront);
    CHECK(std::abs(frame.axisV.z + 1.0f) < 1e-6f);
    std::vector<FaceId> faces = {FaceId(0), FaceId(1)};
    REQUIRE(uv::Project(mesh, faces, 1, uv::ProjectShape::Planar, frame).ok());
    for (const FaceId f : faces) {
        f32 top = 0.0f;
        f32 bottom = 0.0f;
        vAlong(mesh, f.value(), 1, kUp, top, bottom);
        CHECK(top < bottom);
        // Unmirrored, however the face is wound, since it is looked at from
        // the side its winding faces.
        CHECK(shoelace(mesh, f.value(), 1) < 0.0f);
    }
    // A top view has the front at the image's top.
    const uv::ProjectFrame down = uv::UprightFrame(Vector3f{0, 0, 0}, kUp * -1.0f, kUp, kFront);
    CHECK(std::abs(down.axisV.x + 1.0f) < 1e-6f);
    CHECK(std::abs(cross(down.axisU, down.axisV).dot(down.axisN) - 1.0f) < 1e-5f);
    // World units per tile.
    uv::ProjectFrame tiled = frame;
    tiled.extent = 2.0f;
    Mesh twice = mesh;
    REQUIRE(uv::Project(twice, faces, 1, uv::ProjectShape::Planar, tiled).ok());
    const std::span<const Vector2f> a = mesh.attributes.get<const Vector2f>(geom::names::uv(1), Domain::Halfedge);
    const std::span<const Vector2f> b = twice.attributes.get<const Vector2f>(geom::names::uv(1), Domain::Halfedge);
    for (const FaceId f : faces) {
        for (const HalfedgeId h : mesh.topology().fh(f)) {
            CHECK(std::abs(b[h.index()].x - a[h.index()].x * 0.5f) < 1e-6f);
            CHECK(std::abs(b[h.index()].y - a[h.index()].y * 0.5f) < 1e-6f);
        }
    }
}

TEST_CASE("UV project: a cylinder runs down its axis and wraps at the back",
          "[wem][uv][flatten][project]") {
    geom::PrimitiveParams params;
    params.size = Vector3f{1.0f, 1.0f, 2.0f};
    params.sides = 16;
    Mesh mesh = geom::MakeCylinder(params);
    uv::EnsureUvSet(mesh, 1);
    std::vector<FaceId> sides;
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        const Vector3f n = faceNormal(mesh, f);
        if (std::abs(n.z) < 0.5f * n.length()) {
            sides.push_back(FaceId(f));
        }
    }
    REQUIRE(sides.size() == 16u);
    const uv::ProjectFrame frame = uv::PoleFrame(Vector3f{0, 0, 1}, kUp, kUp, kFront);
    CHECK(std::abs(frame.axisV.z + 1.0f) < 1e-6f);
    CHECK(std::abs(frame.axisN.x + 1.0f) < 1e-6f);
    REQUIRE(uv::Project(mesh, sides, 1, uv::ProjectShape::Cylinder, frame).ok());
    for (const FaceId f : sides) {
        f32 top = 0.0f;
        f32 bottom = 0.0f;
        vAlong(mesh, f.value(), 1, kUp, top, bottom);
        CHECK(top < bottom);
        CHECK(shoelace(mesh, f.value(), 1) < 0.0f);
    }
    // The wrap is marked on the back, away from the front.
    const std::span<const u8> marks = mesh.attributes.get<const u8>(geom::names::uvSeam(1), Domain::Edge);
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    u32 wraps = 0;
    for (u32 e = 0; e < marks.size(); ++e) {
        if (marks[e] == 0) {
            continue;
        }
        // The wrap runs up the side; anything else marked (a cap's rim) does not.
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const Vector3f a = positions[mesh.topology().from(h).index()];
        const Vector3f b = positions[mesh.topology().to(h).index()];
        if (std::abs(b.z - a.z) > 0.9f * (b - a).length()) {
            ++wraps;
            CHECK((a.x + b.x) * 0.5f < 0.0f);
        }
    }
    CHECK(wraps >= 1u);
}

TEST_CASE("UV project: a sphere's up pole is the image's top", "[wem][uv][flatten][project]") {
    geom::PrimitiveParams params;
    params.size = Vector3f{1.0f, 1.0f, 1.0f};
    params.sides = 12;
    params.segments = 6;
    Mesh mesh = geom::MakeSphere(params);
    uv::EnsureUvSet(mesh, 1);
    std::vector<FaceId> faces;
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        faces.push_back(FaceId(f));
    }
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    Vector3f centre{0, 0, 0};
    for (const Vector3f& p : positions) {
        centre = centre + p;
    }
    centre = centre * (1.0f / static_cast<f32>(positions.size()));
    const uv::ProjectFrame frame = uv::PoleFrame(centre, kUp, kUp, kFront);
    REQUIRE(uv::Project(mesh, faces, 1, uv::ProjectShape::Sphere, frame).ok());
    for (const FaceId f : faces) {
        f32 top = 0.0f;
        f32 bottom = 0.0f;
        vAlong(mesh, f.value(), 1, kUp, top, bottom);
        CHECK(top <= bottom);
    }
}

// ============================================================================
// The corpus (EDIT_MODE_UV_PLAN.md §6)
// ============================================================================

TEST_CASE("UV flatten: the MDX corpus's own islands", "[wem][uv][flatten][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 200);

    // Both methods on every disc (EDIT_MODE_UV_AUDIT.md §6.4, the gate that
    // picks the default): Conformal on the mesh, Minimum stretch on a copy.
    struct Tally {
        u32 withoutFlips = 0;
        u32 refused = 0;
        u32 flippedTriangles = 0;
        f64 stretch = 0.0;
        std::vector<f32> each;
        f64 ms = 0.0;
    };
    Tally tally[2];
    u32 discs = 0;
    u32 turned = 0;
    u32 unconverged = 0;
    u32 biggest = 0;
    f64 biggestMs = 0.0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        for (test::IngestedMesh& ingested :
             test::IngestMdx(std::span<const u8>(bytes.data(), bytes.size()))) {
            Mesh mesh = std::move(ingested.mesh);
            if (mesh.faceCount() == 0) {
                continue;
            }
            geom::PrepareForModelling(mesh);
            if (!mesh.hasConnectivity()) {
                continue;
            }
            uv::EnsureUvSet(mesh, 0);
            const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
            Mesh stretched = mesh;
            std::vector<u8> laid(islands.count, 0);
            for (u32 island = 0; island < islands.count; ++island) {
                // A disc: one boundary loop, which is what a flattener is for.
                if (islands.closed[island] != 0 || islands.loops[island] != 1) {
                    continue;
                }
                const u32 faces = static_cast<u32>(islands.facesOf(island).size());
                if (faces < 2) {
                    continue;
                }
                ++discs;
                bool ok = true;
                for (u32 method = 0; method < 2; ++method) {
                    const auto started = std::chrono::steady_clock::now();
                    const uv::FlattenResult result = method == 0 ? uv::Lscm(mesh, islands, island, 0)
                                                                 : uv::MinimumStretch(stretched, islands, island, 0);
                    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started)
                                       .count();
                    Tally& t = tally[method];
                    t.ms += ms;
                    if (!result.ok()) {
                        ++t.refused;
                        ok = false;
                        continue;
                    }
                    t.withoutFlips += result.flipped == 0 ? 1u : 0u;
                    t.flippedTriangles += result.flipped;
                    if (method == 0) {
                        unconverged += result.capped ? 1u : 0u;
                        if (faces > biggest) {
                            biggest = faces;
                            biggestMs = ms;
                        }
                    }
                }
                laid[island] = ok ? 1 : 0;
                // Minimum stretch lays it the way round Conformal did: a whole
                // island turned over is no fold, so no flip count would see it.
                if (ok && HandOf(mesh, islands, island) != HandOf(stretched, islands, island)) {
                    ++turned;
                }
            }
            // Stretch per face, over the discs both methods laid.
            for (u32 method = 0; method < 2; ++method) {
                const std::vector<f32> each = uv::FaceStretch(method == 0 ? mesh : stretched, islands, 0);
                for (u32 island = 0; island < islands.count; ++island) {
                    if (laid[island] == 0) {
                        continue;
                    }
                    for (const u32 face : islands.facesOf(island)) {
                        if (face < each.size() && each[face] > 0.0f) {
                            tally[method].stretch += each[face];
                            tally[method].each.push_back(each[face]);
                        }
                    }
                }
            }
        }
    }
    if (discs == 0) {
        SKIP("no island survived the ingest");
    }
    f64 share[2];
    f32 median[2];
    for (u32 method = 0; method < 2; ++method) {
        Tally& t = tally[method];
        std::sort(t.each.begin(), t.each.end());
        const std::size_t faces = t.each.size();
        share[method] = 100.0 * static_cast<f64>(t.withoutFlips) / static_cast<f64>(discs);
        median[method] = faces ? t.each[faces / 2] : 0.0f;
        std::cout << "UV flatten corpus, " << (method == 0 ? "conformal" : "minimum stretch") << ": " << discs
                  << " discs, " << share[method] << "% without a flip, " << t.refused << " refused, "
                  << t.flippedTriangles << " flipped triangles, stretch mean "
                  << (faces ? t.stretch / static_cast<f64>(faces) : 0.0) << ", median "
                  << (faces ? t.each[faces / 2] : 0.0f) << ", 95th " << (faces ? t.each[faces * 95 / 100] : 0.0f)
                  << ", " << t.ms << " ms in all\n";
    }
    std::cout << "UV flatten corpus: " << unconverged << " stopped at the iteration cap, " << turned
              << " turned over by minimum stretch, largest " << biggest << " faces in " << biggestMs << " ms\n";
    // The measured answer (EDIT_MODE_UV_PLAN.md §19): the plan guessed 99 %
    // before the solver existed, and this is what a free-boundary conformal
    // map of shipped Warcraft III geometry really gives.
    CHECK(share[0] >= 98.0);
    // The gate that made Minimum stretch the default (§6.4): no fewer islands
    // without a flip, less stretch, and never an island mirrored.
    CHECK(share[1] >= share[0]);
    CHECK(median[1] <= median[0]);
    CHECK(turned == 0);
}
