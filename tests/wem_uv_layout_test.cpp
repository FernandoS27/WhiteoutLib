// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G-U3: the tile (EDIT_MODE_UV_DESIGN.md §7, §8; EDIT_MODE_UV_PLAN.md §8).
/// What the file placed is locked, so every claim here is as much about what
/// did not move as about what did.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/uv/islands.h>
#include <whiteout/models/wem/geometry/uv/layout.h>

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
using geom::FaceId;
using geom::HalfedgeId;
using geom::Topology;
using geom::VertexId;
namespace uv = geom::uv;

struct Patch {
    Vector2f low{0.0f, 0.0f};
    Vector2f high{1.0f, 1.0f};
    f32 world = 1.0f; ///< The quad's side in world units.
    bool free = true;
};

/// One quad per patch, each its own island (they share no edge), each with the
/// UVs the patch says and the world size the patch says.
Mesh patches(const std::vector<Patch>& list) {
    geom::FaceSet set;
    set.vertexCount = static_cast<u32>(list.size()) * 4;
    std::vector<Vector3f> positions;
    for (u32 i = 0; i < list.size(); ++i) {
        const f32 offset = static_cast<f32>(i) * 10.0f;
        const f32 side = list[i].world;
        positions.push_back(Vector3f{offset, 0.0f, 0.0f});
        positions.push_back(Vector3f{offset + side, 0.0f, 0.0f});
        positions.push_back(Vector3f{offset + side, side, 0.0f});
        positions.push_back(Vector3f{offset, side, 0.0f});
        const u32 corners[4] = {i * 4, i * 4 + 1, i * 4 + 2, i * 4 + 3};
        set.addFace(std::span<const u32>(corners, 4));
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
    const std::span<u8> free = mesh.attributes.getOrCreate<u8>(
        geom::names::uvFree(0), Domain::Face, geom::AttrType::Bool);
    mesh.attributes.getOrCreate<u8>(geom::names::uvPin(0), Domain::Halfedge, geom::AttrType::Bool);
    const Topology& topology = std::as_const(mesh).topology();
    for (u32 i = 0; i < list.size(); ++i) {
        free[i] = list[i].free ? 1 : 0;
        const Vector2f corner[4] = {list[i].low,
                                    Vector2f{list[i].high.x, list[i].low.y},
                                    list[i].high,
                                    Vector2f{list[i].low.x, list[i].high.y}};
        u32 k = 0;
        for (const HalfedgeId h : topology.fh(FaceId(i))) {
            uvs[h.index()] = corner[k++];
        }
    }
    mesh.recomputeBounds();
    return mesh;
}

/// Every island rasterised into one grid, one owner per texel: what overlaps
/// what, told the way a texture would tell it. Triangles, not boxes -- two
/// islands whose boxes cross need not share a texel.
void rasterise(const Mesh& mesh, const uv::UvIslands& islands, u32 set, u32 resolution,
               u32& overlaps, u32& covered, std::span<const u32> groupOf = {}) {
    std::vector<u32> owner(static_cast<std::size_t>(resolution) * resolution, geom::kInvalidId);
    overlaps = 0;
    covered = 0;
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(set), Domain::Halfedge);
    const Topology& topology = mesh.topology();
    for (u32 island = 0; island < islands.count; ++island) {
        for (const u32 face : islands.facesOf(island)) {
            // Fanned from the first corner, which is how every face here is
            // drawn and written.
            std::vector<Vector2f> loop;
            for (const HalfedgeId h : topology.fh(FaceId(face))) {
                loop.push_back(uvs[h.index()]);
            }
            for (std::size_t k = 1; k + 1 < loop.size(); ++k) {
                const Vector2f a = loop[0];
                const Vector2f b = loop[k];
                const Vector2f c = loop[k + 1];
                const f32 area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                if (area == 0.0f) {
                    continue;
                }
                const f32 inverse = 1.0f / area;
                const i32 x0 = std::max(0, static_cast<i32>(std::floor(
                                               std::min(a.x, std::min(b.x, c.x)) * resolution)));
                const i32 x1 = std::min(static_cast<i32>(resolution) - 1,
                                        static_cast<i32>(std::ceil(
                                            std::max(a.x, std::max(b.x, c.x)) * resolution)));
                const i32 y0 = std::max(0, static_cast<i32>(std::floor(
                                               std::min(a.y, std::min(b.y, c.y)) * resolution)));
                const i32 y1 = std::min(static_cast<i32>(resolution) - 1,
                                        static_cast<i32>(std::ceil(
                                            std::max(a.y, std::max(b.y, c.y)) * resolution)));
                for (i32 y = y0; y <= y1; ++y) {
                    for (i32 x = x0; x <= x1; ++x) {
                        const f32 px = (static_cast<f32>(x) + 0.5f) / resolution;
                        const f32 py = (static_cast<f32>(y) + 0.5f) / resolution;
                        const f32 w0 =
                            ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * inverse;
                        const f32 w1 =
                            ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * inverse;
                        const f32 w2 =
                            ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * inverse;
                        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                            continue;
                        }
                        u32& slot = owner[static_cast<std::size_t>(y) * resolution +
                                          static_cast<u32>(x)];
                        if (slot == geom::kInvalidId) {
                            ++covered;
                        } else if (slot != island) {
                            // Two islands the file stacked on purpose share
                            // their texels; that is what a stack is for.
                            const bool stacked =
                                !groupOf.empty() && slot < groupOf.size() &&
                                island < groupOf.size() && groupOf[slot] == groupOf[island];
                            if (!stacked) {
                                ++overlaps;
                            }
                        }
                        slot = island;
                    }
                }
            }
        }
    }
}

