// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G29's drop arm (EDIT_MODE_MODELLING_DESIGN.md §8.1, _PLAN.md MX1): a document
/// holds LOD 0 and every-level meshes only, whichever way it arrived, and every
/// mesh referencer follows the renumbering.
///
/// The trap this suite exists for is the one `RemapMeshReferencers` sets on its
/// own: it only invalidates a gone mesh's `Section` channel, and `Validate` then
/// errors on every Reforged file with a ladder. The drop erases those channels
/// first.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/meshes/remove.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>

#include "wem_corpus_files.h"

#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

constexpr u32 kFileAllLods = 0xFFFFFFFFu;

mdx::Node makeNode(const std::string& name, u32 objectId) {
    mdx::Node node;
    node.name = name;
    node.objectId = objectId;
    node.parentId = mdx::Node::NO_PARENT;
    return node;
}

/// One geoset per level, in the order the corpus's worst files have them: a
/// LOD 1 geoset before LOD 0's.
///
/// | geoset | level | triangles | then |
/// |---|---|---|---|
/// | 0 | 1 | 2 | dropped; its alpha record goes with it |
/// | 1 | 0 | 4 | mesh 0; bone 1 is gated here |
/// | 2 | 2 | 1 | dropped |
/// | 3 | every | 2 | mesh 1 |
/// | 4 | 0 | 2 | mesh 2; its alpha record stays |
///
/// Bone 0 is ungated and names geoset 2 raw; bone 2 is ungated and names
/// geoset 4 raw.
mdx::Model makeLadder() {
    mdx::Model model;
    model.version = 1000;
    model.modelName = "ladder";
    mdx::Texture texture;
    texture.fileName = "textures/body.blp";
    model.textures.push_back(texture);
    mdx::Material material;
    mdx::Layer layer;
    layer.textureId = 0;
    material.layers.push_back(layer);
    model.materials.push_back(material);

    for (u32 b = 0; b < 3; ++b) {
        mdx::Bone bone;
        bone.node = makeNode("bone_" + std::to_string(b), b);
        model.bones.push_back(bone);
    }
    model.bones[0].geosetId = 2;
    model.bones[1].geosetId = 1;
    model.bones[1].geosetAnimationId = 1;
    model.bones[2].geosetId = 4;
    model.pivotPoints = {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}, Vector3f{0, 0, 9}};

    const std::array<u32, 5> levels{1, 0, 2, kFileAllLods, 0};
    const std::array<u32, 5> triangles{2, 4, 1, 2, 2};
    for (u32 g = 0; g < 5; ++g) {
        const f32 x = static_cast<f32>(g) * 4.0f;
        mdx::Geoset geoset;
        geoset.lodName = "part_" + std::to_string(g);
        geoset.lod = levels[g];
        for (u32 t = 0; t < triangles[g]; ++t) {
            const f32 y = static_cast<f32>(t);
            const auto first = static_cast<u16>(geoset.vertexPositions.size());
            geoset.vertexPositions.push_back(Vector3f{x, y, 0});
            geoset.vertexPositions.push_back(Vector3f{x + 1, y, 0});
            geoset.vertexPositions.push_back(Vector3f{x, y + 1, 0});
            for (u16 c = 0; c < 3; ++c) {
                geoset.faces.push_back(static_cast<u16>(first + c));
                geoset.vertexNormals.push_back(Vector3f{0, 0, 1});
                geoset.vertexGroups.push_back(0);
            }
        }
        geoset.textureCoordinateSets.push_back(
            std::vector<Vector2f>(geoset.vertexPositions.size(), Vector2f{0.5f, 0.5f}));
        geoset.matrixGroups = {1};
        geoset.matrixIndices = {0};
        model.geosets.push_back(geoset);
    }

    for (const u32 g : {0u, 1u, 4u}) {
        mdx::GeosetAnimation keyed;
        keyed.geosetId = g;
        keyed.alphaTracks.isUsed = true;
        keyed.alphaTracks.interpolationType = mdx::InterpolationType::Linear;
        keyed.alphaTracks.keyCount = 2;
        keyed.alphaTracks.timestamps = {0, 100};
        keyed.alphaTracks.keys_data = {0.0f, 1.0f};
        model.geosetAnimations.push_back(keyed);
    }

    mdx::Sequence stand;
    stand.name = "Stand";
    stand.intervalStart = 0;
    stand.intervalEnd = 100;
    model.sequences.push_back(stand);
    return model;
}

