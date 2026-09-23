// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G22's canonical arms (EDIT_MODE_MODELLING_DESIGN.md §2.1, _PLAN.md ML0).
///
/// Every Halfedge layer is indexed by the in-memory halfedge id and every Edge
/// layer by the edge id, and the disk form is the face set: a load numbers them
/// as a fresh build does. A maintaining op numbers them otherwise, so a save
/// straight after one scrambled every corner and edge value. Each case here
/// reads the corners by (face, corner vertex) and the edges by their endpoints,
/// which no renumbering changes, and holds them equal across a `.wem` trip.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/checks.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/geometry/triangulation.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/writer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

using geom::Domain;
using geom::EdgeId;
using geom::FaceId;
using geom::HalfedgeId;
using geom::Topology;
using geom::VertexId;

/// A position as a comparable key.
using Point = std::array<f32, 3>;

Point pointOf(const Vector3f& p) {
    return {p.x, p.y, p.z};
}

/// C1-C9 over one mesh; empty when clean.
std::string intact(const Mesh& mesh) {
    Diagnostics diagnostics;
    geom::CheckStructural(mesh, 0, diagnostics);
    geom::CheckManifold(mesh, 0, diagnostics);
    return diagnostics.hasErrors() ? diagnostics.formatHistogram() : std::string();
}

/// @p rows x @p columns cells of @p valence 3 (two triangles) or 4 (one quad),
/// the heights uneven so no two positions agree and nothing is flat.
Mesh grid(u32 columns, u32 rows, u32 valence) {
    geom::FaceSet faces;
    faces.vertexCount = (columns + 1) * (rows + 1);
    const auto at = [&](u32 x, u32 y) { return y * (columns + 1) + x; };
    for (u32 y = 0; y < rows; ++y) {
        for (u32 x = 0; x < columns; ++x) {
            if (valence == 4) {
                const std::array<u32, 4> quad{at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)};
                faces.addFace(quad);
            } else {
                faces.addTriangle(at(x, y), at(x + 1, y), at(x + 1, y + 1));
                faces.addTriangle(at(x, y), at(x + 1, y + 1), at(x, y + 1));
            }
        }
    }
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> positions =
        mesh.attributes.getOrCreate<Vector3f>(geom::names::kPosition, Domain::Vertex,
                                              geom::AttrType::F32x3);
    for (u32 y = 0; y <= rows; ++y) {
        for (u32 x = 0; x <= columns; ++x) {
            positions[at(x, y)] = Vector3f{static_cast<f32>(x), static_cast<f32>(y),
                                           0.05f * static_cast<f32>((x * 7 + y * 3) % 5)};
        }
    }
    mesh.attributes.getOrCreate<u32>(geom::names::kSection, Domain::Face, geom::AttrType::U32);
    mesh.sections.emplace_back();
    REQUIRE(mesh.ensureConnectivity().ok());
    return mesh;
}

/// Every corner a value of its own, derived from the face and the corner's
/// position so no renumbering changes it; every edge likewise; every face a
/// smoothing group; every polygon a stored row.
void paint(Mesh& mesh) {
    const Mesh& readable = mesh;
    const Topology& topology = readable.topology();
    const std::span<const Vector3f> positions =
        readable.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const std::span<Vector3f> normals = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kNormal, Domain::Halfedge, geom::AttrType::F32x3);
    const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
        geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    const std::span<f32> crease =
        mesh.attributes.getOrCreate<f32>(geom::names::kCrease, Domain::Edge, geom::AttrType::F32);
    const std::span<u8> sharp =
        mesh.attributes.getOrCreate<u8>(geom::names::kSharp, Domain::Edge, geom::AttrType::Bool);
    const std::span<u32> smoothing = mesh.attributes.getOrCreate<u32>(
        geom::names::kSmoothGroup, Domain::Face, geom::AttrType::U32);
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        Vector3f centre{0, 0, 0};
        u32 count = 0;
        for (const VertexId v : topology.fv(FaceId(f))) {
            centre.x += positions[v.index()].x;
            centre.y += positions[v.index()].y;
            ++count;
        }
        smoothing[f] = 100 + f;
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            const Vector3f& p = positions[topology.from(h).index()];
            normals[h.index()] = Vector3f{p.x + centre.x * 0.01f, p.y, 1.0f + centre.y * 0.01f};
            uvs[h.index()] = Vector2f{p.x * 0.1f + centre.x / count, p.y * 0.1f + centre.y / count};
        }
    }
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const Vector3f& a = positions[topology.from(h).index()];
        const Vector3f& b = positions[topology.to(h).index()];
        crease[e] = std::abs(a.x - b.x) + 2.0f * std::abs(a.y - b.y) + 0.1f * (a.x + b.x + a.y + b.y);
        sharp[e] = static_cast<u8>((static_cast<u32>(a.x + b.x + 3.0f * (a.y + b.y)) % 2));
    }
    geom::MaterialiseRows(mesh);
}

