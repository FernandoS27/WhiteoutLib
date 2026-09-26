// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// What is wrong with a layout (EDIT_MODE_UV_REDESIGN.md §11,
/// EDIT_MODE_UV_REDESIGN_PLAN.md R3): one fixture per row, and the two things a
/// file does on purpose that are not problems -- a stack, and a whole island
/// mirrored in the map.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/uv/checks.h>
#include <whiteout/models/wem/geometry/uv/flatten.h>
#include <whiteout/models/wem/geometry/uv/islands.h>
#include <whiteout/models/wem/geometry/uv/layout.h>
#include <whiteout/models/wem/geometry/uv/seams.h>
#include <whiteout/models/wem/skinning/points.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
namespace uv = geom::uv;

/// One quad per entry, each its own island, a unit square in the world, with
/// the four UVs given (low-left, low-right, high-right, high-left).
Mesh quads(const std::vector<std::array<Vector2f, 4>>& list) {
    geom::FaceSet set;
    set.vertexCount = static_cast<u32>(list.size()) * 4;
    std::vector<Vector3f> positions;
    for (u32 i = 0; i < list.size(); ++i) {
        const f32 x = static_cast<f32>(i) * 10.0f;
        positions.push_back({x, 0, 0});
        positions.push_back({x + 1, 0, 0});
        positions.push_back({x + 1, 1, 0});
        positions.push_back({x, 1, 0});
        const u32 corners[4] = {i * 4, i * 4 + 1, i * 4 + 2, i * 4 + 3};
        set.addFace(std::span<const u32>(corners, 4));
    }
    Mesh mesh;
    mesh.setFaceSet(set);
    const std::span<Vector3f> at =
        mesh.attributes.getOrCreate<Vector3f>(geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    std::copy(positions.begin(), positions.end(), at.begin());
    REQUIRE(mesh.ensureConnectivity().ok());
    mesh.faceSections();
    mesh.sections.emplace_back();
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    const Topology& topology = std::as_const(mesh).topology();
    for (u32 i = 0; i < list.size(); ++i) {
        u32 k = 0;
        for (const HalfedgeId h : topology.fh(FaceId(i))) {
            uvs[h.index()] = list[i][k++];
        }
    }
    return mesh;
}

std::array<Vector2f, 4> square(f32 x, f32 y, f32 side) {
    return {Vector2f{x, y}, Vector2f{x + side, y}, Vector2f{x + side, y + side}, Vector2f{x, y + side}};
}

uv::UvChecks check(const Mesh& mesh, const uv::UvCheckOptions& options = {}) {
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const uv::UvCheckInput input{&mesh, &islands, 0};
    return uv::CheckUv(std::span<const uv::UvCheckInput>(&input, 1), options);
}

std::vector<u32> islandsOf(const std::vector<uv::UvIslandRef>& refs) {
    std::vector<u32> out;
    for (const uv::UvIslandRef& r : refs) {
        out.push_back(r.island);
    }
    return out;
}

/// A tube round z, open at both ends, of @p around by two cells.
Mesh tube(u32 around) {
    constexpr f32 kPi = 3.14159265358979323846f;
    geom::FaceSet set;
    set.vertexCount = around * 3;
    const auto at = [&](u32 a, u32 b) { return (a % around) * 3 + b; };
    for (u32 a = 0; a < around; ++a) {
        for (u32 b = 0; b < 2; ++b) {
            const u32 corners[4] = {at(a, b), at(a + 1, b), at(a + 1, b + 1), at(a, b + 1)};
            set.addFace(std::span<const u32>(corners, 4));
        }
    }
    Mesh mesh;
    mesh.setFaceSet(set);
    const std::span<Vector3f> positions =
        mesh.attributes.getOrCreate<Vector3f>(geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    for (u32 a = 0; a < around; ++a) {
        const f32 angle = 2.0f * kPi * static_cast<f32>(a) / static_cast<f32>(around);
        for (u32 b = 0; b <= 2; ++b) {
            positions[at(a, b)] = Vector3f{std::cos(angle), std::sin(angle), static_cast<f32>(b)};
        }
    }
    REQUIRE(mesh.ensureConnectivity().ok());
    mesh.faceSections();
    mesh.sections.emplace_back();
    uv::EnsureUvSet(mesh, 0);
    return mesh;
}

} // namespace

TEST_CASE("UV check: an overlap is named, and a stack is not one", "[wem][uv][checks]") {
    // Two overlapping squares, and two laid exactly on one another.
    const Mesh mesh = quads({square(0.1f, 0.1f, 0.3f), square(0.2f, 0.2f, 0.3f), square(0.6f, 0.6f, 0.3f),
                             square(0.6f, 0.6f, 0.3f)});
    const uv::UvChecks checks = check(mesh);
    CHECK(islandsOf(checks.overlapping) == std::vector<u32>{0, 1});
    REQUIRE(checks.overlapPairs.size() == 1u);
    CHECK(checks.overlapPairs[0].first == uv::UvIslandRef{0, 0});
    CHECK(checks.overlapPairs[0].second == uv::UvIslandRef{0, 1});
    CHECK(checks.stacks == 1u);
    CHECK(checks.Problems() == 2u);
}

TEST_CASE("UV check: the overlap's texels are the shared square, the stack's none", "[wem][uv][checks]") {
    const Mesh mesh = quads({square(0.1f, 0.1f, 0.3f), square(0.2f, 0.2f, 0.3f), square(0.6f, 0.6f, 0.3f),
                             square(0.6f, 0.6f, 0.3f)});
    uv::UvCheckOptions options;
    options.resolution = 100;
    const uv::UvChecks checks = check(mesh, options);
    // [0.2, 0.4] squared: 20 by 20 texels, give or take the raster's edge rule.
    CHECK(checks.overlapTexels.size() >= 19u * 19u);
    CHECK(checks.overlapTexels.size() <= 21u * 21u);
    CHECK(std::is_sorted(checks.overlapTexels.begin(), checks.overlapTexels.end()));
    for (const u32 texel : checks.overlapTexels) {
        const u32 x = texel % 100;
        const u32 y = texel / 100;
        CHECK((x >= 19 && x <= 40 && y >= 19 && y <= 40));
    }
}

TEST_CASE("UV check: a triangle turned over is flipped, a mirrored island is not", "[wem][uv][checks]") {
    std::array<Vector2f, 4> bowtie = square(0.1f, 0.1f, 0.3f);
    bowtie[3] = Vector2f{0.55f, 0.25f}; // the second triangle of the fan turns over
    std::array<Vector2f, 4> mirrored = square(0.5f, 0.5f, 0.3f);
    std::swap(mirrored[0], mirrored[1]);
    std::swap(mirrored[2], mirrored[3]);
    const uv::UvChecks checks = check(quads({bowtie, mirrored}));
    CHECK(islandsOf(checks.flipped) == std::vector<u32>{0});
}

TEST_CASE("UV check: outside the tile, unless the texture wraps", "[wem][uv][checks]") {
    const Mesh mesh = quads({square(0.1f, 0.1f, 0.3f), square(1.2f, 0.1f, 0.3f)});
    CHECK(islandsOf(check(mesh).outside) == std::vector<u32>{1});
    uv::UvCheckOptions wraps;
    wraps.wraps = true;
    CHECK(check(mesh, wraps).outside.empty());
}

TEST_CASE("UV check: no map, and nothing else said of it", "[wem][uv][checks]") {
    const uv::UvChecks checks = check(quads({square(0.1f, 0.1f, 0.3f), square(0.0f, 0.0f, 0.0f)}));
    CHECK(islandsOf(checks.noMap) == std::vector<u32>{1});
    CHECK(checks.flipped.empty());
    CHECK(checks.overlapping.empty());
}

TEST_CASE("UV check: stretched faces and density outliers", "[wem][uv][checks]") {
    std::array<Vector2f, 4> squashed = square(0.5f, 0.1f, 0.3f);
    squashed[2].y = squashed[3].y = 0.13f;
    const uv::UvChecks checks = check(
        quads({square(0.1f, 0.1f, 0.2f), square(0.1f, 0.5f, 0.2f), square(0.5f, 0.5f, 0.05f), squashed}));
    // A quad mapped ten times wider than tall is stretched against itself...
    CHECK(islandsOf(checks.stretched) == std::vector<u32>{3});
    // ...and a quarter of the median's size across is an outlier.
    const std::vector<u32> outliers = islandsOf(checks.density);
    CHECK(std::find(outliers.begin(), outliers.end(), 2u) != outliers.end());
    CHECK(std::find(outliers.begin(), outliers.end(), 0u) == outliers.end());
}

TEST_CASE("UV check: a tube flattens to a ring, and its ring seam opens it", "[wem][uv][checks][seams]") {
    Mesh mesh = tube(12);
    uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 1u);
    REQUIRE(islands.loops[0] == 2u);
    REQUIRE(uv::Lscm(mesh, islands, 0, 0).ok());
    CHECK(islandsOf(check(mesh).rings) == std::vector<u32>{0});

    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    uv::SeamPathOptions options;
    options.visible = Vector3f{1.0f, 0.0f, 0.0f};
    const uv::SeamPath path = uv::RingSeam(mesh, points, islands, 0, options);
    REQUIRE_FALSE(path.stopped);
    // From one end to the other, two cells long, on the side away from +X.
    CHECK(path.edges.size() == 2u);
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (const EdgeId edge : path.edges) {
        const HalfedgeId h = Topology::halfedge(edge, 0);
        CHECK(positions[mesh.topology().from(h).index()].x < 0.0f);
    }
    uv::ApplyMarks(mesh, 0, std::span<const EdgeId>(path.edges.data(), path.edges.size()), true);
    islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 1u);
    CHECK(islands.loops[0] == 1u);
}