u32 sectionChannelsOf(const Model& model, u32 mesh) {
    u32 count = 0;
    for (const AnimChannel& channel : model.animChannels.channels) {
        count += channel.target.kind == TrackTarget::Kind::Section && channel.target.mesh == mesh;
    }
    return count;
}

/// Every referencer of the §5.12 table names a mesh that exists, and no
/// sub-track joins on a channel that is gone.
void requireReferencersResolve(const Document& document) {
    for (u32 m = 0; m < document.models.size(); ++m) {
        const Model& model = document.models[m];
        const u32 meshes = static_cast<u32>(model.meshes.size());
        for (const AnimChannel& channel : model.animChannels.channels) {
            if (channel.target.kind == TrackTarget::Kind::Section) {
                CHECK(channel.target.mesh < meshes);
            }
        }
        for (const Node& node : model.nodes.nodes) {
            if (const auto* bone = std::get_if<BonePayload>(&node.payload)) {
                CHECK((bone->gateMesh == kInvalidIndex || bone->gateMesh < meshes));
            }
        }
        for (const Clip& clip : document.clips) {
            if (clip.model != m) {
                continue;
            }
            for (const SubTrackContainer& container : clip.containers) {
                for (const SubTrack& track : container.subTracks) {
                    CHECK(model.animChannels.find(track.channel) != nullptr);
                }
            }
        }
    }
}

u32 validationErrors(const Document& document) {
    const Diagnostics report = Validate(document, ValidateLevel::Profile);
    u32 errors = 0;
    for (const Diagnostic& diagnostic : report.all()) {
        errors += diagnostic.severity == Severity::Error;
    }
    return errors;
}

Document viaWem(const Document& document) {
    Writer writer;
    const std::vector<u8> bytes = writer.write(document);
    Parser parser;
    std::optional<Document> read = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(read.has_value());
    return std::move(*read);
}

} // namespace

TEST_CASE("wem lod drop an mdx import keeps LOD 0 and every-level meshes, in order",
          "[wem][lod]") {
    const MdxConverter converter;
    const Result<Document> result = converter.fromMdx(makeLadder());
    REQUIRE(result.ok());
    const Model& model = result->models[0];

    REQUIRE(model.meshes.size() == 3);
    CHECK(model.meshes[0].name == "part_1");
    CHECK(model.meshes[1].name == "part_3");
    CHECK(model.meshes[2].name == "part_4");
    CHECK(model.meshes[0].lodLevel == 0);
    CHECK(model.meshes[1].lodLevel == kAllLods);
    CHECK(model.meshes[2].lodLevel == 0);
    CHECK(result.diagnostics.countOf(DiagCode::LevelOfDetailDropped) == 1u);

    SECTION("the dropped geoset's alpha record goes, and the kept ones follow their mesh") {
        CHECK(sectionChannelsOf(model, 0) == 1u);
        CHECK(sectionChannelsOf(model, 1) == 0u);
        CHECK(sectionChannelsOf(model, 2) == 1u);
        CHECK(result.diagnostics.countOf(DiagCode::AnimChannelInvalidated) == 0u);
        requireReferencersResolve(*result);
        CHECK(validationErrors(*result) == 0u);
    }
    SECTION("a gate and the raw bag geosetId follow the renumbering") {
        const auto& bone0 = model.nodes.nodes[0];
        const auto& bone1 = model.nodes.nodes[1];
        const auto& bone2 = model.nodes.nodes[2];
        CHECK(std::get<BonePayload>(bone1.payload).gateMesh == 0u);
        // Geoset 2 is gone: the raw id is the file's "no geoset".
        CHECK(bone0.native.value("geosetId") == static_cast<i64>(kFileAllLods));
        CHECK(bone2.native.value("geosetId") == 2);
    }
    SECTION("the setting records the source's ladder without turning it on") {
        const LodExport& lods = model.lodExport;
        CHECK_FALSE(lods.generate);
        CHECK(lods.sourceHadLevels);
        // LOD 0 is 4 + 2 triangles; LOD 1 holds 2 and LOD 2 holds 1. LOD 3 is
        // absent, and keeps the corpus's ratio.
        CHECK(std::abs(lods.ratios[0] - 2.0f / 6.0f) < 1e-6f);
        CHECK(std::abs(lods.ratios[1] - 1.0f / 6.0f) < 1e-6f);
        CHECK(lods.ratios[2] == LodExport{}.ratios[2]);
    }
    SECTION("the export writes LOD 0 in its own order") {
        const Result<mdx::Model> exported =
            converter.toMdx(*result, ProfileId::Wc3Reforged, 1000);
        REQUIRE(exported.ok());
        REQUIRE(exported->geosets.size() == 3);
        CHECK(exported->geosets[0].lodName == "part_1");
        CHECK(exported->geosets[1].lod == kFileAllLods);
        CHECK(exported->geosets[2].lodName == "part_4");
    }
}