using CornerKey = std::pair<std::vector<Point>, Point>;
using EdgeKey = std::pair<Point, Point>;

std::vector<u8> bytesAt(const Mesh& mesh, Domain domain, u32 index) {
    std::vector<u8> out;
    std::vector<const geom::AttrLayer*> layers;
    for (const geom::AttrLayer& layer : mesh.attributes.layers()) {
        if (layer.domain == domain) {
            layers.push_back(&layer);
        }
    }
    std::sort(layers.begin(), layers.end(),
              [](const geom::AttrLayer* a, const geom::AttrLayer* b) { return a->name < b->name; });
    for (const geom::AttrLayer* layer : layers) {
        const std::size_t stride = geom::AttrTypeSize(layer->type);
        const std::size_t at = stride * index;
        if (at + stride <= layer->data.size()) {
            out.insert(out.end(), layer->data.begin() + at, layer->data.begin() + at + stride);
        }
    }
    return out;
}

/// Every live corner's Halfedge values keyed by (its face's corner positions,
/// its own position); every live edge's Edge values by its endpoints; every
/// face's Face values by its corners.
struct Tables {
    std::map<CornerKey, std::vector<u8>> corners;
    std::map<EdgeKey, std::vector<u8>> edges;
    std::map<std::vector<Point>, std::vector<u8>> faces;
    std::map<std::vector<Point>, std::vector<std::array<Point, 3>>> rows; ///< As drawn, by position.
};

Tables tablesOf(Mesh mesh) {
    REQUIRE(mesh.ensureConnectivity().ok());
    Tables out;
    const Mesh& readable = mesh;
    const Topology& topology = readable.topology();
    const std::span<const Vector3f> positions =
        readable.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (topology.isDeleted(FaceId(f))) {
            continue;
        }
        std::vector<Point> face;
        for (const VertexId v : topology.fv(FaceId(f))) {
            face.push_back(pointOf(positions[v.index()]));
        }
        std::sort(face.begin(), face.end());
        out.faces[face] = bytesAt(readable, Domain::Face, f);
        std::vector<std::array<Point, 3>>& row = out.rows[face];
        const std::span<const u32> stored = readable.triangulation.row(f);
        for (std::size_t t = 0; t + 2 < stored.size(); t += 3) {
            row.push_back({pointOf(positions[stored[t]]), pointOf(positions[stored[t + 1]]),
                           pointOf(positions[stored[t + 2]])});
        }
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            out.corners[{face, pointOf(positions[topology.from(h).index()])}] =
                bytesAt(readable, Domain::Halfedge, static_cast<u32>(h.index()));
        }
    }
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        if (topology.isDeleted(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        Point a = pointOf(positions[topology.from(h).index()]);
        Point b = pointOf(positions[topology.to(h).index()]);
        if (b < a) {
            std::swap(a, b);
        }
        out.edges[{a, b}] = bytesAt(readable, Domain::Edge, e);
    }
    return out;
}

Mesh throughWem(const Mesh& mesh) {
    Document document;
    document.declare(ProfileId::Generic);
    document.defaultProfile = ProfileId::Generic;
    Model model;
    model.name = "canonical";
    model.meshes.push_back(mesh);
    document.models.push_back(std::move(model));
    Writer writer;
    const std::vector<u8> bytes = writer.write(document);
    Parser parser;
    std::optional<Document> read = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(read.has_value());
    REQUIRE(read->models.size() == 1);
    REQUIRE(read->models[0].meshes.size() == 1);
    return std::move(read->models[0].meshes[0]);
}

