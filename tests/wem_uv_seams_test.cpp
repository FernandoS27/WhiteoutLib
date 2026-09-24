// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G-U4: where a cut wants to go (EDIT_MODE_UV_DESIGN.md §5;
/// EDIT_MODE_UV_PLAN.md §9). A cut is a thing a modeller sees, so every claim
/// here is about where it went and which way it faced.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/uv/islands.h>
#include <whiteout/models/wem/geometry/uv/seams.h>
#include <whiteout/models/wem/rigging/detect.h>
#include <whiteout/models/wem/skinning/mirror.h>
#include <whiteout/models/wem/skinning/points.h>

#include <algorithm>
#include <cmath>
#include <iostream>
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
namespace uv = geom::uv;

/// A closed ring of quads round the z axis, of `around` by `along` cells: a
/// sleeve, which is what a limb is and what a cut has to open.
Mesh sleeve(u32 around, u32 along, f32 radius) {
    constexpr f32 kPi = 3.14159265358979323846f;
    geom::FaceSet set;
    set.vertexCount = around * (along + 1);
    const auto at = [&](u32 a, u32 b) { return (a % around) * (along + 1) + b; };
    for (u32 a = 0; a < around; ++a) {
        for (u32 b = 0; b < along; ++b) {
            const u32 corners[4] = {at(a, b), at(a + 1, b), at(a + 1, b + 1), at(a, b + 1)};
            set.addFace(std::span<const u32>(corners, 4));
        }
    }
    Mesh mesh;
    mesh.setFaceSet(set);
    const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    for (u32 a = 0; a < around; ++a) {
        const f32 angle = 2.0f * kPi * static_cast<f32>(a) / static_cast<f32>(around);
        for (u32 b = 0; b <= along; ++b) {
            positions[at(a, b)] = Vector3f{radius * std::cos(angle), radius * std::sin(angle),
                                           static_cast<f32>(b)};
        }
    }
    REQUIRE(mesh.ensureConnectivity().ok());
    mesh.faceSections();
    mesh.sections.emplace_back();
    mesh.attributes.getOrCreate<Vector2f>(geom::names::uv(0), Domain::Halfedge,
                                          geom::AttrType::F32x2);
    mesh.recomputeBounds();
    return mesh;
}

/// Two quads that share no edge: two shells, so nothing joins them.
Mesh twoShells() {
    geom::FaceSet set;
    set.vertexCount = 8;
    const u32 a[4] = {0, 1, 2, 3};
    const u32 b[4] = {4, 5, 6, 7};
    set.addFace(std::span<const u32>(a, 4));
    set.addFace(std::span<const u32>(b, 4));
    Mesh mesh;
    mesh.setFaceSet(set);
    const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    const Vector3f places[8] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                {5, 0, 0}, {6, 0, 0}, {6, 1, 0}, {5, 1, 0}};
    std::copy(std::begin(places), std::end(places), positions.begin());
    REQUIRE(mesh.ensureConnectivity().ok());
    mesh.faceSections();
    mesh.sections.emplace_back();
    mesh.recomputeBounds();
    return mesh;
}

u32 markedCount(const Mesh& mesh, u32 set) {
    const std::span<const u8> marks =
        mesh.attributes.get<const u8>(geom::names::uvSeam(set), Domain::Edge);
    u32 count = 0;
    for (const u8 value : marks) {
        count += value;
    }
    return count;
}

} // namespace

