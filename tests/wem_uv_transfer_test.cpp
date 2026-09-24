// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G-U6: Transfer, the bake (EDIT_MODE_UV_DESIGN.md §9.3; EDIT_MODE_UV_PLAN.md
/// §12.1). A texture redrawn for another set has to be the same paint on the
/// same surface: exact where the two sets agree, within a rounding where they
/// do not, and a normal map has to point the same way in the world afterwards.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/triangulation.h>
#include <whiteout/models/wem/geometry/uv/islands.h>
#include <whiteout/models/wem/geometry/uv/transfer.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

using geom::Domain;
using geom::FaceId;
using geom::HalfedgeId;
namespace uv = geom::uv;
using textures::PixelFormat;
using textures::Texture;
using textures::TextureKind;

/// Where a corner lands in one set: its face and its world position decide.
using UvOf = std::function<Vector2f(u32 face, const Vector3f& position)>;

/// `n` by `n` quads over the unit square in xy, facing +z, with two sets.
Mesh sheet(u32 n, const UvOf& set0, const UvOf& set1) {
    geom::FaceSet faces;
    faces.vertexCount = (n + 1) * (n + 1);
    const auto at = [n](u32 x, u32 y) { return y * (n + 1) + x; };
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            faces.addFace(std::vector<u32>{at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)});
        }
    }
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    for (u32 y = 0; y <= n; ++y) {
        for (u32 x = 0; x <= n; ++x) {
            positions[at(x, y)] = Vector3f{static_cast<f32>(x) / static_cast<f32>(n),
                                           static_cast<f32>(y) / static_cast<f32>(n), 0.0f};
        }
    }
    REQUIRE(mesh.ensureConnectivity().ok());
    mesh.sections.emplace_back();
    const geom::Topology& topology = std::as_const(mesh).topology();
    for (u32 set = 0; set < 2; ++set) {
        const UvOf& of = set == 0 ? set0 : set1;
        const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
            geom::names::uv(set), Domain::Halfedge, geom::AttrType::F32x2);
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                uvs[h.index()] = of(f, positions[topology.from(h).index()]);
            }
        }
    }
    mesh.recomputeBounds();
    return mesh;
}

Vector2f same(u32, const Vector3f& p) {
    return Vector2f{p.x, p.y};
}

/// Flipped in u and turned a quarter: together, the mirror across the
/// diagonal. Every texel centre still lands on a texel centre, and the frame
/// changes both its axes and its handedness.
Vector2f flippedAndTurned(u32, const Vector3f& p) {
    return Vector2f{p.y, p.x};
}

Texture image(u32 size, bool srgb, const std::function<void(u32 x, u32 y, u8 (&px)[4])>& fill) {
    Texture t = Texture::create2D(PixelFormat::RGBA8, size, size, 1);
    const std::span<u8> bytes = t.mipData(0);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            u8 px[4];
            fill(x, y, px);
            std::copy(px, px + 4, bytes.begin() + (static_cast<std::size_t>(y) * size + x) * 4);
        }
    }
    t.setSrgb(srgb);
    t.setKind(TextureKind::Diffuse);
    return t;
}

/// A hash, not a pattern: a bake that read the neighbouring texel would show.
u8 noise(u32 x, u32 y, u32 c) {
    u32 h = x * 374761393u + y * 668265263u + c * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<u8>(h >> 24);
}

const u8* texel(const Texture& t, u32 x, u32 y) {
    return t.mipData(0).data() + (static_cast<std::size_t>(y) * t.width() + x) * 4;
}

uv::TransferReport transfer(const Mesh& mesh, u32 from, u32 to, const uv::TransferSource& source,
                            Texture& out, u32 padding = 4) {
    const uv::TransferMesh entry{&mesh, from, to, nullptr};
    uv::TransferOptions options;
    options.padding = padding;
    uv::TransferReport report;
    std::optional<Texture> made =
        uv::TransferTexture(std::span<const uv::TransferMesh>(&entry, 1), source, options, report);
    REQUIRE(made.has_value());
    out = std::move(*made);
    return report;
}

Vector3f decodeNormal(const u8* px) {
    const Vector3f n{px[0] / 255.0f * 2.0f - 1.0f, px[1] / 255.0f * 2.0f - 1.0f,
                     px[2] / 255.0f * 2.0f - 1.0f};
    return n * (1.0f / n.length());
}

} // namespace

