// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P2 gate — the §5.8 identity property.
///
/// > For a mesh imported from an all-triangle source where each source vertex
/// > became one `mergeGroup` and its halfedges kept source order,
/// > `BuildRenderMesh` with the source's attribute set reproduces the source
/// > vertex buffer and index buffer **byte-identically** — repaired or not.
///
/// "Byte-identically" is taken literally here: the expected side is built by
/// feeding the *source's own arrays* through the same `VertexBufferBuilder` the
/// render view uses, and the two interleaved blobs are `memcmp`'d. Nothing in the
/// comparison has a tolerance.
///
/// Two honest qualifications, both properties of shipped content rather than of
/// the render view:
///
/// - A source vertex no surviving face references has no GPU vertex. The expected
///   buffer therefore covers the *referenced* vertices, in source order.
/// - A face the §5.3 repair dropped contributes no indices. The expected index
///   list is the source's minus exactly those faces, which `RepairLog` names.
///
/// Where neither applies — the synthetic fixtures — the comparison is against the
/// source arrays verbatim, with no compaction at all.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/render_view.h>

#include "test_helpers.h"
#include "wem_geometry_ingest.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

utils::VertexBuffer buildExpected(const std::vector<Vector3f>& positions,
                                  const std::vector<Vector3f>& normals,
                                  const std::vector<Vector2f>& uv0) {
    utils::VertexBufferBuilder builder;
    builder.declareAttribute(positions, utils::AttributeClass::Position,
                             utils::AttributeEncoding::Float32);
    builder.declareAttribute(normals, utils::AttributeClass::Normal,
                             utils::AttributeEncoding::Float32);
    builder.declareAttribute(uv0, utils::AttributeClass::UV, utils::AttributeEncoding::Float32);
    return builder.build();
}

struct IdentityCheck {
    bool ran = false;
    bool matched = false;
    bool compacted = false; ///< Unreferenced vertices or dropped faces were excluded.
    std::string detail;
};

/// The source triangles that survived into the built mesh, in builder-face order.
///
/// `ingestIndexed` skips a triangle with an out-of-range corner before the
/// builder ever sees it, and the repair drops faces by *builder* index — so the
/// two filters have to be applied in that order or the indices do not line up.
std::vector<u32> survivingTriangles(const test::IngestedMesh& ingested) {
    const std::size_t vertexCount = ingested.sourcePositions.size();
    std::vector<u32> accepted;
    const std::size_t triangles = ingested.sourceIndices.size() / 3;
    for (std::size_t t = 0; t < triangles; ++t) {
        const u32 a = ingested.sourceIndices[t * 3 + 0];
        const u32 b = ingested.sourceIndices[t * 3 + 1];
        const u32 c = ingested.sourceIndices[t * 3 + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount) {
            continue;
        }
        accepted.push_back(static_cast<u32>(t));
    }
    std::vector<bool> dropped(accepted.size(), false);
    for (const geom::FaceRecord& record : ingested.mesh.repairLog.droppedFaces) {
        if (record.index < dropped.size()) {
            dropped[record.index] = true;
        }
    }
    std::vector<u32> out;
    out.reserve(accepted.size());
    for (std::size_t i = 0; i < accepted.size(); ++i) {
        if (!dropped[i]) {
            out.push_back(accepted[i]);
        }
    }
    return out;
}