TEST_CASE("wem lod drop a model with no base level keeps its meshes and says so", "[wem][lod]") {
    mdx::Model source = makeLadder();
    for (mdx::Geoset& geoset : source.geosets) {
        geoset.lod = 2;
    }
    const MdxConverter converter;
    const Result<Document> result = converter.fromMdx(source);
    REQUIRE(result.ok());
    CHECK(result->models[0].meshes.size() == 5);
    CHECK(result.diagnostics.countOf(DiagCode::LevelOfDetailDropped) == 1u);
    CHECK(validationErrors(*result) == 0u);
}

TEST_CASE("wem lod drop a model with no ladder is untouched and says nothing", "[wem][lod]") {
    mdx::Model source = makeLadder();
    for (mdx::Geoset& geoset : source.geosets) {
        geoset.lod = 0;
    }
    const MdxConverter converter;
    const Result<Document> result = converter.fromMdx(source);
    REQUIRE(result.ok());
    CHECK(result->models[0].meshes.size() == 5);
    CHECK(result.diagnostics.countOf(DiagCode::LevelOfDetailDropped) == 0u);
    CHECK_FALSE(result->models[0].lodExport.sourceHadLevels);
}

TEST_CASE("wem lod drop a .wem holding levels loads the way an import does", "[wem][lod]") {
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(makeLadder());
    REQUIRE(imported.ok());
    Document document = std::move(*imported.value);
    Model& model = document.models[0];

    // A LOD 1 copy of mesh 2 with an alpha channel of its own, keyed in the
    // clip, and a bone gated on it: what a document from before the drop held.
    Mesh lod = model.meshes[2];
    lod.name = "part_4_LOD1";
    lod.lodLevel = 1;
    model.meshes.insert(model.meshes.begin(), std::move(lod));
    for (AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Section) {
            ++channel.target.mesh;
        }
    }
    for (Node& node : model.nodes.nodes) {
        if (auto* bone = std::get_if<BonePayload>(&node.payload);
            bone != nullptr && bone->gateMesh != kInvalidIndex) {
            ++bone->gateMesh;
        }
    }
    AnimChannel channel;
    channel.id = model.animChannels.nextFreeId();
    channel.target.kind = TrackTarget::Kind::Section;
    channel.target.mesh = 0;
    channel.target.channel = Channel::Alpha;
    channel.valueType = geom::AttrType::F32;
    const f32 one = 1.0f;
    channel.initValue.assign(reinterpret_cast<const u8*>(&one),
                             reinterpret_cast<const u8*>(&one) + sizeof(one));
    const u32 added = channel.id;
    model.animChannels.add(channel);
    REQUIRE_FALSE(document.clips.empty());
    SubTrack track;
    track.channel = added;
    track.times = {0.0f};
    track.values = channel.initValue;
    document.clips[0].containers[0].subTracks.push_back(track);
    std::get<BonePayload>(model.nodes.nodes[2].payload).gateMesh = 0;

    const Document read = viaWem(document);
    const Model& back = read.models[0];
    REQUIRE(back.meshes.size() == 3);
    CHECK(back.meshes[0].name == "part_1");
    CHECK(back.meshes[2].name == "part_4");
    CHECK(back.animChannels.find(added) == nullptr);
    CHECK(sectionChannelsOf(back, 0) == 1u);
    CHECK(sectionChannelsOf(back, 2) == 1u);
    CHECK(std::get<BonePayload>(back.nodes.nodes[2].payload).gateMesh == kInvalidIndex);
    requireReferencersResolve(read);
    CHECK(validationErrors(read) == 0u);
}