TEST_CASE("UV check: two meshes on one image overlap each other", "[wem][uv][checks]") {
    const Mesh body = quads({square(0.1f, 0.1f, 0.4f)});
    const Mesh cape = quads({square(0.3f, 0.3f, 0.4f)});
    const uv::UvIslands a = uv::BuildUvIslands(body, 0);
    const uv::UvIslands b = uv::BuildUvIslands(cape, 0);
    const uv::UvCheckInput inputs[] = {{&body, &a, 0}, {&cape, &b, 0}};
    const uv::UvChecks checks = uv::CheckUv(inputs);
    REQUIRE(checks.overlapping.size() == 2u);
    CHECK(checks.overlapping[0] == uv::UvIslandRef{0, 0});
    CHECK(checks.overlapping[1] == uv::UvIslandRef{1, 0});
}

// ============================================================================
// The corpus
// ============================================================================

TEST_CASE("UV check: the MDX corpus's own layouts", "[wem][uv][checks][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 60);
    u32 meshes = 0;
    u32 islandCount = 0;
    u32 stacks = 0;
    u32 overlapping = 0;
    u32 flipped = 0;
    u32 mirrored = 0;
    std::vector<f64> times;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        for (test::IngestedMesh& ingested : test::IngestMdx(std::span<const u8>(bytes.data(), bytes.size()))) {
            Mesh mesh = std::move(ingested.mesh);
            geom::PrepareForModelling(mesh);
            if (!mesh.hasConnectivity() || mesh.faceCount() == 0) {
                continue;
            }
            uv::EnsureUvSet(mesh, 0);
            const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
            const uv::UvCheckInput input{&mesh, &islands, 0};
            const auto started = std::chrono::steady_clock::now();
            const uv::UvChecks checks = uv::CheckUv(std::span<const uv::UvCheckInput>(&input, 1));
            times.push_back(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count());
            ++meshes;
            islandCount += islands.count;
            stacks += checks.stacks;
            overlapping += static_cast<u32>(checks.overlapping.size());
            flipped += static_cast<u32>(checks.flipped.size());
            // Whole islands laid mirrored in the map, which the file means.
            const std::span<const Vector2f> uvs =
                mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
            for (u32 island = 0; island < islands.count; ++island) {
                f32 area = 0.0f;
                for (const u32 face : islands.facesOf(island)) {
                    std::vector<Vector2f> ring;
                    for (const HalfedgeId h : mesh.topology().fh(FaceId(face))) {
                        ring.push_back(uvs[h.index()]);
                    }
                    for (std::size_t k = 0; k < ring.size(); ++k) {
                        area += ring[k].x * ring[(k + 1) % ring.size()].y - ring[(k + 1) % ring.size()].x * ring[k].y;
                    }
                }
                mirrored += area < 0.0f ? 1u : 0u;
            }
            // A stack is never an overlap: every island a stack holds is
            // absent from the row unless something else lies on it.
            for (const std::vector<u32>& stack : uv::FindStacks(mesh, islands, 0)) {
                for (const uv::UvIslandRef& ref : checks.overlapping) {
                    const bool member = std::find(stack.begin(), stack.end(), ref.island) != stack.end();
                    // Only an overlap outside the stack may name a member.
                    if (member) {
                        bool other = false;
                        for (const uv::UvIslandRef& o : checks.overlapping) {
                            other = other || std::find(stack.begin(), stack.end(), o.island) == stack.end();
                        }
                        CHECK(other);
                    }
                }
            }
        }
    }
    if (times.empty()) {
        SKIP("no mesh survived the ingest");
    }
    std::sort(times.begin(), times.end());
    std::cout << "UV check corpus: " << meshes << " meshes, " << islandCount << " islands, " << stacks
              << " stacks, " << overlapping << " overlapping, " << flipped << " flipped (" << mirrored
              << " mirrored whole), median " << times[times.size() / 2] << " ms, slowest " << times.back()
              << " ms\n";
    CHECK(times[times.size() / 2] < 5.0);
}