TEST_CASE("UV transfer: a copied set is byte-exact", "[wem][uv][transfer]") {
    const Mesh mesh = sheet(4, same, same);
    const Texture source = image(256, true, [](u32 x, u32 y, u8 (&px)[4]) {
        for (u32 c = 0; c < 4; ++c) {
            px[c] = noise(x, y, c);
        }
    });
    Texture out;
    const uv::TransferReport report = transfer(mesh, 0, 1, {&source, true, true, true}, out);
    REQUIRE(out.width() == 256u);
    CHECK(out.isSrgb());
    CHECK(report.written == 256u * 256u);
    CHECK(report.unmappedFaces == 0u);
    u32 differing = 0;
    for (u32 y = 0; y < 256; ++y) {
        for (u32 x = 0; x < 256; ++x) {
            differing += std::equal(texel(out, x, y), texel(out, x, y) + 4, texel(source, x, y)) ? 0 : 1;
        }
    }
    CHECK(differing == 0u);
}

TEST_CASE("UV transfer: there and back through a flipped, turned set", "[wem][uv][transfer]") {
    const Mesh mesh = sheet(4, same, flippedAndTurned);
    const Texture source = image(256, true, [](u32 x, u32 y, u8 (&px)[4]) {
        for (u32 c = 0; c < 4; ++c) {
            px[c] = noise(x, y, c);
        }
    });
    Texture there;
    transfer(mesh, 0, 1, {&source, true, true, true}, there);
    // The paint followed the surface: set 1's texel (x, y) is set 0's (y, x).
    CHECK(std::equal(texel(there, 17, 200), texel(there, 17, 200) + 4, texel(source, 200, 17)));
    Texture back;
    transfer(mesh, 1, 0, {&there, true, true, true}, back);
    u32 worst = 0;
    for (u32 y = 0; y < 256; ++y) {
        for (u32 x = 0; x < 256; ++x) {
            for (u32 c = 0; c < 4; ++c) {
                worst = std::max<u32>(worst, static_cast<u32>(std::abs(
                                                 static_cast<i32>(texel(back, x, y)[c]) -
                                                 static_cast<i32>(texel(source, x, y)[c]))));
            }
        }
    }
    CHECK(worst <= 2u);
}

TEST_CASE("UV transfer: a normal map points the same way afterwards", "[wem][uv][transfer]") {
    const Mesh mesh = sheet(4, same, flippedAndTurned);
    // A field tilted differently in x and y, so a frame that swapped them, or
    // mirrored one, would move every normal.
    Texture source = image(128, false, [](u32 x, u32 y, u8 (&px)[4]) {
        const f32 a = static_cast<f32>(x) / 128.0f * 6.2831853f;
        const f32 b = static_cast<f32>(y) / 128.0f * 3.1415926f;
        Vector3f n{0.6f * std::sin(a), 0.25f * std::cos(b), 1.0f};
        n = n * (1.0f / n.length());
        px[0] = static_cast<u8>(std::lround((n.x * 0.5f + 0.5f) * 255.0f));
        px[1] = static_cast<u8>(std::lround((n.y * 0.5f + 0.5f) * 255.0f));
        px[2] = static_cast<u8>(std::lround((n.z * 0.5f + 0.5f) * 255.0f));
        px[3] = 255;
    });
    source.setKind(TextureKind::Normal);
    Texture out;
    transfer(mesh, 0, 1, {&source, true, true, false, uv::NormalPacking::None}, out);
    CHECK_FALSE(out.isSrgb());

    // Each set's frame on this sheet, worked out by hand: set 0's u runs along
    // x and v along y; set 1's u runs along y and v along x, which is the
    // other handedness. The face normal is +z for both.
    const auto world0 = [](const Vector3f& t) { return Vector3f{t.x, t.y, t.z}; };
    const auto world1 = [](const Vector3f& t) { return Vector3f{t.y, t.x, t.z}; };
    f32 worst = 0.0f;
    for (u32 y = 0; y < 128; ++y) {
        for (u32 x = 0; x < 128; ++x) {
            const Vector3f painted = world0(decodeNormal(texel(source, y, x)));
            const Vector3f baked = world1(decodeNormal(texel(out, x, y)));
            const f32 cosine = std::clamp(painted.dot(baked), -1.0f, 1.0f);
            worst = std::max(worst, std::acos(cosine) * 57.29578f);
        }
    }
    CHECK(worst <= 2.0f);
}

