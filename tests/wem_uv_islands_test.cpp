// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G-U0: islands, wedges and the four layers (EDIT_MODE_UV_DESIGN.md §3, §4;
/// EDIT_MODE_UV_PLAN.md §5.3). Islands are derived, never stored, so every one
/// of these builds a mesh and asks what follows from it.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/uv/islands.h>
#include <whiteout/models/wem/geometry/uv/seams.h>

#include <algorithm>
#include <iostream>
#include <string>
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

/// A mesh from positions, faces and per-corner UVs, with connectivity: what a
/// file's own layout looks like once it is in the kernel.
struct Fixture {
    std::vector<Vector3f> positions;
    std::vector<std::vector<u32>> faces;
    std::vector<std::vector<Vector2f>> uvs;
    std::vector<u32> sections;

    void add(std::vector<u32> corners, std::vector<Vector2f> uv, u32 section = 0) {
        if (uv.empty()) {
            uv.assign(corners.size(), Vector2f{0.0f, 0.0f});
        }
        faces.push_back(std::move(corners));
        uvs.push_back(std::move(uv));
        sections.push_back(section);
    }
};

Mesh meshOf(const Fixture& fixture) {
    geom::FaceSet set;
    set.vertexCount = static_cast<u32>(fixture.positions.size());
    for (const std::vector<u32>& face : fixture.faces) {
        set.addFace(face);
    }
    Mesh mesh;
    mesh.setFaceSet(set);
    const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    std::copy(fixture.positions.begin(), fixture.positions.end(), positions.begin());
    REQUIRE(mesh.ensureConnectivity().ok());
    const std::span<u32> sections = mesh.faceSections();
    std::copy(fixture.sections.begin(), fixture.sections.end(), sections.begin());
    u32 highest = 0;
    for (const u32 s : fixture.sections) {
        highest = std::max(highest, s);
    }
    for (u32 s = 0; s <= highest; ++s) {
        mesh.sections.emplace_back();
    }
    const std::span<Vector2f> uv = mesh.attributes.getOrCreate<Vector2f>(
        geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    const Topology& topology = std::as_const(mesh).topology();
    for (u32 f = 0; f < fixture.faces.size(); ++f) {
        u32 corner = 0;
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            uv[h.index()] = fixture.uvs[f][corner];
            ++corner;
        }
    }
    mesh.recomputeBounds();
    return mesh;
}

/// `sx` by `sy` quads in the xy plane, each as one quad face, UVs continuous
/// over the whole sheet: one island until something cuts it.
Fixture grid(u32 sx, u32 sy) {
    Fixture f;
    const u32 nx = sx + 1;
    const u32 ny = sy + 1;
    const auto at = [&](u32 x, u32 y) { return x * ny + y; };
    for (u32 x = 0; x < nx; ++x) {
        for (u32 y = 0; y < ny; ++y) {
            f.positions.push_back(Vector3f{static_cast<f32>(x), static_cast<f32>(y), 0.0f});
        }
    }
    const auto uvAt = [&](u32 v) {
        return Vector2f{static_cast<f32>(v / ny), static_cast<f32>(v % ny)};
    };
    for (u32 x = 0; x < sx; ++x) {
        for (u32 y = 0; y < sy; ++y) {
            const u32 a = at(x, y), b = at(x + 1, y), c = at(x + 1, y + 1), d = at(x, y + 1);
            f.add({a, b, c, d}, {uvAt(a), uvAt(b), uvAt(c), uvAt(d)});
        }
    }
    return f;
}

/// The edge between two vertices, which must exist.
EdgeId edgeBetween(const Mesh& mesh, u32 a, u32 b) {
    const HalfedgeId h = mesh.topology().findHalfedge(VertexId(a), VertexId(b));
    REQUIRE(h.valid());
    return Topology::edge(h);
}

/// How many wedges the corners round @p vertex fall into.
u32 wedgesAt(const Mesh& mesh, const uv::UvIslands& islands, u32 vertex) {
    std::vector<u32> seen;
    const Topology& topology = mesh.topology();
    for (const HalfedgeId h : topology.voh(VertexId(vertex))) {
        const u32 wedge = islands.wedgeOf(h);
        if (wedge != geom::kInvalidId) {
            seen.push_back(wedge);
        }
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    return static_cast<u32>(seen.size());
}

void markSeam(Mesh& mesh, EdgeId edge, u32 set = 0) {
    const EdgeId one[1] = {edge};
    uv::ApplyMarks(mesh, set, std::span<const EdgeId>(one, 1), true);
}

} // namespace