TEST_CASE("UV seam path: of two ways round, it takes the hidden one",
          "[wem][uv][seams]") {
    // A sleeve round z, and two edges half a turn apart: the two ways round are
    // the same length, so what decides between them is which one faces away.
    Mesh mesh = sleeve(16, 2, 2.0f);
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);

    const auto columnEdge = [&](u32 column) {
        const HalfedgeId h =
            topology.findHalfedge(VertexId(column * 3), VertexId(column * 3 + 1));
        REQUIRE(h.valid());
        return Topology::edge(h);
    };
    const EdgeId from = columnEdge(0);  // at +x
    const EdgeId to = columnEdge(8);    // at -x

    // The mean y of a path: which side of the sleeve it went round.
    const auto meanY = [&](const uv::SeamPath& path) {
        f32 sum = 0.0f;
        for (const EdgeId edge : path.edges) {
            const HalfedgeId h = Topology::halfedge(edge, 0);
            sum += 0.5f * (positions[topology.from(h).index()].y +
                           positions[topology.to(h).index()].y);
        }
        return path.edges.empty() ? 0.0f : sum / static_cast<f32>(path.edges.size());
    };

    uv::SeamPathOptions options;
    options.visible = Vector3f{0.0f, 1.0f, 0.0f};
    const uv::SeamPath away = uv::FindSeamPath(mesh, points, from, to, options);
    REQUIRE_FALSE(away.stopped);
    REQUIRE_FALSE(away.edges.empty());
    CHECK(meanY(away) < 0.0f);

    options.visible = Vector3f{0.0f, -1.0f, 0.0f};
    const uv::SeamPath back = uv::FindSeamPath(mesh, points, from, to, options);
    REQUIRE_FALSE(back.stopped);
    CHECK(meanY(back) > 0.0f);
    // The same number of edges either way: it is the facing that chose, not
    // the distance.
    CHECK(away.edges.size() == back.edges.size());
}

TEST_CASE("UV seam path: nothing joins two shells, and it says so",
          "[wem][uv][seams]") {
    Mesh mesh = twoShells();
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const Topology& topology = mesh.topology();
    const EdgeId from = Topology::edge(topology.findHalfedge(VertexId(0), VertexId(1)));
    const EdgeId to = Topology::edge(topology.findHalfedge(VertexId(4), VertexId(5)));
    const uv::SeamPath path = uv::FindSeamPath(mesh, points, from, to);
    CHECK(path.stopped);
    CHECK(path.edges.empty());
}

TEST_CASE("UV mirror marks: a cut on one side is a cut on the other",
          "[wem][uv][seams]") {
    // A sleeve is symmetric about the plane x = 0 by construction with an even
    // number of columns, so the twin of every vertex is the one across it.
    Mesh mesh = sleeve(16, 2, 2.0f);
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    std::vector<u32> mirror(mesh.vertexCount(), geom::kInvalidId);
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        for (u32 w = 0; w < mesh.vertexCount(); ++w) {
            if (std::abs(positions[v].x - positions[w].x) < 1e-4f &&
                std::abs(positions[v].y + positions[w].y) < 1e-4f &&
                std::abs(positions[v].z - positions[w].z) < 1e-4f) {
                mirror[v] = w;
                break;
            }
        }
    }

    const Topology& topology = mesh.topology();
    // One column of edges on the +y side.
    std::vector<EdgeId> cut;
    for (u32 b = 0; b < 2; ++b) {
        const HalfedgeId h = topology.findHalfedge(VertexId(4 * 3 + b), VertexId(4 * 3 + b + 1));
        REQUIRE(h.valid());
        cut.push_back(Topology::edge(h));
    }
    uv::ApplyMarks(mesh, 0, std::span<const EdgeId>(cut.data(), cut.size()), true);
    REQUIRE(markedCount(mesh, 0) == 2u);

    const u32 added = uv::MirrorMarks(mesh, 0, std::span<const u32>(mirror.data(), mirror.size()));
    CHECK(added == 2u);
    CHECK(markedCount(mesh, 0) == 4u);

    // Mirror-exact: every marked edge's twin is marked too.
    const std::span<const u8> marks =
        mesh.attributes.get<const u8>(geom::names::uvSeam(0), Domain::Edge);
    for (u32 e = 0; e < marks.size(); ++e) {
        if (marks[e] == 0) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const u32 twinA = mirror[topology.from(h).value()];
        const u32 twinB = mirror[topology.to(h).value()];
        REQUIRE(twinA != geom::kInvalidId);
        REQUIRE(twinB != geom::kInvalidId);
        const HalfedgeId twin = topology.findHalfedge(VertexId(twinA), VertexId(twinB));
        REQUIRE(twin.valid());
        CHECK(marks[Topology::edge(twin).index()] == 1u);
    }
    // Asking twice adds nothing.
    CHECK(uv::MirrorMarks(mesh, 0, std::span<const u32>(mirror.data(), mirror.size())) == 0u);
}