std::vector<Vector2f> uvBytes(const Mesh& mesh) {
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    return std::vector<Vector2f>(uvs.begin(), uvs.end());
}

} // namespace

TEST_CASE("UV pack: the free ones move and the locked one does not",
          "[wem][uv][layout]") {
    // Three free islands and one the file placed in the middle of the tile.
    Mesh mesh = patches({
        Patch{{0.4f, 0.4f}, {0.6f, 0.6f}, 1.0f, false},
        Patch{{2.0f, 0.0f}, {2.3f, 0.3f}, 1.0f, true},
        Patch{{2.0f, 0.4f}, {2.25f, 0.65f}, 1.0f, true},
        Patch{{2.0f, 1.0f}, {2.15f, 1.15f}, 1.0f, true},
    });
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 4u);
    const uv::UvBounds lockedBefore = uv::BoundsOf(mesh, islands, 0, 0);

    uv::PackOptions options;
    options.resolution = 256;
    options.padding = 2;
    const uv::PackResult result = uv::Pack(mesh, islands, 0, options);
    CHECK_FALSE(result.refusedTiling);
    CHECK(result.placed == 3u);
    // They fitted as they were: the locked island in the middle of the tile
    // leaves a column of free space on either side of it, and the three go in
    // beside it.
    CHECK(result.scaled == 1.0f);

    // The locked island is where it was, to the bit.
    const uv::UvBounds lockedAfter = uv::BoundsOf(mesh, islands, 0, 0);
    CHECK(std::memcmp(&lockedBefore.low, &lockedAfter.low, sizeof(Vector2f)) == 0);
    CHECK(std::memcmp(&lockedBefore.high, &lockedAfter.high, sizeof(Vector2f)) == 0);

    // Everything is in the tile, and nothing is on anything else.
    for (u32 island = 1; island < islands.count; ++island) {
        const uv::UvBounds bounds = uv::BoundsOf(mesh, islands, island, 0);
        CHECK(bounds.low.x >= -0.001f);
        CHECK(bounds.low.y >= -0.001f);
        CHECK(bounds.high.x <= 1.001f);
        CHECK(bounds.high.y <= 1.001f);
    }
    u32 overlaps = 0;
    u32 covered = 0;
    rasterise(mesh, islands, 0, options.resolution, overlaps, covered);
    CHECK(overlaps == 0u);

    // And the padding is really there: no two islands come within `padding`
    // texels of one another, which is what keeps a mip from bleeding one into
    // the next.
    f32 closest = 1e30f;
    for (u32 a = 0; a < islands.count; ++a) {
        const uv::UvBounds boxA = uv::BoundsOf(mesh, islands, a, 0);
        for (u32 b = a + 1; b < islands.count; ++b) {
            const uv::UvBounds boxB = uv::BoundsOf(mesh, islands, b, 0);
            const f32 gapX = std::max(boxA.low.x - boxB.high.x, boxB.low.x - boxA.high.x);
            const f32 gapY = std::max(boxA.low.y - boxB.high.y, boxB.low.y - boxA.high.y);
            closest = std::min(closest, std::max(gapX, gapY));
        }
    }
    CHECK(closest * static_cast<f32>(options.resolution) >= static_cast<f32>(options.padding));
}