TEST_CASE("UV islands: a continuous sheet is one island", "[wem][uv]") {
    const Mesh mesh = meshOf(grid(2, 2));
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 1u);
    REQUIRE(islands.facesOf(0).size() == 4u);
    REQUIRE(islands.closed[0] == 0u);
    REQUIRE(islands.loops[0] == 1u);
    // Every corner of the sheet is one UV vertex, so there are as many wedges
    // as there are vertices.
    REQUIRE(islands.wedgeCount == mesh.vertexCount());
    // The sheet's border is its boundary: eight edges round a 2x2 grid.
    REQUIRE(islands.boundaryOf(0).size() == 8u);
}

TEST_CASE("UV islands: a cut across a sheet makes two", "[wem][uv]") {
    // A 2x1 strip of quads, cut down the middle column: the cut separates the
    // two quads, and the two vertices the cut runs through carry two UV
    // vertices each.
    Mesh mesh = meshOf(grid(2, 1));
    const EdgeId cut = edgeBetween(mesh, 2, 3);
    markSeam(mesh, cut);

    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 2u);
    REQUIRE(islands.facesOf(0).size() == 1u);
    REQUIRE(islands.facesOf(1).size() == 1u);
    REQUIRE(wedgesAt(mesh, islands, 2) == 2u);
    REQUIRE(wedgesAt(mesh, islands, 3) == 2u);
    REQUIRE(wedgesAt(mesh, islands, 0) == 1u);
    // Four corners of one quad and four of the other, none shared.
    REQUIRE(islands.wedgeCount == 8u);

    // A mark, not a difference: the UVs still agree across the cut, so the
    // document is unchanged and only the islands moved.
    const uv::UvIslands without = uv::BuildUvIslands(mesh, 1);
    REQUIRE(without.count == 1u);
}

TEST_CASE("UV islands: UVs that disagree cut without a mark", "[wem][uv]") {
    // What an imported file looks like: no layer written, the corners split.
    Fixture f = grid(2, 1);
    // The right quad's corners at the shared column move, so the two faces
    // disagree at both ends of the middle edge.
    f.uvs[1][0] = Vector2f{5.0f, 0.0f};
    f.uvs[1][3] = Vector2f{5.0f, 1.0f};
    const Mesh mesh = meshOf(f);

    const std::vector<u8> cuts = uv::CutsOf(mesh, 0);
    const EdgeId middle = edgeBetween(mesh, 2, 3);
    REQUIRE(cuts[middle.index()] == 1u);

    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 2u);
    REQUIRE(wedgesAt(mesh, islands, 2) == 2u);
}

TEST_CASE("UV islands: a dart splits its mouth and not its tip", "[wem][uv]") {
    // A cut from the border to one interior vertex. The surface stays one
    // island -- the way round is still open -- and the corners at the cut's
    // mouth part while the tip's fan closes round it, which is what a dart is.
    Mesh mesh = meshOf(grid(2, 2));
    markSeam(mesh, edgeBetween(mesh, 3, 4));

    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 1u);
    REQUIRE(wedgesAt(mesh, islands, 3) == 2u); // the mouth, on the border
    REQUIRE(wedgesAt(mesh, islands, 4) == 1u); // the tip, fan closed
    REQUIRE(islands.wedgeCount == mesh.vertexCount() + 1u);
    // One loop still: the boundary walks out along one side of the dart and
    // back along the other.
    REQUIRE(islands.loops[0] == 1u);
}

TEST_CASE("UV islands: a section border cuts", "[wem][uv]") {
    Fixture f = grid(2, 1);
    f.sections[1] = 1;
    const Mesh mesh = meshOf(f);
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 2u);
    // Every island is in one section, so a host can always name the material
    // an island draws with.
    REQUIRE(islands.islandOf(0) != islands.islandOf(1));
}

TEST_CASE("UV islands: a closed surface has no boundary", "[wem][uv]") {
    geom::PrimitiveParams params;
    Mesh box = geom::MakeBox(params);
    REQUIRE(box.ensureConnectivity().ok());
    // A box's own unwrap is a 0-1 square per side, so it arrives as six
    // islands with the same seams a modeller would have cut.
    {
        const uv::UvIslands sides = uv::BuildUvIslands(box, 0);
        REQUIRE(sides.count == 6u);
        for (u32 i = 0; i < sides.count; ++i) {
            REQUIRE(sides.closed[i] == 0u);
            REQUIRE(sides.loops[i] == 1u);
        }
    }
    // Flattened to one value everywhere, nothing disagrees and the whole cube
    // is one island with no boundary at all -- what no flattener can lay out.
    {
        const std::span<Vector2f> uvs =
            box.attributes.getOrCreate<Vector2f>(geom::names::uv(0), Domain::Halfedge,
                                                 geom::AttrType::F32x2);
        std::fill(uvs.begin(), uvs.end(), Vector2f{0.0f, 0.0f});
        const uv::UvIslands whole = uv::BuildUvIslands(box, 0);
        REQUIRE(whole.count == 1u);
        REQUIRE(whole.closed[0] == 1u);
        REQUIRE(whole.boundaryOf(0).empty());
        REQUIRE(whole.loops[0] == 0u);
        REQUIRE(whole.wedgeCount == box.vertexCount());
    }
}