void requireSame(const Tables& before, const Tables& after) {
    CHECK(before.corners.size() == after.corners.size());
    CHECK(before.edges.size() == after.edges.size());
    CHECK(before.faces.size() == after.faces.size());
    u32 differing = 0;
    for (const auto& [key, value] : before.corners) {
        const auto found = after.corners.find(key);
        differing += found == after.corners.end() || found->second != value ? 1u : 0u;
    }
    CHECK(differing == 0u);
    differing = 0;
    for (const auto& [key, value] : before.edges) {
        const auto found = after.edges.find(key);
        differing += found == after.edges.end() || found->second != value ? 1u : 0u;
    }
    CHECK(differing == 0u);
    differing = 0;
    for (const auto& [key, value] : before.faces) {
        const auto found = after.faces.find(key);
        differing += found == after.faces.end() || found->second != value ? 1u : 0u;
    }
    CHECK(differing == 0u);
    differing = 0;
    for (const auto& [key, value] : before.rows) {
        const auto found = after.rows.find(key);
        differing += found == after.rows.end() || found->second != value ? 1u : 0u;
    }
    CHECK(differing == 0u);
}

/// The first interior edge, and one on the border.
EdgeId interiorEdge(const Mesh& mesh) {
    const Topology& topology = mesh.topology();
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        if (!topology.isBoundary(EdgeId(e)) && !topology.isDeleted(EdgeId(e))) {
            return EdgeId(e);
        }
    }
    return EdgeId();
}

EdgeId borderEdge(const Mesh& mesh) {
    const Topology& topology = mesh.topology();
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        if (topology.isBoundary(EdgeId(e)) && !topology.isDeleted(EdgeId(e))) {
            return EdgeId(e);
        }
    }
    return EdgeId();
}

/// The maintaining ops, each on its own painted grid, each leaving the mesh
/// renumbered as a fresh build would not.
struct Case {
    const char* name;
    u32 valence;
    std::function<bool(Mesh&)> apply;
};

std::vector<Case> maintainingOps() {
    return {
        {"SplitEdge inside", 3,
         [](Mesh& m) { return geom::SplitEdge(m, interiorEdge(m), 0.3f).valid(); }},
        {"SplitEdge on the border", 3,
         [](Mesh& m) { return geom::SplitEdge(m, borderEdge(m), 0.6f).valid(); }},
        {"SplitFace", 4, [](Mesh& m) { return geom::SplitFace(m, FaceId(4), 0, 2).valid(); }},
        {"FlipEdge", 3, [](Mesh& m) { return geom::FlipEdge(m, interiorEdge(m)); }},
        {"DissolveEdge", 3, [](Mesh& m) { return geom::DissolveEdge(m, interiorEdge(m)); }},
        {"Triangulate", 4, [](Mesh& m) { return geom::Triangulate(m, FaceId(4)) == 1u; }},
        {"CollapseEdge", 3,
         [](Mesh& m) {
             const Topology& topology = std::as_const(m).topology();
             for (u32 h = 0; h < topology.halfedgeCount(); ++h) {
                 if (geom::IsCollapseLegal(m, HalfedgeId(h)) &&
                     !topology.isBoundary(Topology::edge(HalfedgeId(h)))) {
                     return geom::CollapseEdge(m, HalfedgeId(h));
                 }
             }
             return false;
         }},
        {"SplitEdge then DissolveVertex", 3,
         [](Mesh& m) {
             const VertexId v = geom::SplitEdge(m, interiorEdge(m), 0.5f);
             // Each side a quad now, so the vertex goes back out legally.
             return v.valid() && geom::DissolveVertex(m, v);
         }},
    };
}

} // namespace