TEST_CASE("UV seams from the rig: no limb in the record is a refusal",
          "[wem][uv][seams]") {
    Model model;
    model.name = "unrigged";
    model.meshes.push_back(sleeve(8, 2, 1.0f));
    Mesh view = model.meshes[0];
    const uv::SkeletonSeams result = uv::SeamsFromSkeleton(model, 0, view, 0, {});
    CHECK_FALSE(result.ok());
    CHECK(result.refusal == uv::SkeletonSeams::Refusal::NoLimbs);
    CHECK(markedCount(view, 0) == 0u);
}

TEST_CASE("UV seams from the rig: two parts are cut apart and opened",
          "[wem][uv][seams]") {
    // A sleeve bound half to one joint and half to another, with the record
    // calling them an upper and a lower: one arm, in two parts.
    Model model;
    model.name = "arm";
    Mesh mesh = sleeve(12, 4, 1.0f);
    mesh.skin.reset(0);
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        geom::Influence influence;
        influence.bone = positions[v].z < 2.0f ? 0u : 1u;
        influence.weight = 1.0f;
        mesh.skin.appendVertex(std::span<const geom::Influence>(&influence, 1));
    }
    model.meshes.push_back(std::move(mesh));

    Node upper;
    upper.name = "upper";
    upper.rig.role = RigRole::Upper;
    Node lower;
    lower.name = "lower";
    lower.rig.role = RigRole::Lower;
    model.nodes.nodes.push_back(std::move(upper));
    model.nodes.nodes.push_back(std::move(lower));
    model.nodes.nodes[1].parent = 0;

    Mesh view = model.meshes[0];
    const uv::SkeletonSeams result = uv::SeamsFromSkeleton(model, 0, view, 0, {});
    REQUIRE(result.ok());
    CHECK(result.parts == 2u);
    CHECK(result.cuts > 0u);

    // The ring between the two parts is cut, and each sleeve is opened along
    // its length: what was one closed island is now two that can be laid flat.
    const uv::UvIslands islands = uv::BuildUvIslands(view, 0);
    CHECK(islands.count == 2u);
    for (u32 island = 0; island < islands.count; ++island) {
        CHECK(islands.closed[island] == 0u);
    }
}

// ============================================================================
// The corpus (EDIT_MODE_UV_PLAN.md §9)
// ============================================================================

TEST_CASE("UV seams from the rig: the MDX corpus's figures", "[wem][uv][seams][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 60);

    u32 rigged = 0;
    u32 refused = 0;
    u32 withinSixteen = 0;
    f64 hidden = 0.0;
    u32 measured = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        mdx::Parser parser;
        const mdx::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        if (source.geosets.empty() || source.bones.empty()) {
            continue;
        }
        const MdxConverter converter;
        Result<Document> imported = converter.fromMdx(source);
        if (!imported.ok() || imported->models.empty()) {
            continue;
        }
        Model& model = imported->models.front();
        EnsureRig(model);
        for (u32 m = 0; m < model.meshes.size() && m < 4; ++m) {
            Mesh view = model.meshes[m];
            if (view.faceCount() == 0 || view.faceCount() > 4000) {
                continue;
            }
            geom::PrepareForModelling(view);
            if (!view.hasConnectivity()) {
                continue;
            }
            uv::EnsureUvSet(view, 0);
            const uv::SkeletonSeams result = uv::SeamsFromSkeleton(model, m, view, 0, {});
            if (!result.ok()) {
                ++refused;
                continue;
            }
            ++rigged;
            withinSixteen += result.parts <= 16 ? 1u : 0u;
            if (result.cuts > 0) {
                hidden += result.hiddenShare;
                ++measured;
            }
        }
    }
    if (rigged == 0) {
        SKIP("no rigged mesh survived the ingest");
    }
    std::cout << "UV seams corpus: " << rigged << " rigged meshes, " << refused
              << " without a limb, " << (100.0 * withinSixteen / rigged)
              << "% in sixteen parts or fewer, mean hidden share "
              << (measured > 0 ? 100.0 * hidden / measured : 0.0) << "%\n";
    CHECK(withinSixteen * 100 >= rigged * 90);
}