IdentityCheck checkIdentity(const test::IngestedMesh& ingested) {
    IdentityCheck result;
    const std::size_t vertexCount = ingested.sourcePositions.size();
    if (vertexCount == 0 || ingested.sourceNormals.size() != vertexCount ||
        ingested.sourceUv0.size() != vertexCount) {
        // A partial attribute set is a different property; the render view would
        // correctly skip the missing layer and the comparison would be vacuous.
        return result;
    }

    const std::vector<u32> triangles = survivingTriangles(ingested);
    if (triangles.empty()) {
        return result;
    }

    std::vector<u32> gpuOf(vertexCount, geom::kInvalidId);
    std::vector<u32> kept;
    for (u32 t : triangles) {
        for (u32 c = 0; c < 3; ++c) {
            const u32 v = ingested.sourceIndices[t * 3 + c];
            if (gpuOf[v] == geom::kInvalidId) {
                gpuOf[v] = 0; // marked; renumbered in source order below
            }
        }
    }
    for (u32 v = 0; v < vertexCount; ++v) {
        if (gpuOf[v] != geom::kInvalidId) {
            gpuOf[v] = static_cast<u32>(kept.size());
            kept.push_back(v);
        }
    }

    std::vector<Vector3f> positions;
    std::vector<Vector3f> normals;
    std::vector<Vector2f> uv0;
    positions.reserve(kept.size());
    normals.reserve(kept.size());
    uv0.reserve(kept.size());
    for (u32 v : kept) {
        positions.push_back(ingested.sourcePositions[v]);
        normals.push_back(ingested.sourceNormals[v]);
        uv0.push_back(ingested.sourceUv0[v]);
    }
    std::vector<u32> indices;
    indices.reserve(triangles.size() * 3);
    for (u32 t : triangles) {
        for (u32 c = 0; c < 3; ++c) {
            indices.push_back(gpuOf[ingested.sourceIndices[t * 3 + c]]);
        }
    }

    result.ran = true;
    result.compacted =
        kept.size() != vertexCount || triangles.size() != ingested.sourceIndices.size() / 3;

    const utils::VertexBuffer expected = buildExpected(positions, normals, uv0);
    const geom::RenderMesh render =
        geom::BuildRenderMesh(ingested.mesh, geom::RenderMeshDesc::Standard());

    if (render.vertices.vertex_stride != expected.vertex_stride) {
        result.detail = "stride " + std::to_string(render.vertices.vertex_stride) + " != " +
                        std::to_string(expected.vertex_stride);
        return result;
    }
    if (render.vertices.data.size() != expected.data.size()) {
        result.detail = "vertex bytes " + std::to_string(render.vertices.data.size()) + " != " +
                        std::to_string(expected.data.size());
        return result;
    }
    if (render.vertices.data != expected.data) {
        std::size_t first = 0;
        while (first < expected.data.size() && render.vertices.data[first] == expected.data[first]) {
            ++first;
        }
        result.detail = "vertex bytes differ at " + std::to_string(first) + " (vertex " +
                        std::to_string(first / std::max<std::size_t>(expected.vertex_stride, 1)) +
                        ")";
        return result;
    }
    if (render.indices != indices) {
        result.detail = "indices differ (" + std::to_string(render.indices.size()) + " vs " +
                        std::to_string(indices.size()) + ")";
        return result;
    }
    result.matched = true;
    return result;
}

/// A spread of non-collinear positions — a collinear fixture is zero-area and the
/// repair correctly drops every face of it.
const Vector3f kSpread[8] = {
    {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.25f},
    {0.5f, 2.0f, 1.0f}, {2.0f, 0.5f, 1.5f}, {2.5f, 2.5f, 0.75f}, {1.5f, 3.0f, 2.0f},
};

test::IngestedMesh makeFixture(std::size_t vertexCount, const std::vector<u32>& indices,
                               const char* name) {
    std::vector<Vector3f> positions;
    std::vector<Vector3f> normals;
    std::vector<Vector2f> uv0;
    for (std::size_t i = 0; i < vertexCount; ++i) {
        positions.push_back(kSpread[i % 8]);
        normals.push_back(Vector3f{static_cast<f32>(i) * 0.125f, 1.0f, 0.5f});
        uv0.push_back(Vector2f{static_cast<f32>(i) * 0.25f, 0.75f});
    }
    return test::detail::ingestIndexed(positions, normals, uv0, indices, name);
}