TEST_CASE("wem canonical a maintaining op then Canonicalize reloads every value in place",
          "[wem][geometry][canonical]") {
    for (const Case& c : maintainingOps()) {
        CAPTURE(c.name);
        Mesh mesh = grid(3, 3, c.valence);
        paint(mesh);
        REQUIRE(c.apply(mesh));
        const Tables before = tablesOf(mesh);

        const geom::CanonicalRemap remap = geom::Canonicalize(mesh);
        CHECK(geom::IsCanonical(mesh));
        CHECK_FALSE(geom::NumberingDirty(mesh));
        CHECK(intact(mesh).empty());
        requireSame(before, tablesOf(mesh));
        requireSame(before, tablesOf(throughWem(mesh)));
        // A second call has nothing to do.
        CHECK(geom::Canonicalize(mesh).identity());
        (void)remap;
    }
}

TEST_CASE("wem canonical a save straight after a maintaining op writes the canonical numbering",
          "[wem][geometry][canonical]") {
    // No `Canonicalize`: the writer is the chokepoint (§2.1). CollapseEdge
    // leaves lazily deleted faces and vertices behind, which the write compacts.
    for (const Case& c : maintainingOps()) {
        CAPTURE(c.name);
        Mesh mesh = grid(3, 3, c.valence);
        paint(mesh);
        REQUIRE(c.apply(mesh));
        const Tables before = tablesOf(mesh);
        const Mesh read = throughWem(mesh);
        requireSame(before, tablesOf(read));
        CHECK(intact(read).empty());
        // The mesh in memory is untouched by the write.
        CHECK(geom::NumberingDirty(mesh));
    }
}

TEST_CASE("wem canonical the design's example: two triangles and one SplitEdge",
          "[wem][geometry][canonical]") {
    // SplitEdge gives the new edge pair the next ids, so an even halfedge runs
    // high -> low -- a rebuild numbers it odd, and every corner scrambled.
    Mesh mesh = grid(1, 1, 3);
    paint(mesh);
    const EdgeId diagonal = interiorEdge(mesh);
    REQUIRE(diagonal.valid());
    REQUIRE(geom::SplitEdge(mesh, diagonal, 0.5f).valid());
    CHECK_FALSE(geom::IsCanonical(mesh));
    const Tables before = tablesOf(mesh);
    requireSame(before, tablesOf(throughWem(mesh)));
}

TEST_CASE("wem canonical invalidateConnectivity carries the values to the next build",
          "[wem][geometry][canonical]") {
    Mesh mesh = grid(3, 3, 3);
    paint(mesh);
    REQUIRE(geom::SplitEdge(mesh, interiorEdge(mesh), 0.25f).valid());
    const Tables before = tablesOf(mesh);
    mesh.invalidateConnectivity();
    REQUIRE(mesh.ensureConnectivity().ok());
    requireSame(before, tablesOf(mesh));
}

TEST_CASE("wem canonical every rebuilding op leaves the mesh canonical",
          "[wem][geometry][canonical]") {
    SECTION("WeldVertices") {
        Mesh mesh = grid(2, 2, 3);
        std::span<Vector3f> positions =
            mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex);
        positions[1] = positions[0];
        (void)geom::WeldVertices(mesh, 1e-3f, false);
        CHECK(geom::IsCanonical(mesh));
    }
    SECTION("UnifyWinding") {
        Mesh mesh = grid(2, 2, 3);
        (void)geom::UnifyWinding(mesh);
        CHECK(geom::IsCanonical(mesh));
    }
    SECTION("SplitVertexByHalfedgeAttr") {
        Mesh mesh = grid(2, 2, 3);
        paint(mesh);
        const std::string layers[] = {geom::names::uv(0)};
        CHECK(geom::SplitVertexByHalfedgeAttr(mesh, layers) > 0u);
        CHECK(geom::IsCanonical(mesh));
    }
    SECTION("MergeMeshes and SplitMesh, of meshes an op renumbered") {
        std::vector<Mesh> parts{grid(2, 1, 3), grid(1, 2, 4)};
        // Apart, so no position is in both and the tables key one way.
        for (Vector3f& p :
             parts[1].attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex)) {
            p.x += 10.0f;
        }
        for (Mesh& part : parts) {
            paint(part);
            REQUIRE(geom::SplitEdge(part, interiorEdge(part), 0.5f).valid());
        }
        const Tables first = tablesOf(parts[0]);
        Mesh merged = geom::MergeMeshes(parts);
        CHECK(geom::IsCanonical(merged));
        CHECK(intact(merged).empty());
        // The first part's values arrive where they were: its corners and edges
        // are a subset of the merge's, keyed by position.
        const Tables all = tablesOf(merged);
        u32 missing = 0;
        for (const auto& [key, value] : first.corners) {
            const auto found = all.corners.find(key);
            missing += found == all.corners.end() || found->second != value ? 1u : 0u;
        }
        for (const auto& [key, value] : first.edges) {
            const auto found = all.edges.find(key);
            missing += found == all.edges.end() || found->second != value ? 1u : 0u;
        }
        CHECK(missing == 0u);
        for (const Mesh& part : geom::SplitMesh(merged)) {
            CHECK(geom::IsCanonical(part));
        }
    }
}