TEST_CASE("UV pack: a tiling locked island is refused", "[wem][uv][layout]") {
    Mesh mesh = patches({
        Patch{{0.0f, 0.0f}, {3.0f, 3.0f}, 1.0f, false}, // tiles on purpose
        Patch{{5.0f, 0.0f}, {5.2f, 0.2f}, 1.0f, true},
    });
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const std::vector<Vector2f> before = uvBytes(mesh);
    uv::PackOptions options;
    options.resolution = 128;
    const uv::PackResult result = uv::Pack(mesh, islands, 0, options);
    CHECK(result.refusedTiling);
    CHECK(result.placed == 0u);
    // Refused means untouched, the free island included.
    CHECK(uvBytes(mesh) == before);
}

TEST_CASE("UV pack: too much to fit is scaled down together", "[wem][uv][layout]") {
    std::vector<Patch> list;
    for (u32 i = 0; i < 6; ++i) {
        list.push_back(Patch{{2.0f + static_cast<f32>(i), 0.0f},
                             {2.6f + static_cast<f32>(i), 0.6f},
                             1.0f,
                             true});
    }
    Mesh mesh = patches(list);
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    uv::PackOptions options;
    options.resolution = 128;
    options.padding = 3;
    const uv::PackResult result = uv::Pack(mesh, islands, 0, options);
    CHECK(result.placed == 6u);
    CHECK(result.scaled < 1.0f);
    CHECK(result.scaled > 0.0f);
    u32 overlaps = 0;
    u32 covered = 0;
    rasterise(mesh, islands, 0, options.resolution, overlaps, covered);
    CHECK(overlaps == 0u);

    // Packed as tight as it goes, so the padding is what stands between them:
    // no two islands come within `padding` texels of one another, which is what
    // keeps a mip from bleeding one into the next.
    f32 closest = 1e30f;
    for (u32 a = 0; a < islands.count; ++a) {
        const uv::UvBounds boxA = uv::BoundsOf(mesh, islands, a, 0);
        for (u32 b = a + 1; b < islands.count; ++b) {
            const uv::UvBounds boxB = uv::BoundsOf(mesh, islands, b, 0);
            const f32 gapX = std::max(boxA.low.x - boxB.high.x, boxB.low.x - boxA.high.x);
            const f32 gapY = std::max(boxA.low.y - boxB.high.y, boxB.low.y - boxA.high.y);
            closest = std::min(closest, std::max(gapX, gapY));
        }
    }
    CHECK(closest * static_cast<f32>(options.resolution) >=
          static_cast<f32>(options.padding));
}

TEST_CASE("UV pack: a stack moves as one thing", "[wem][uv][layout]") {
    // Two islands laid on one another, which is what a file does when two parts
    // are meant to share a patch of texture.
    Mesh mesh = patches({
        Patch{{2.0f, 0.0f}, {2.4f, 0.4f}, 1.0f, true},
        Patch{{2.0f, 0.0f}, {2.4f, 0.4f}, 1.0f, true},
        Patch{{3.0f, 0.0f}, {3.2f, 0.2f}, 1.0f, true},
    });
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const std::vector<std::vector<u32>> stacks = uv::FindStacks(mesh, islands, 0);
    REQUIRE(stacks.size() == 1u);
    CHECK(stacks[0].size() == 2u);

    uv::PackOptions options;
    options.resolution = 256;
    const uv::PackResult result = uv::Pack(mesh, islands, 0, options);
    // Two groups: the stack and the third island.
    CHECK(result.placed == 2u);
    // And the stack is still a stack afterwards.
    const std::vector<std::vector<u32>> after = uv::FindStacks(mesh, islands, 0);
    REQUIRE(after.size() == 1u);
    CHECK(after[0] == stacks[0]);
}

TEST_CASE("UV stack: a mirrored pair is laid corner for corner", "[wem][uv][layout]") {
    // Two quads, the second the first mirrored in x: what a left and a right
    // arm are to the point mirror.
    Mesh mesh = patches({
        Patch{{0.0f, 0.0f}, {0.5f, 0.5f}, 1.0f, true},
        Patch{{0.6f, 0.6f}, {0.9f, 0.9f}, 1.0f, true},
    });
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 2u);

    // The twin of every vertex, by hand: the fixture knows its own pairing.
    std::vector<u32> mirror(mesh.vertexCount(), geom::kInvalidId);
    for (u32 v = 0; v < 4; ++v) {
        mirror[v] = v + 4;
        mirror[v + 4] = v;
    }
    const uv::StackResult result =
        uv::Stack(mesh, islands, 0, 1, 0, std::span<const u32>(mirror.data(), mirror.size()));
    CHECK(result.ok());
    CHECK(result.byMirror);

    // Corner for corner: the second island now holds the first's UVs exactly.
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    const Topology& topology = mesh.topology();
    for (const HalfedgeId h : topology.fh(FaceId(1))) {
        const u32 twin = mirror[topology.from(h).value()];
        bool found = false;
        for (const HalfedgeId other : topology.fh(FaceId(0))) {
            if (topology.from(other).value() == twin) {
                found = std::memcmp(&uvs[h.index()], &uvs[other.index()], sizeof(Vector2f)) == 0;
                break;
            }
        }
        CHECK(found);
    }
    // And the two are one stack now.
    CHECK(uv::FindStacks(mesh, islands, 0).size() == 1u);
}