TEST_CASE("UV transfer: a stacked target counts where its sources disagree", "[wem][uv][transfer]") {
    // Two quads side by side in set 0, both on the whole tile in set 1.
    // Per face rather than per point: the shared edge is x = 0.5, which is
    // u = 1 for the left face and u = 0 for the right one.
    const UvOf perFace = [](u32 face, const Vector3f& p) {
        return Vector2f{face == 0 ? p.x * 2.0f : p.x * 2.0f - 1.0f, p.y};
    };
    Mesh mesh;
    {
        geom::FaceSet faces;
        faces.vertexCount = 6;
        faces.addFace(std::vector<u32>{0, 1, 4, 3});
        faces.addFace(std::vector<u32>{1, 2, 5, 4});
        mesh.setFaceSet(faces);
        const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
            geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
        const Vector3f at[6] = {{0, 0, 0}, {0.5f, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0.5f, 1, 0}, {1, 1, 0}};
        std::copy(at, at + 6, positions.begin());
        REQUIRE(mesh.ensureConnectivity().ok());
        mesh.sections.emplace_back();
        const geom::Topology& topology = std::as_const(mesh).topology();
        for (u32 set = 0; set < 2; ++set) {
            const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
                geom::names::uv(set), Domain::Halfedge, geom::AttrType::F32x2);
            for (u32 f = 0; f < 2; ++f) {
                for (const HalfedgeId h : topology.fh(FaceId(f))) {
                    const Vector3f p = positions[topology.from(h).index()];
                    uvs[h.index()] = set == 0 ? Vector2f{p.x, p.y} : perFace(f, p);
                }
            }
        }
    }
    const uv::UvIslands islands = uv::BuildUvIslands(mesh, 1);
    REQUIRE(islands.count == 2u);
    const uv::TransferMesh entry{&mesh, 0, 1, &islands};

    // The two halves of the source differ: every texel the second island
    // covers disagrees with the first.
    const Texture halves = image(64, false, [](u32 x, u32, u8 (&px)[4]) {
        px[0] = x < 32 ? 255 : 0;
        px[1] = 0;
        px[2] = x < 32 ? 0 : 255;
        px[3] = 255;
    });
    uv::TransferReport report;
    REQUIRE(uv::TransferTexture(std::span<const uv::TransferMesh>(&entry, 1), {&halves, true, true, false},
                                {}, report)
                .has_value());
    CHECK(report.twiceDiffering > 64u * 60u);

    // The same stack over a flat colour agrees everywhere.
    const Texture flat = image(64, false, [](u32, u32, u8 (&px)[4]) {
        px[0] = 90;
        px[1] = 120;
        px[2] = 150;
        px[3] = 255;
    });
    REQUIRE(uv::TransferTexture(std::span<const uv::TransferMesh>(&entry, 1), {&flat, true, true, false},
                                {}, report)
                .has_value());
    CHECK(report.twiceDiffering == 0u);
}

TEST_CASE("UV transfer: a wrapping source repeats past its edge", "[wem][uv][transfer]") {
    // Set 0 runs from u = 0.5 to 1.5: its right half is past the edge.
    const Mesh mesh = sheet(4, [](u32, const Vector3f& p) { return Vector2f{p.x + 0.5f, p.y}; }, same);
    const Texture source = image(64, false, [](u32 x, u32 y, u8 (&px)[4]) {
        for (u32 c = 0; c < 4; ++c) {
            px[c] = noise(x, y, c);
        }
    });
    Texture wrapped;
    transfer(mesh, 0, 1, {&source, true, true, false}, wrapped);
    Texture clamped;
    transfer(mesh, 0, 1, {&source, false, true, false}, clamped);
    for (const u32 x : {40u, 50u, 60u}) {
        // Target texel x reads source u = x/64 + 0.5, which repeats to x - 32.
        CHECK(std::equal(texel(wrapped, x, 10), texel(wrapped, x, 10) + 4, texel(source, x - 32, 10)));
        // Clamped, it reads the last column.
        CHECK(std::equal(texel(clamped, x, 10), texel(clamped, x, 10) + 4, texel(source, 63, 10)));
    }
}