// ============================================================================
// The collapse arms (§2.7.9)
// ============================================================================

TEST_CASE("wem canonical a collapse beside quads is legal", "[wem][geometry][canonical]") {
    SECTION("a 3x3 quad grid's interior edge") {
        Mesh mesh = grid(3, 3, 4);
        const Topology& topology = std::as_const(mesh).topology();
        // Vertices (1,1) and (2,1): 5 and 6 on a 4-wide row.
        const HalfedgeId h = topology.findHalfedge(VertexId(5), VertexId(6));
        REQUIRE(h.valid());
        CHECK(geom::IsCollapseLegal(mesh, h));
        REQUIRE(geom::CollapseEdge(mesh, h));
        CHECK(intact(mesh).empty());
    }
    SECTION("a quad strip's border edge") {
        Mesh mesh = grid(3, 1, 4);
        const Topology& topology = std::as_const(mesh).topology();
        const HalfedgeId h = topology.findHalfedge(VertexId(1), VertexId(2));
        REQUIRE(h.valid());
        CHECK(geom::IsCollapseLegal(mesh, h));
        REQUIRE(geom::CollapseEdge(mesh, h));
        CHECK(intact(mesh).empty());
    }
}

TEST_CASE("wem canonical a collapse that would repeat a vertex in a polygon is refused",
          "[wem][geometry][canonical]") {
    // A flat pillow: a hexagon (0..5) on the front, two quads on the back split
    // by the edge 0-3. The quads have no apex, so the rings pass; the hexagon
    // holds both 0 and 3, and collapsing would visit 3 twice.
    geom::FaceSet faces;
    faces.vertexCount = 6;
    const std::array<u32, 6> hexagon{0, 1, 2, 3, 4, 5};
    const std::array<u32, 4> back1{3, 2, 1, 0};
    const std::array<u32, 4> back2{0, 5, 4, 3};
    faces.addFace(hexagon);
    faces.addFace(back1);
    faces.addFace(back2);
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    for (u32 i = 0; i < 6; ++i) {
        const f32 angle = 1.0471975f * static_cast<f32>(i);
        positions[i] = Vector3f{std::cos(angle), std::sin(angle), 0.0f};
    }
    REQUIRE(mesh.ensureConnectivity().ok());
    const HalfedgeId h = std::as_const(mesh).topology().findHalfedge(VertexId(0), VertexId(3));
    REQUIRE(h.valid());
    CHECK_FALSE(geom::IsCollapseLegal(mesh, h));
    CHECK_FALSE(geom::CollapseEdge(mesh, h));
}

TEST_CASE("wem canonical SplitFaceAt refuses a diagonal that is already an edge",
          "[wem][geometry][canonical]") {
    // Two triangles (0,1,2) and (0,2,3), capped on the back by (0,3,2,1):
    // cutting the cap along 0-2 would make a second 0-2 edge (§2.6).
    geom::FaceSet faces;
    faces.vertexCount = 4;
    faces.addTriangle(0, 1, 2);
    faces.addTriangle(0, 2, 3);
    const std::array<u32, 4> cap{0, 3, 2, 1};
    faces.addFace(cap);
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    positions[0] = Vector3f{0, 0, 0};
    positions[1] = Vector3f{1, 0, 0};
    positions[2] = Vector3f{1, 1, 0.2f};
    positions[3] = Vector3f{0, 1, 0};
    REQUIRE(mesh.ensureConnectivity().ok());
    CHECK_FALSE(geom::SplitFace(mesh, FaceId(2), 0, 2).valid());
    CHECK(intact(mesh).empty());
}