TEST_CASE("UV first visit: a set the file placed is locked", "[wem][uv]") {
    Fixture f = grid(2, 1);
    f.uvs[1][0] = Vector2f{5.0f, 0.0f};
    f.uvs[1][3] = Vector2f{5.0f, 1.0f};
    Mesh mesh = meshOf(f);

    const uv::UvFirstVisit visit = uv::EnsureUvSet(mesh, 0);
    REQUIRE_FALSE(visit.wroteUvs);
    REQUIRE(visit.wroteSeams);
    REQUIRE(visit.wroteFree);
    REQUIRE_FALSE(visit.bornFree);

    // The layout it shipped, read back as the cut that made it -- and only the
    // interior one: a border cuts by being a border.
    const std::span<const u8> marks =
        mesh.attributes.get<const u8>(geom::names::uvSeam(0), Domain::Edge);
    REQUIRE(marks[edgeBetween(mesh, 2, 3).index()] == 1u);
    u32 marked = 0;
    for (const u8 m : marks) {
        marked += m;
    }
    REQUIRE(marked == 1u);

    // Every face of it is locked: the file placed them.
    const std::span<const u8> free =
        mesh.attributes.get<const u8>(geom::names::uvFree(0), Domain::Face);
    REQUIRE(free.size() == mesh.faceCount());
    for (const u8 value : free) {
        REQUIRE(value == 0u);
    }
    REQUIRE(mesh.attributes.has(geom::names::uvPin(0), Domain::Halfedge));

    // Asking twice changes nothing.
    const uv::UvFirstVisit again = uv::EnsureUvSet(mesh, 0);
    REQUIRE_FALSE(again.wroteUvs);
    REQUIRE_FALSE(again.wroteSeams);
    REQUIRE_FALSE(again.wroteFree);
}

TEST_CASE("UV first visit: a set that never existed is born free", "[wem][uv]") {
    Mesh mesh = meshOf(grid(2, 1));
    const uv::UvFirstVisit visit = uv::EnsureUvSet(mesh, 1);
    REQUIRE(visit.wroteUvs);
    REQUIRE(visit.bornFree);
    REQUIRE(geom::UvSetCount(mesh.attributes) == 2u);

    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(1), Domain::Halfedge);
    for (const Vector2f& value : uvs) {
        REQUIRE(value.x == 0.0f);
        REQUIRE(value.y == 0.0f);
    }
    const std::span<const u8> free =
        mesh.attributes.get<const u8>(geom::names::uvFree(1), Domain::Face);
    for (const u8 value : free) {
        REQUIRE(value == 1u);
    }
    // Nothing disagrees in a set of zeroes, so it is one island.
    REQUIRE(uv::BuildUvIslands(mesh, 1).count == 1u);
}

TEST_CASE("UV sets: the count is the contiguous run", "[wem][uv]") {
    Mesh mesh = meshOf(grid(1, 1));
    REQUIRE(geom::UvSetCount(mesh.attributes) == 1u);
    mesh.attributes.getOrCreate<Vector2f>(geom::names::uv(2), Domain::Halfedge,
                                          geom::AttrType::F32x2);
    // A gap is a broken document; counting past it would hide it.
    REQUIRE(geom::UvSetCount(mesh.attributes) == 1u);
    mesh.attributes.getOrCreate<Vector2f>(geom::names::uv(1), Domain::Halfedge,
                                          geom::AttrType::F32x2);
    REQUIRE(geom::UvSetCount(mesh.attributes) == 3u);
}