TEST_CASE("UV transfer: the padding rings an island", "[wem][uv][transfer]") {
    // A small island in the middle of set 1, so there is room round it.
    const Mesh mesh =
        sheet(2, same, [](u32, const Vector3f& p) { return Vector2f{0.25f + p.x * 0.5f, 0.25f + p.y * 0.5f}; });
    const Texture source = image(64, false, [](u32, u32, u8 (&px)[4]) {
        px[0] = px[1] = px[2] = px[3] = 255;
    });
    Texture out;
    const uv::TransferReport report = transfer(mesh, 0, 1, {&source, true, true, false}, out, 3);
    CHECK(report.written == 32u * 32u);
    CHECK(report.dilated > 0u);
    // The island's texels are [16, 48); the raster's graze ring writes one
    // more on each side, and three rings of padding go on from there: [12, 52).
    CHECK(texel(out, 12, 30)[3] == 255);
    CHECK(texel(out, 51, 30)[3] == 255);
    CHECK(texel(out, 11, 30)[3] == 0);
    CHECK(texel(out, 52, 30)[3] == 0);
}

// ============================================================================
// The corpus (EDIT_MODE_UV_PLAN.md §12)
// ============================================================================

namespace {

/// The texels a triangle's centre test covers, inclusive of its edges: what
/// an island owns, worked out apart from the bake that is being checked.
void cover(std::vector<u8>& owned, u32 size, const Vector2f (&t)[3]) {
    const auto edge = [](const Vector2f& a, const Vector2f& b, const Vector2f& p) {
        return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    };
    const f32 area = edge(t[0], t[1], t[2]);
    if (std::abs(area) < 1e-12f) {
        return;
    }
    const f32 sign = area > 0.0f ? 1.0f : -1.0f;
    const i32 x0 = std::max(0, static_cast<i32>(std::floor(std::min({t[0].x, t[1].x, t[2].x}))));
    const i32 x1 = std::min(static_cast<i32>(size) - 1,
                            static_cast<i32>(std::ceil(std::max({t[0].x, t[1].x, t[2].x}))));
    const i32 y0 = std::max(0, static_cast<i32>(std::floor(std::min({t[0].y, t[1].y, t[2].y}))));
    const i32 y1 = std::min(static_cast<i32>(size) - 1,
                            static_cast<i32>(std::ceil(std::max({t[0].y, t[1].y, t[2].y}))));
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const Vector2f p{static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f};
            if (sign * edge(t[0], t[1], p) >= 0.0f && sign * edge(t[1], t[2], p) >= 0.0f &&
                sign * edge(t[2], t[0], p) >= 0.0f) {
                owned[static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)] = 1;
            }
        }
    }
}

} // namespace