TEST_CASE("wem canonical a weld carries every Edge value, and a closed edge combines its sides",
          "[wem][geometry][canonical]") {
    // Two quads side by side that the file kept apart: vertices 1/4 and 2/7
    // coincide. The weld closes 1-2 with 4-7: `crease` takes the larger side,
    // `sharp` either side's. Edges at the merged-away vertices keep their own
    // values -- the old endpoint lookup named only each cluster's first vertex.
    geom::FaceSet faces;
    faces.vertexCount = 8;
    const std::array<u32, 4> left{0, 1, 2, 3};
    const std::array<u32, 4> right{4, 5, 6, 7};
    faces.addFace(left);
    faces.addFace(right);
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    positions[0] = Vector3f{0, 0, 0};
    positions[1] = Vector3f{1, 0, 0};
    positions[2] = Vector3f{1, 1, 0};
    positions[3] = Vector3f{0, 1, 0};
    positions[4] = Vector3f{1, 0, 0};
    positions[5] = Vector3f{2, 0, 0.1f};
    positions[6] = Vector3f{2, 1, 0.1f};
    positions[7] = Vector3f{1, 1, 0};
    mesh.attributes.getOrCreate<u32>(geom::names::kSection, Domain::Face, geom::AttrType::U32);
    mesh.sections.emplace_back();
    REQUIRE(mesh.ensureConnectivity().ok());
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<f32> crease =
        mesh.attributes.getOrCreate<f32>(geom::names::kCrease, Domain::Edge, geom::AttrType::F32);
    const std::span<u8> sharp =
        mesh.attributes.getOrCreate<u8>(geom::names::kSharp, Domain::Edge, geom::AttrType::Bool);
    const auto edgeOf = [&](u32 a, u32 b) {
        const HalfedgeId h = topology.findHalfedge(VertexId(a), VertexId(b));
        REQUIRE(h.valid());
        return Topology::edge(h).index();
    };
    crease[edgeOf(1, 2)] = 0.25f;
    crease[edgeOf(7, 4)] = 0.75f;
    sharp[edgeOf(7, 4)] = 1;
    crease[edgeOf(4, 5)] = 0.5f; // at 4, which the weld merges away
    crease[edgeOf(6, 7)] = 0.125f;

    const geom::WeldResult weld = geom::WeldVertices(mesh, 1e-4f, false);
    REQUIRE(weld.verticesMerged == 2u);
    REQUIRE(mesh.faceCount() == 2u);
    CHECK(intact(mesh).empty());

    const Tables after = tablesOf(mesh);
    const auto valueOf = [&](const Vector3f& a, const Vector3f& b) {
        Point x = pointOf(a);
        Point y = pointOf(b);
        if (y < x) {
            std::swap(x, y);
        }
        const auto found = after.edges.find({x, y});
        REQUIRE(found != after.edges.end());
        // `crease` then `sharp`, by name.
        f32 value = 0.0f;
        std::memcpy(&value, found->second.data(), sizeof(f32));
        return std::pair<f32, u8>{value, found->second[sizeof(f32)]};
    };
    const auto closed = valueOf(Vector3f{1, 0, 0}, Vector3f{1, 1, 0});
    CHECK(closed.first == 0.75f);
    CHECK(closed.second == 1);
    CHECK(valueOf(Vector3f{1, 0, 0}, Vector3f{2, 0, 0.1f}).first == 0.5f);
    CHECK(valueOf(Vector3f{2, 1, 0.1f}, Vector3f{1, 1, 0}).first == 0.125f);
}

// ============================================================================
// The corpus arm (§2.7.11): the ops over shipped meshes
// ============================================================================