TEST_CASE("UV stack: with no mirror the two surfaces are walked", "[wem][uv][layout]") {
    Mesh mesh = patches({
        Patch{{0.0f, 0.0f}, {0.5f, 0.5f}, 1.0f, true},
        Patch{{0.6f, 0.6f}, {0.9f, 0.9f}, 1.0f, true},
    });
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const uv::StackResult result = uv::Stack(mesh, islands, 0, 1, 0, {});
    CHECK(result.ok());
    CHECK_FALSE(result.byMirror);
    CHECK(uv::FindStacks(mesh, islands, 0).size() == 1u);
}

TEST_CASE("UV stack: two shapes that do not pair are refused whole",
          "[wem][uv][layout]") {
    // A quad against two quads: no walk pairs them, and half a stack is worse
    // than none.
    geom::FaceSet set;
    set.vertexCount = 10;
    std::vector<Vector3f> positions = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},               // one quad
        {5, 0, 0}, {6, 0, 0}, {6, 1, 0}, {5, 1, 0}, {7, 0, 0}, {7, 1, 0}, // two
    };
    const u32 a[4] = {0, 1, 2, 3};
    const u32 b[4] = {4, 5, 6, 7};
    const u32 c[4] = {5, 8, 9, 6};
    set.addFace(std::span<const u32>(a, 4));
    set.addFace(std::span<const u32>(b, 4));
    set.addFace(std::span<const u32>(c, 4));
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
    for (u32 face = 0; face < 3; ++face) {
        for (const HalfedgeId h : topology.fh(FaceId(face))) {
            const Vector3f& p = out[topology.from(h).index()];
            uvs[h.index()] = Vector2f{p.x * 0.1f, p.y * 0.1f};
        }
    }
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    REQUIRE(islands.count == 2u);
    const std::vector<Vector2f> before = uvBytes(mesh);
    const u32 single = islands.islandOf(0);
    const u32 pair = islands.islandOf(1);
    const uv::StackResult result = uv::Stack(mesh, islands, single, pair, 0, {});
    CHECK_FALSE(result.ok());
    CHECK(result.primaryFaces == 1u);
    CHECK(result.otherFaces == 2u);
    CHECK(result.refusedFaces > 0u);
    CHECK(uvBytes(mesh) == before);
}

TEST_CASE("UV density: matching brings two islands to one scale",
          "[wem][uv][layout]") {
    // Two quads the same size in the world, laid out at different sizes in the
    // map: one reads twice as sharp as the other until they are matched.
    Mesh mesh = patches({
        Patch{{0.0f, 0.0f}, {0.4f, 0.4f}, 1.0f, true},
        Patch{{2.0f, 0.0f}, {2.2f, 0.2f}, 1.0f, true},
    });
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
    const f32 first = uv::TexelDensity(mesh, islands, 0, 0, 1024);
    const f32 second = uv::TexelDensity(mesh, islands, 1, 0, 1024);
    CHECK(std::abs(first / second - 2.0f) < 1e-3f);

    const u32 both[2] = {0, 1};
    uv::MatchDensity(mesh, islands, std::span<const u32>(both, 2), 0, first, 1024);
    CHECK(std::abs(uv::TexelDensity(mesh, islands, 0, 0, 1024) - first) < 1e-2f);
    CHECK(std::abs(uv::TexelDensity(mesh, islands, 1, 0, 1024) - first) < 1e-2f);
}

// ============================================================================
// The corpus (EDIT_MODE_UV_PLAN.md §8)
// ============================================================================