TEST_CASE("UV layers: the reserved table knows the three families", "[wem][uv]") {
    REQUIRE(geom::names::uvSeam(0) == "uvSeam0");
    REQUIRE(geom::names::uvPin(3) == "uvPin3");
    REQUIRE(geom::names::uvFree(11) == "uvFree11");
    REQUIRE(geom::LookupReserved("uvSeam0").domain == Domain::Edge);
    REQUIRE(geom::LookupReserved("uvSeam0").type == geom::AttrType::Bool);
    REQUIRE(geom::LookupReserved("uvPin0").domain == Domain::Halfedge);
    REQUIRE(geom::LookupReserved("uvFree0").domain == Domain::Face);
    // `uv` itself is digits only, so the three never take its rows.
    REQUIRE(geom::LookupReserved("uv1").domain == Domain::Halfedge);
    REQUIRE(geom::LookupReserved("uv1").type == geom::AttrType::F32x2);
    REQUIRE(geom::names::IsUvPin("uvPin0"));
    REQUIRE(geom::names::IsUvPin("uvPin12"));
    REQUIRE_FALSE(geom::names::IsUvPin("uvPin"));
    REQUIRE_FALSE(geom::names::IsUvPin("uv0"));
    REQUIRE_FALSE(geom::names::IsUvPin("uvSeam0"));
}

TEST_CASE("UV seams: the delimit is re-derived both ways", "[wem][uv]") {
    // Two quads whose corners disagree across the middle: the delimit is there.
    Fixture f = grid(2, 1);
    f.uvs[1][0] = Vector2f{5.0f, 0.0f};
    f.uvs[1][3] = Vector2f{5.0f, 1.0f};
    Mesh mesh = meshOf(f);
    const u32 middle = edgeBetween(mesh, 2, 3).index();

    REQUIRE(uv::MarkDelimitSeams(mesh) == 1u);
    REQUIRE(mesh.attributes.get<const u8>(geom::names::kSeam, Domain::Edge)[middle] == 1u);

    // Bring the corners back together and the delimit goes: a walk that stopped
    // there would be stopping at history.
    {
        const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
            geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
        const Topology& topology = std::as_const(mesh).topology();
        u32 corner = 0;
        for (const HalfedgeId h : topology.fh(FaceId(1))) {
            uvs[h.index()] = f.uvs[1][corner] = grid(2, 1).uvs[1][corner];
            ++corner;
        }
    }
    REQUIRE(uv::MarkDelimitSeams(mesh) == 0u);
    REQUIRE(mesh.attributes.get<const u8>(geom::names::kSeam, Domain::Edge)[middle] == 0u);
}

// ============================================================================
// The corpus (EDIT_MODE_UV_PLAN.md §5.3)
// ============================================================================

TEST_CASE("UV islands: the MDX corpus, prepared", "[wem][uv][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 400);

    std::vector<u32> perMesh;
    std::vector<u32> largest;
    u32 meshes = 0;
    u32 closedIslands = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        for (test::IngestedMesh& ingested : test::IngestMdx(
                 std::span<const u8>(bytes.data(), bytes.size()))) {
            Mesh mesh = std::move(ingested.mesh);
            if (mesh.faceCount() == 0) {
                continue;
            }
            // On the prepared copy, as the survey welded: the file's own
            // (position, UV) split is what the islands must reproduce.
            geom::PrepareForModelling(mesh);
            if (!mesh.hasConnectivity()) {
                continue;
            }
            const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
            ++meshes;
            perMesh.push_back(islands.count);

            // The invariant: every live face is in exactly one island, and the
            // islands' face lists are the same faces.
            u32 listed = 0;
            u32 biggest = 0;
            for (u32 island = 0; island < islands.count; ++island) {
                const std::span<const u32> faces = islands.facesOf(island);
                listed += static_cast<u32>(faces.size());
                biggest = std::max(biggest, static_cast<u32>(faces.size()));
                for (const u32 face : faces) {
                    REQUIRE(islands.islandOf(face) == island);
                }
                closedIslands += islands.closed[island];
            }
            u32 live = 0;
            for (u32 face = 0; face < islands.islandOfFace.size(); ++face) {
                live += islands.islandOfFace[face] != geom::kInvalidId ? 1u : 0u;
            }
            REQUIRE(listed == live);
            largest.push_back(biggest);
            // A wedge is a UV copy of a vertex, so there is never one fewer.
            REQUIRE(islands.wedgeCount >= mesh.vertexCount());
        }
    }
    if (meshes == 0) {
        SKIP("no mesh survived the ingest");
    }
    const auto median = [](std::vector<u32>& values) {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    std::cout << "UV corpus: " << meshes << " meshes, median islands " << median(perMesh)
              << ", median largest island " << median(largest) << " faces, " << closedIslands
              << " closed islands\n";
    REQUIRE(median(perMesh) >= 1u);
}
