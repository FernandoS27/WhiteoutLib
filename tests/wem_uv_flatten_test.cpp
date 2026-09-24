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

TEST_CASE("UV rectangle: a bent strip lies out as its own arc lengths",
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
    // about the two sides and not about which is x: one is the strip's width,
    // the other the zigzag's arc length, which is longer than the six units the
    // strip spans.
    const f32 shorter = std::min(bounds.width(), bounds.height());
    const f32 longer = std::max(bounds.width(), bounds.height());
    CHECK(std::abs(shorter - 1.0f) < 1e-3f);
    CHECK(longer > 6.0f);
    CHECK(std::abs(longer - 6.0f * std::sqrt(1.0f + 0.35f * 0.35f)) < 1e-2f);
    CHECK(std::abs(bounds.low.x) < 1e-4f);
    CHECK(std::abs(bounds.low.y) < 1e-4f);

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

// ============================================================================
// The corpus (EDIT_MODE_UV_PLAN.md §6)
// ============================================================================

TEST_CASE("UV flatten: the MDX corpus's own islands", "[wem][uv][flatten][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 200);

    u32 discs = 0;
    u32 withoutFlips = 0;
    u32 refused = 0;
    u32 unconverged = 0;
    u32 flippedTriangles = 0;
    u32 biggest = 0;
    f64 biggestMs = 0.0;
    f64 totalMs = 0.0;
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
            for (u32 island = 0; island < islands.count; ++island) {
                // A disc: one boundary loop, which is what a flattener is for.
                if (islands.closed[island] != 0 || islands.loops[island] != 1) {
                    continue;
                }
                const u32 faces = static_cast<u32>(islands.facesOf(island).size());
                if (faces < 2) {
                    continue;
                }
                const auto started = std::chrono::steady_clock::now();
                const uv::FlattenResult result = uv::Lscm(mesh, islands, island, 0);
                const f64 ms = std::chrono::duration<f64, std::milli>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
                totalMs += ms;
                ++discs;
                if (!result.ok()) {
                    ++refused;
                    continue;
                }
                withoutFlips += result.flipped == 0 ? 1u : 0u;
                unconverged += result.residual > 1e-6f ? 1u : 0u;
                flippedTriangles += result.flipped;
                if (faces > biggest) {
                    biggest = faces;
                    biggestMs = ms;
                }
            }
        }
    }
    if (discs == 0) {
        SKIP("no island survived the ingest");
    }
    const f64 share = 100.0 * static_cast<f64>(withoutFlips) / static_cast<f64>(discs);
    std::cout << "UV flatten corpus: " << discs << " discs, " << share
              << "% without a flip, " << refused << " refused, " << unconverged
              << " over the tolerance, " << flippedTriangles << " flipped triangles"
              << ", largest " << biggest
              << " faces in " << biggestMs << " ms, " << totalMs << " ms in all\n";
    // The measured answer (EDIT_MODE_UV_PLAN.md §19): the plan guessed 99 %
    // before the solver existed, and this is what a free-boundary conformal
    // map of shipped Warcraft III geometry really gives.
    CHECK(share >= 98.0);
}