TEST_CASE("UV pack: the MDX corpus, freed and packed", "[wem][uv][layout][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 60);

    u32 meshes = 0;
    u32 improved = 0;
    u32 overlapping = 0;
    u32 stacksKept = 0;
    u32 stacksBefore = 0;
    std::vector<f64> times;
    f64 coverage = 0.0;
    u32 reported = 0;
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
            if (mesh.faceCount() == 0 || mesh.faceCount() > 4000) {
                continue;
            }
            geom::PrepareForModelling(mesh);
            if (!mesh.hasConnectivity()) {
                continue;
            }
            uv::EnsureUvSet(mesh, 0);
            const uv::UvIslands islands = uv::BuildUvIslands(mesh, 0);
            if (islands.count == 0 || islands.count > 200) {
                continue;
            }
            const std::vector<std::vector<u32>> before = uv::FindStacks(mesh, islands, 0);
            f32 shipped = 0.0f;
            for (u32 island = 0; island < islands.count; ++island) {
                const uv::UvBounds bounds = uv::BoundsOf(mesh, islands, island, 0);
                if (bounds.empty || bounds.low.x < -0.01f || bounds.high.x > 1.01f ||
                    bounds.low.y < -0.01f || bounds.high.y > 1.01f) {
                    shipped = -1.0f;
                    break;
                }
                shipped += bounds.width() * bounds.height();
            }
            if (shipped < 0.0f) {
                continue; // a tiling layout: not a packing question
            }
            // Free every island and lay the whole thing out again.
            const std::span<u8> free = mesh.attributes.getOrCreate<u8>(
                geom::names::uvFree(0), Domain::Face, geom::AttrType::Bool);
            std::fill(free.begin(), free.end(), static_cast<u8>(1));

            uv::PackOptions options;
            options.resolution = 256;
            options.padding = 2;
            const auto started = std::chrono::steady_clock::now();
            const uv::PackResult result = uv::Pack(mesh, islands, 0, options);
            times.push_back(std::chrono::duration<f64, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count());
            if (result.placed == 0) {
                continue;
            }
            ++meshes;
            u32 overlaps = 0;
            u32 covered = 0;
            std::vector<u32> groupOf(islands.count, geom::kInvalidId);
            {
                u32 g = 0;
                for (const std::vector<u32>& group : uv::FindStacks(mesh, islands, 0)) {
                    for (const u32 island : group) {
                        groupOf[island] = g;
                    }
                    ++g;
                }
            }
            rasterise(mesh, islands, 0, options.resolution, overlaps, covered,
                      std::span<const u32>(groupOf.data(), groupOf.size()));
            overlapping += overlaps > 0 ? 1u : 0u;
            coverage += static_cast<f64>(covered) /
                        (static_cast<f64>(options.resolution) * options.resolution);
            const std::vector<std::vector<u32>> after = uv::FindStacks(mesh, islands, 0);
            std::vector<u32> groupAfter(islands.count, geom::kInvalidId);
            for (u32 g = 0; g < after.size(); ++g) {
                for (const u32 island : after[g]) {
                    groupAfter[island] = g;
                }
            }
            // The claim is not that the number of stacks is the same -- two
            // stacks can land in one group -- but that no stack came apart.
            for (const std::vector<u32>& group : before) {
                ++stacksBefore;
                bool together = true;
                for (const u32 island : group) {
                    together = together && groupAfter[island] != geom::kInvalidId &&
                               groupAfter[island] == groupAfter[group.front()];
                }
                stacksKept += together ? 1u : 0u;
                if (!together && reported < 3) {
                    ++reported;
                    std::cout << "  stack lost in " << test::pathText(files[i].filename()) << " "
                              << mesh.name << ": " << group.size() << " islands, scaled "
                              << result.scaled << std::endl;
                }
            }

            f32 packed = 0.0f;
            for (u32 island = 0; island < islands.count; ++island) {
                const uv::UvBounds bounds = uv::BoundsOf(mesh, islands, island, 0);
                packed += bounds.width() * bounds.height();
            }
            improved += packed >= shipped * 0.98f ? 1u : 0u;
        }
    }
    if (meshes == 0) {
        SKIP("no mesh survived the ingest");
    }
    std::sort(times.begin(), times.end());
    std::cout << "UV pack corpus: " << meshes << " meshes, " << overlapping
              << " with an overlap, mean coverage " << (100.0 * coverage / meshes) << "%, "
              << (100.0 * improved / meshes)
              << "% at least as tight as shipped, stacks " << stacksKept << " of "
              << stacksBefore << ", median " << times[times.size() / 2] << " ms\n";
    // The one thing a packer may never do.
    CHECK(overlapping == 0u);
    // And the stacks a file shipped are still stacked afterwards. Not every
    // one: two islands a file laid within a thousandth of one another are a
    // stack by the tolerance and can sort differently once they have been
    // turned, which is a handful in fifteen hundred.
    CHECK(stacksKept * 100 >= stacksBefore * 99);
    // The measured answer (EDIT_MODE_UV_PLAN.md §19). The skyline packer fills
    // no hole it has passed, so a hand layout still beats it on about a third
    // of the corpus; folding a hole-filling pass in is the improvement, and it
    // is not this plan's.
    CHECK(improved * 100 >= meshes * 60);
}