TEST_CASE("wem canonical the ops over corpus meshes stay clean and canonical",
          "[wem][geometry][canonical][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 40);
    const MdxConverter converter;
    u32 meshes = 0;
    u32 operations = 0;
    u32 dirty = 0;

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
            if (!mesh.ensureConnectivity().ok() || mesh.faceCount() < 4) {
                continue;
            }
            ++meshes;
            const u32 edges = std::as_const(mesh).topology().edgeCount();
            for (u32 e = 0; e < edges; e += 5) {
                operations += geom::DissolveEdge(mesh, EdgeId(e)) ? 1u : 0u;
            }
            const u32 faces = std::as_const(mesh).topology().faceCount();
            for (u32 f = 0; f < faces; f += 3) {
                const Topology& topology = std::as_const(mesh).topology();
                if (!topology.isDeleted(FaceId(f)) && topology.valence(FaceId(f)) >= 4) {
                    operations += geom::SplitFace(mesh, FaceId(f), 0, 2).valid() ? 1u : 0u;
                }
            }
            const u32 split = std::as_const(mesh).topology().edgeCount();
            for (u32 e = 1; e < split; e += 7) {
                if (!std::as_const(mesh).topology().isDeleted(EdgeId(e))) {
                    operations += geom::SplitEdge(mesh, EdgeId(e), 0.37f).valid() ? 1u : 0u;
                }
            }
            for (u32 e = 2; e < split; e += 11) {
                if (!std::as_const(mesh).topology().isDeleted(EdgeId(e))) {
                    operations += geom::FlipEdge(mesh, EdgeId(e)) ? 1u : 0u;
                }
            }
            const u32 halfedges = std::as_const(mesh).topology().halfedgeCount();
            for (u32 h = 3; h < halfedges; h += 13) {
                if (!std::as_const(mesh).topology().isDeleted(HalfedgeId(h)) &&
                    geom::IsCollapseLegal(mesh, HalfedgeId(h))) {
                    operations += geom::CollapseEdge(mesh, HalfedgeId(h)) ? 1u : 0u;
                }
            }
            dirty += geom::NumberingDirty(mesh) ? 1u : 0u;
            const Tables before = tablesOf(mesh);
            std::string broken = intact(mesh);
            geom::Canonicalize(mesh);
            if (broken.empty() && !geom::IsCanonical(mesh)) {
                broken = "not canonical after Canonicalize";
            }
            if (broken.empty()) {
                broken = intact(mesh);
            }
            if (broken.empty()) {
                const Tables after = tablesOf(throughWem(mesh));
                if (after.corners != before.corners || after.edges != before.edges) {
                    broken = "values moved across the .wem trip";
                }
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
    std::cout << "ops corpus arm: " << meshes << " meshes, " << operations << " operations, "
              << dirty << " renumbered\n";
    CHECK(meshes > 0u);
    CHECK(failing.empty());
}

TEST_CASE("wem canonical collapsing a valence-2 vertex into its neighbour leaves it a way out",
          "[wem][geometry][canonical]") {
    // Found by the corpus arm. Vertex 1 is an ear on the border of the square's
    // triangle (0,1,3); collapsing it into 3 splices that triangle away, and
    // both halfedges 1 brought go with it. Vertex 3's way out was the border
    // half of the collapsed edge, and nothing was left to replace it: 3 was
    // reachable and invisible to every circulation.
    Mesh mesh = grid(1, 1, 3);
    const Topology& topology = std::as_const(mesh).topology();
    const HalfedgeId h = topology.findHalfedge(VertexId(1), VertexId(3));
    REQUIRE(h.valid());
    REQUIRE(topology.outgoing(VertexId(3)) == Topology::opposite(h));
    REQUIRE(geom::IsCollapseLegal(mesh, h));
    REQUIRE(geom::CollapseEdge(mesh, h));
    CHECK(intact(mesh).empty());
    const HalfedgeId out = std::as_const(mesh).topology().outgoing(VertexId(3));
    REQUIRE(out.valid());
    CHECK_FALSE(std::as_const(mesh).topology().isDeleted(out));
}