TEST_CASE("wem lod drop the setting survives a .wem round trip", "[wem][lod]") {
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(makeLadder());
    REQUIRE(imported.ok());
    Document document = std::move(*imported.value);
    LodExport& lods = document.models[0].lodExport;
    lods.generate = true;
    lods.ratios = {0.9f, 0.5f, 0.25f};
    lods.lockBorders = true;

    const Document read = viaWem(document);
    const LodExport& back = read.models[0].lodExport;
    CHECK(back.generate);
    CHECK(back.ratios == lods.ratios);
    CHECK(back.lockBorders);
    CHECK(back.sourceHadLevels);
}

TEST_CASE("wem lod drop RemoveMeshes erases a mesh's channels and remaps the rest",
          "[wem][lod]") {
    const MdxConverter converter;
    mdx::Model source = makeLadder();
    for (mdx::Geoset& geoset : source.geosets) {
        geoset.lod = 0;
    }
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());
    Document document = std::move(*imported.value);
    const u32 channels = static_cast<u32>(document.models[0].animChannels.channels.size());

    const std::array<u8, 5> drop{0, 1, 0, 0, 0};
    Diagnostics out;
    CHECK(RemoveMeshes(document, 0, drop, out) == 1u);
    const Model& model = document.models[0];
    REQUIRE(model.meshes.size() == 4);
    CHECK(model.meshes[0].name == "part_0");
    CHECK(model.meshes[1].name == "part_2");
    CHECK(model.animChannels.channels.size() == channels - 1);
    CHECK(sectionChannelsOf(model, 0) == 1u);
    CHECK(sectionChannelsOf(model, 3) == 1u);
    CHECK(std::get<BonePayload>(model.nodes.nodes[1].payload).gateMesh == kInvalidIndex);
    CHECK(out.countOf(DiagCode::AnimChannelInvalidated) == 0u);
    requireReferencersResolve(document);
    CHECK(validationErrors(document) == 0u);

    SECTION("nothing marked is nothing done") {
        const std::array<u8, 4> none{0, 0, 0, 0};
        Diagnostics quiet;
        CHECK(RemoveMeshes(document, 0, none, quiet) == 0u);
        CHECK(document.models[0].meshes.size() == 4);
        CHECK(quiet.empty());
    }
}

// ============================================================================
// The corpus arm
// ============================================================================