std::string pathText(const fs::path& path) {
    const std::u8string utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::vector<u8> readFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::vector<u8>((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
}

std::vector<fs::path> gather(const char* envVar, const char* extension,
                             std::initializer_list<const char*> subdirs) {
    std::vector<fs::path> files;
    const auto collect = [&](const fs::path& root) {
        if (!fs::is_directory(root)) {
            return;
        }
        std::error_code error;
        for (fs::recursive_directory_iterator it(
                 root, fs::directory_options::skip_permission_denied, error);
             it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (error) {
                break;
            }
            if (it->is_regular_file(error) && it->path().extension() == extension) {
                files.push_back(it->path());
            }
        }
    };
    if (const char* env = std::getenv(envVar); env != nullptr && fs::is_directory(env)) {
        collect(env);
    } else {
        const std::string base = test::findCorpusBase("Corpus");
        if (!base.empty()) {
            for (const char* sub : subdirs) {
                collect(fs::path(base) / sub);
            }
        }
    }
    std::error_code absError;
    for (fs::path& file : files) {
        const fs::path resolved = fs::absolute(file, absError);
        if (!absError) {
            file = resolved.lexically_normal();
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

// ============================================================================
// (a) a clean mesh — the strict form, no compaction anywhere
// ============================================================================

TEST_CASE("WEM render view reproduces a clean mesh byte-identically", "[wem][geometry][render]") {
    // A quad as two triangles: every vertex referenced, no face dropped.
    const test::IngestedMesh quad = makeFixture(4, {0, 1, 3, 0, 3, 2}, "quad");
    REQUIRE(quad.repairStats.verticesAdded == 0);
    REQUIRE(quad.repairStats.facesDropped == 0);

    const IdentityCheck check = checkIdentity(quad);
    INFO(check.detail);
    CHECK(check.ran);
    CHECK_FALSE(check.compacted);
    CHECK(check.matched);

    // And spelled out once against the source arrays themselves, so the property
    // does not rest on `checkIdentity`'s bookkeeping being right.
    const utils::VertexBuffer expected =
        buildExpected(quad.sourcePositions, quad.sourceNormals, quad.sourceUv0);
    const geom::RenderMesh render =
        geom::BuildRenderMesh(quad.mesh, geom::RenderMeshDesc::Standard());
    CHECK(render.vertices.data == expected.data);
    CHECK(render.indices == quad.sourceIndices);
    CHECK(render.vertexCount() == 4);
    for (u32 i = 0; i < 4; ++i) {
        CHECK(render.vertexToWemVertex[i] == i);
    }
}

TEST_CASE("WEM render view emits one range per section", "[wem][geometry][render]") {
    test::IngestedMesh mesh = makeFixture(6, {0, 1, 3, 2, 4, 5}, "two_sections");
    mesh.mesh.sections.push_back(MeshSection{});
    mesh.mesh.sections.back().name = "second";
    mesh.mesh.sections.back().materialSlot = 7;
    mesh.mesh.faceSections()[1] = 1;

    const geom::RenderMesh render =
        geom::BuildRenderMesh(mesh.mesh, geom::RenderMeshDesc::Standard());
    REQUIRE(render.ranges.size() == 2);
    CHECK(render.ranges[0].section == 0);
    CHECK(render.ranges[0].indexCount == 3);
    CHECK(render.ranges[1].section == 1);
    CHECK(render.ranges[1].materialSlot == 7);
    CHECK(render.ranges[1].indexCount == 3);
    CHECK(render.indices.size() == 6);
}

TEST_CASE("WEM render view splits a seam into two GPU vertices", "[wem][geometry][render]") {
    // The other half of the contract: the merge group groups, the *attributes*
    // still split. Two triangles sharing an edge, with one shared vertex given
    // different UVs on each side.
    test::IngestedMesh mesh = makeFixture(4, {0, 1, 3, 0, 3, 2}, "seam");
    const geom::Topology& topology = mesh.mesh.topology();
    // Corner 0 of face 1 is vertex 0; give it a different UV from face 0's.
    const geom::HalfedgeId corner = topology.halfedge(geom::FaceId(1));
    auto uvs = mesh.mesh.attributes.get<Vector2f>(geom::names::uv(0), geom::Domain::Halfedge);
    REQUIRE(topology.from(corner) == geom::VertexId(0));
    uvs[corner.index()] = Vector2f{9.0f, 9.0f};

    const geom::RenderMesh render =
        geom::BuildRenderMesh(mesh.mesh, geom::RenderMeshDesc::Standard());
    CHECK(render.vertexCount() == 5);
    // The split copy sorts right after its group-mate, not at the end.
    CHECK(render.vertexToWemVertex[0] == 0);
    CHECK(render.vertexToWemVertex[1] == 0);
}

// ============================================================================
// (b) a mesh the repair touched
// ============================================================================

TEST_CASE("WEM render view survives a non-manifold edge repair", "[wem][geometry][render]") {
    // Three faces on edge (0,1). The repair splits rather than refuses, and the
    // copies carry the same merge group — so the GPU view is the source's.
    const test::IngestedMesh mesh =
        makeFixture(5, {0, 1, 2, 1, 0, 3, 0, 1, 4}, "non_manifold_edge");
    REQUIRE(mesh.repairStats.verticesAdded > 0);

    const IdentityCheck check = checkIdentity(mesh);
    INFO(check.detail);
    CHECK(check.ran);
    CHECK(check.matched);
    CHECK_FALSE(check.compacted);
}

TEST_CASE("WEM render view survives a bowtie repair", "[wem][geometry][render]") {
    // Two triangles meeting at vertex 0 only: one fan each, so vertex 0 splits.
    const test::IngestedMesh mesh = makeFixture(5, {0, 1, 2, 0, 3, 4}, "bowtie");
    REQUIRE(mesh.repairStats.verticesAdded > 0);

    const IdentityCheck check = checkIdentity(mesh);
    INFO(check.detail);
    CHECK(check.ran);
    CHECK(check.matched);
}

TEST_CASE("WEM render view accounts for a dropped degenerate face", "[wem][geometry][render]") {
    // Face 1 repeats a corner, so the repair drops it outright.
    const test::IngestedMesh mesh = makeFixture(4, {0, 1, 3, 0, 0, 1, 0, 3, 2}, "degenerate");
    REQUIRE(mesh.repairStats.facesDropped == 1);

    const IdentityCheck check = checkIdentity(mesh);
    INFO(check.detail);
    CHECK(check.ran);
    CHECK(check.matched);
    CHECK(check.compacted); // a dropped face means fewer indices, by construction
}

TEST_CASE("WEM repair splits a duplicate face rather than dropping it",
          "[wem][geometry][render]") {
    // Worth pinning here rather than in P1's tests, because it is the identity
    // property that depends on it: a repeated face is the cheap two-sided trick,
    // and dropping it would silently lose one of the two surfaces — so the repair
    // duplicates every corner instead, and the index buffer comes back whole.
    const test::IngestedMesh mesh = makeFixture(4, {0, 1, 3, 0, 1, 3, 0, 3, 2}, "duplicate");
    CHECK(mesh.repairStats.facesDropped == 0);
    CHECK(mesh.repairStats.verticesAdded == 3);

    const IdentityCheck check = checkIdentity(mesh);
    INFO(check.detail);
    CHECK(check.ran);
    CHECK(check.matched);
    CHECK_FALSE(check.compacted);
}

// ============================================================================
// (c) the §6.4 basis change
// ============================================================================

TEST_CASE("WEM render view is exact through the SC2 basis change", "[wem][geometry][render]") {
    // `new = (-old.y, old.x, old.z)` — a permutation with sign flips, so it is
    // bit-exact in floating point and the gate stays a byte comparison.
    const auto rebase = [](const Vector3f& v) { return Vector3f{-v.y, v.x, v.z}; };

    test::IngestedMesh mesh = makeFixture(4, {0, 1, 3, 0, 3, 2}, "sc2_basis");
    auto positions =
        mesh.mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    for (Vector3f& p : positions) {
        p = rebase(p);
    }
    auto normals = mesh.mesh.attributes.get<Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
    for (Vector3f& n : normals) {
        n = rebase(n);
    }

    std::vector<Vector3f> expectedPositions;
    std::vector<Vector3f> expectedNormals;
    for (const Vector3f& p : mesh.sourcePositions) {
        expectedPositions.push_back(rebase(p));
    }
    for (const Vector3f& n : mesh.sourceNormals) {
        expectedNormals.push_back(rebase(n));
    }

    const utils::VertexBuffer expected =
        buildExpected(expectedPositions, expectedNormals, mesh.sourceUv0);
    const geom::RenderMesh render =
        geom::BuildRenderMesh(mesh.mesh, geom::RenderMeshDesc::Standard());
    CHECK(render.vertices.data == expected.data);
    CHECK(render.indices == mesh.sourceIndices);
}

// ============================================================================
// The corpus leg — the property over shipped content
// ============================================================================

namespace {

struct SweepResult {
    u32 meshes = 0;
    u32 checked = 0;
    u32 matched = 0;
    u32 repaired = 0; ///< Of the checked, how many the repair had touched.
    u32 empty = 0;    ///< Divisions/geosets with no geometry at all.
    u32 skipped = 0;  ///< Partial attribute sets; see `checkIdentity`.
    std::vector<std::string> failures;

    void report(const char* format) const {
        std::cout << "=== WEM P2 identity sweep: " << format << " ===\n"
                  << "  meshes            " << meshes << "\n"
                  << "  checked           " << checked << "  (" << repaired
                  << " had been repaired)\n"
                  << "  byte-identical    " << matched << "\n"
                  << "  empty             " << empty << "\n"
                  << "  skipped (partial) " << skipped << std::endl;
        for (std::size_t i = 0; i < failures.size() && i < 20; ++i) {
            std::cout << "    " << failures[i] << std::endl;
        }
    }

    void fold(const test::IngestedMesh& ingested, const std::string& label) {
        ++meshes;
        const IdentityCheck check = checkIdentity(ingested);
        if (!check.ran) {
            if (ingested.sourcePositions.empty() || ingested.sourceIndices.empty()) {
                ++empty;
            } else {
                ++skipped;
            }
            return;
        }
        ++checked;
        if (ingested.repairStats.verticesAdded != 0 || ingested.repairStats.facesDropped != 0) {
            ++repaired;
        }
        if (check.matched) {
            ++matched;
        } else if (failures.size() < 20) {
            failures.push_back(label + " -> " + check.detail);
        }
    }
};

} // namespace

TEST_CASE("WEM P2 identity sweep: mdx", "[wem][corpus][geometry][render]") {
    const auto files = gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    SweepResult sweep;
    const std::size_t limit = std::min<std::size_t>(files.size(), 200);
    for (std::size_t i = 0; i < limit; ++i) {
        // The two files `mdx::Parser` does not terminate on; named in
        // wem_geometry_corpus_test.cpp with the symptom.
        if (files[i].filename() == "Aris.mdx" || files[i].filename() == "Aris.fixed.mdx") {
            continue;
        }
        const auto bytes = readFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        auto meshes = test::IngestMdx(std::span<const u8>(bytes.data(), bytes.size()));
        for (std::size_t m = 0; m < meshes.size(); ++m) {
            sweep.fold(meshes[m], pathText(files[i].filename()) + "#" + std::to_string(m));
        }
    }
    sweep.report("mdx");
    CHECK(sweep.checked > 0);
    CHECK(sweep.repaired > 0); // the repaired leg of the gate is really exercised
    CHECK(sweep.matched == sweep.checked);
}

TEST_CASE("WEM P2 identity sweep: m3", "[wem][corpus][geometry][render]") {
    const auto files =
        gather("WEM_M3_CORPUS_DIR", ".m3", {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"});
    if (files.empty()) {
        SKIP("M3 corpus not found");
    }
    SweepResult sweep;
    const std::size_t limit = std::min<std::size_t>(files.size(), 200);
    for (std::size_t i = 0; i < limit; ++i) {
        const auto bytes = readFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        m3::Parser parser;
        const m3::Model model = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        auto meshes = test::IngestM3(model);
        for (std::size_t m = 0; m < meshes.size(); ++m) {
            sweep.fold(meshes[m], pathText(files[i].filename()) + "#" + std::to_string(m));
        }
    }
    sweep.report("m3");
    CHECK(sweep.checked > 0);
    CHECK(sweep.repaired > 0);
    CHECK(sweep.matched == sweep.checked);
}