TEST_CASE("UV transfer: the HD models with two sets", "[wem][uv][transfer][corpus]") {
    const auto all = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (all.empty()) {
        SKIP("MDX corpus not found");
    }
    // The HD models first, so the sweep limit counts the files this is about.
    // UTF-8, not `string()`: that one is the ANSI code page, and a corpus name
    // outside it throws.
    std::vector<std::filesystem::path> files;
    for (const auto& file : all) {
        const std::u8string name = file.generic_u8string();
        if (std::string(name.begin(), name.end()).find("_hd.w3mod") != std::string::npos) {
            files.push_back(file);
        }
    }
    constexpr u32 kSize = 1024;
    constexpr u32 kPadding = 4;
    // A synthetic gradient, opaque everywhere, so "written" is alpha and no
    // texture has to be read.
    const Texture gradient = image(kSize, true, [](u32 x, u32 y, u8 (&px)[4]) {
        px[0] = static_cast<u8>(x >> 2);
        px[1] = static_cast<u8>(y >> 2);
        px[2] = static_cast<u8>((x + y) >> 3);
        px[3] = 255;
    });
    const MdxConverter converter;
    const std::size_t limit = test::sweepLimit(files.size(), 400);

    u32 models = 0;
    u32 holed = 0;
    std::vector<f32> coverages;
    std::vector<f64> times;
    for (std::size_t i = 0; i < limit; ++i) {
        const std::u8string name = files[i].generic_u8string();
        const std::string path(name.begin(), name.end());
        if (test::isKnownBad(files[i])) {
            continue;
        }
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        Result<Document> imported = converter.importFromBytes(std::span<const u8>(bytes));
        if (!imported.ok() || imported->models.empty()) {
            continue;
        }
        std::vector<Mesh> prepared;
        bool every = true;
        for (const Mesh& mesh : imported->models[0].meshes) {
            if (mesh.lodLevel != 0) {
                continue;
            }
            every = every && geom::UvSetCount(mesh.attributes) >= 2;
            prepared.push_back(mesh);
        }
        if (prepared.empty() || !every) {
            continue;
        }
        ++models;
        std::vector<uv::TransferMesh> entries;
        for (Mesh& mesh : prepared) {
            geom::PrepareForModelling(mesh);
            if (mesh.hasConnectivity()) {
                entries.push_back(uv::TransferMesh{&mesh, 0, 1, nullptr});
            }
        }
        uv::TransferOptions options;
        options.padding = kPadding;
        uv::TransferReport report;
        const auto start = std::chrono::steady_clock::now();
        const std::optional<Texture> out =
            uv::TransferTexture(std::span<const uv::TransferMesh>(entries), {&gradient, true, true, true},
                                options, report);
        times.push_back(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                            .count());
        REQUIRE(out.has_value());

        // What the islands own, and every texel within the padding of it,
        // has to have been written.
        std::vector<u8> owned(static_cast<std::size_t>(kSize) * kSize, 0);
        for (const uv::TransferMesh& entry : entries) {
            const Mesh& mesh = *entry.mesh;
            const geom::Topology& topology = mesh.topology();
            const std::span<const Vector2f> uvs =
                mesh.attributes.get<const Vector2f>(geom::names::uv(1), Domain::Halfedge);
            const std::span<const Vector3f> positions =
                mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
            std::vector<u32> loop;
            std::vector<HalfedgeId> corners;
            std::vector<u32> cut;
            for (u32 f = 0; f < topology.faceCount(); ++f) {
                if (topology.isDeleted(FaceId(f))) {
                    continue;
                }
                loop.clear();
                corners.clear();
                for (const HalfedgeId h : topology.fh(FaceId(f))) {
                    corners.push_back(h);
                    loop.push_back(topology.from(h).value());
                }
                cut.clear();
                geom::TriangulateFace(loop, positions, mesh.triangulation.row(f), cut);
                for (std::size_t c = 0; c + 2 < cut.size(); c += 3) {
                    Vector2f t[3];
                    for (u32 k = 0; k < 3; ++k) {
                        const Vector2f u = uvs[corners[cut[c + k]].index()];
                        t[k] = Vector2f{u.x * kSize, u.y * kSize};
                    }
                    cover(owned, kSize, t);
                }
            }
        }
        const std::span<const u8> written = out->mipData(0);
        u32 ownedCount = 0;
        u32 holes = 0;
        for (i32 y = 0; y < static_cast<i32>(kSize); ++y) {
            for (i32 x = 0; x < static_cast<i32>(kSize); ++x) {
                if (owned[static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)] == 0) {
                    continue;
                }
                ++ownedCount;
                for (i32 dy = -static_cast<i32>(kPadding); dy <= static_cast<i32>(kPadding); ++dy) {
                    for (i32 dx = -static_cast<i32>(kPadding); dx <= static_cast<i32>(kPadding); ++dx) {
                        const i32 nx = x + dx;
                        const i32 ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<i32>(kSize) || ny >= static_cast<i32>(kSize)) {
                            continue;
                        }
                        if (written[(static_cast<std::size_t>(ny) * kSize + static_cast<std::size_t>(nx)) * 4 + 3] == 0) {
                            ++holes;
                        }
                    }
                }
            }
        }
        if (holes != 0) {
            ++holed;
            INFO(path << ": " << holes << " unwritten texels within the padding");
            CHECK(holes == 0u);
        }
        coverages.push_back(static_cast<f32>(ownedCount) / static_cast<f32>(kSize * kSize));
    }
    if (models == 0) {
        SKIP("no HD model with two sets in the sweep");
    }
    std::sort(coverages.begin(), coverages.end());
    std::sort(times.begin(), times.end());
    const f32 median = coverages[coverages.size() / 2];
    std::cout << "UV transfer corpus: " << models << " HD models with two sets, " << holed
              << " with a hole; median coverage " << median * 100.0f << "% (shipped 71%), median "
              << times[times.size() / 2] << " ms, slowest " << times.back() << " ms" << std::endl;
    CHECK(holed == 0u);
    // The coverage is the shipped layout's, not the bake's: it is printed
    // beside the survey's 71 %, which was measured by another rasteriser (a
    // centre test at 1024 gives thin islands less than a conservative one at a
    // lower size). What the bake owes is the hole count above, over every HD
    // model that ships the set -- so the sweep may not quietly shrink.
    if (limit == files.size()) {
        CHECK(models >= 340u);
    }
}