TEST_CASE("wem lod drop every shipped HD ladder drops cleanly", "[wem][lod][corpus]") {
    std::vector<test::fs::path> files =
        test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL/war3.w3mod/_hd.w3mod"});
    std::erase_if(files, [](const test::fs::path& path) {
        return test::pathText(path).find("_hd.w3mod") == std::string::npos;
    });
    if (files.empty()) {
        SKIP("HD corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 300);
    const MdxConverter converter;

    u32 read = 0;
    u32 withLevels = 0;
    u32 baseAfterLevel = 0;
    u32 meshesDropped = 0;
    u32 failures = 0;
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
        ++read;
        const std::string file = test::pathText(files[i].filename());

        // What the file holds, per level, and whether a LOD 0 geoset follows a
        // LOD n one (the files a drop without the remap got wrong).
        std::array<u64, 4> triangles{};
        std::vector<std::string> kept;
        bool anyBase = false;
        bool levelSeen = false;
        bool levelBeforeBase = false;
        for (const mdx::Geoset& geoset : source.geosets) {
            if (geoset.lod == kFileAllLods || geoset.lod == 0) {
                anyBase = true;
                kept.push_back(geoset.lodName);
                levelBeforeBase = levelBeforeBase || (geoset.lod == 0 && levelSeen);
            } else {
                levelSeen = true;
            }
            if (geoset.lod < 4) {
                triangles[geoset.lod] += geoset.faces.size() / 3;
            }
        }
        if (!levelSeen) {
            continue;
        }
        ++withLevels;
        baseAfterLevel += levelBeforeBase;

        const Model& model = document->models[0];
        const u32 before = static_cast<u32>(source.geosets.size());
        if (!anyBase) {
            CHECK(model.meshes.size() == before);
            continue;
        }
        meshesDropped += before - static_cast<u32>(model.meshes.size());
        bool ok = model.meshes.size() == kept.size();
        for (std::size_t m = 0; ok && m < kept.size(); ++m) {
            ok = model.meshes[m].lodLevel == 0 || model.meshes[m].lodLevel == kAllLods;
        }
        requireReferencersResolve(*document);
        for (const Node& node : model.nodes.nodes) {
            if (const NativeBag::Entry* raw = node.native.find("geosetId")) {
                const auto value = static_cast<u64>(raw->value);
                ok = ok && (value < model.meshes.size() || value == kFileAllLods || value >= before);
            }
        }
        const u32 errors = validationErrors(*document);
        ok = ok && errors == 0 && model.lodExport.sourceHadLevels && !model.lodExport.generate;
        // The ratios are the document's triangles, after the import's repair
        // dropped what it drops (degenerate faces, the duplicate side of a
        // double-sided card): the simplifier reduces those same meshes. So
        // they are measured on the same import with every geoset at LOD 0,
        // which drops nothing, geoset g becoming mesh g.
        mdx::Model flat = source;
        for (mdx::Geoset& geoset : flat.geosets) {
            geoset.lod = 0;
        }
        const Result<Document> undropped = converter.fromMdx(flat);
        std::array<u64, 4> imported{};
        if (undropped.ok() && !undropped->models.empty() &&
            undropped->models[0].meshes.size() == source.geosets.size()) {
            for (std::size_t g = 0; g < source.geosets.size(); ++g) {
                const u32 lod = source.geosets[g].lod;
                if (lod < 4) {
                    for (const u32 corners : undropped->models[0].meshes[g].faceSet().faceValence) {
                        imported[lod] += corners - 2;
                    }
                }
            }
        }
        for (u32 level = 1; ok && level < 4 && imported[0] != 0; ++level) {
            if (imported[level] == 0) {
                continue;
            }
            const f32 want = static_cast<f32>(imported[level]) / static_cast<f32>(imported[0]);
            ok = std::abs(model.lodExport.ratios[level - 1] - want) <= 1e-5f * want;
            triangles[level] = imported[level];
        }
        triangles[0] = imported[0];
        if (!ok) {
            ++failures;
            if (failures <= 10) {
                std::cout << "  " << file << ": " << model.meshes.size() << " meshes of "
                          << kept.size() << " kept, " << errors << " validation error(s), ratios";
                for (u32 level = 1; level < 4; ++level) {
                    std::cout << " " << model.lodExport.ratios[level - 1] << "/"
                              << (triangles[0] != 0 ? static_cast<f32>(triangles[level]) /
                                                          static_cast<f32>(triangles[0])
                                                    : 0.0f);
                }
                std::cout << " had=" << model.lodExport.sourceHadLevels << "\n";
            }
        }
    }

    std::cout << "LOD drop corpus arm: " << read << " files read, " << withLevels
              << " with levels, " << baseAfterLevel << " with a LOD 0 geoset after a LOD n one, "
              << meshesDropped << " meshes dropped\n";
    CHECK(read > 0);
    CHECK(failures == 0u);
}
