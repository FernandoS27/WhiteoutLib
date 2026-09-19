// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// `SubTrack::tcb` — TCB kept as the Hermite curve it produces, plus the three
/// numbers per key that chose its tangents (EDIT_MODE_ANIMATIONS_DESIGN.md §3.3).
///
/// Every consumer that ignores the field plays the same curve, so what is worth
/// testing is where the numbers live and where they are lost: a `.wem` keeps
/// them, a `STRK` written before them reads as "none", `Validate` holds their
/// size, and `.mdx` drops them while keeping the tangents -- and says so.

#include <cstring>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>

#include "whiteout/models/wem/chunk_tags.h"
#include "whiteout/models/wem/text_dump.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

template <class T>
mdx::Track<T> makeTrack(mdx::InterpolationType interp, std::vector<u32> times,
                        std::vector<T> values) {
    mdx::Track<T> track;
    track.isUsed = true;
    track.interpolationType = interp;
    track.keyCount = times.size();
    track.timestamps = std::move(times);
    track.keys_data = std::move(values);
    return track;
}

/// Stand and Walk, one bone whose translation is Hermite in both: three keys
/// in Stand, two in Walk.
mdx::Model makeModel() {
    mdx::Model model;
    model.version = 800;
    model.modelName = "tcb";
    for (const auto& [name, start] : {std::pair{"Stand", 0u}, std::pair{"Walk", 2000u}}) {
        mdx::Sequence sequence;
        sequence.name = name;
        sequence.intervalStart = start;
        sequence.intervalEnd = start + 1000;
        model.sequences.push_back(sequence);
    }
    mdx::Texture texture;
    texture.fileName = "textures/body.blp";
    model.textures.push_back(texture);
    mdx::Material material;
    mdx::Layer layer;
    layer.filterMode = mdx::Layer::FilterMode::None;
    layer.textureId = 0;
    layer.textureAnimationId = 0xFFFFFFFF;
    material.layers.push_back(layer);
    model.materials.push_back(material);

    mdx::Bone root;
    root.node.name = "root";
    root.node.objectId = 0;
    root.node.parentId = mdx::Node::NO_PARENT;
    const Vector3f zero{0, 0, 0};
    root.node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Hermite, {0, 500, 1000, 2000, 3000},
        {Vector3f{0, 0, 0}, zero, zero, Vector3f{0, 0, 4}, zero, zero, Vector3f{1, 0, 0}, zero, zero,
         Vector3f{2, 0, 0}, zero, zero, Vector3f{5, 0, 0}, zero, zero});
    model.bones.push_back(root);
    model.pivotPoints = {zero};

    mdx::Geoset geoset;
    geoset.lodName = "body";
    geoset.vertexPositions = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{1, 1, 0}};
    geoset.vertexNormals = {Vector3f{0, 0, 1}, Vector3f{0, 0, 1}, Vector3f{0, 0, 1}};
    geoset.textureCoordinateSets.push_back({Vector2f{0, 0}, Vector2f{1, 0}, Vector2f{1, 1}});
    geoset.faces = {0, 1, 2};
    geoset.materialId = 0;
    model.geosets.push_back(geoset);
    return model;
}

Document convert(const mdx::Model& model) {
    MdxConverter converter;
    Result<Document> result = converter.fromMdx(model);
    REQUIRE(result.ok());
    return std::move(*result.value);
}

/// The translation sub-track of the clip named @p name.
SubTrack* translationOf(Document& document, const std::string& name) {
    for (Clip& clip : document.clips) {
        if (clip.name != name) {
            continue;
        }
        const AnimChannelTable& table = document.models[clip.model].animChannels;
        for (SubTrackContainer& container : clip.containers) {
            for (SubTrack& track : container.subTracks) {
                const AnimChannel* channel = table.find(track.channel);
                if (channel != nullptr && channel->target.channel == Channel::Translation) {
                    return &track;
                }
            }
        }
    }
    return nullptr;
}

/// Tension, continuity and bias for each of @p track's keys, all different so a
/// read that shifts them by one is caught.
std::vector<f32> parametersFor(const SubTrack& track) {
    std::vector<f32> tcb;
    for (std::size_t k = 0; k < track.keyCount(); ++k) {
        tcb.push_back(0.1f * static_cast<f32>(k + 1));
        tcb.push_back(-0.2f * static_cast<f32>(k + 1));
        tcb.push_back(0.05f * static_cast<f32>(k + 1));
    }
    return tcb;
}

/// The document with both clips' translation keyed as TCB.
Document tcbDocument() {
    Document document = convert(makeModel());
    for (const char* name : {"Stand", "Walk"}) {
        SubTrack* track = translationOf(document, name);
        REQUIRE(track != nullptr);
        REQUIRE(track->interp == Interpolation::Hermite);
        track->tcb = parametersFor(*track);
    }
    return document;
}

std::optional<Document> reread(const std::vector<u8>& bytes) {
    Parser parser;
    return parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
}

} // namespace

TEST_CASE("wem a .wem round trip keeps a sub-track's TCB parameters", "[wem][anim][tcb]") {
    const Document document = tcbDocument();
    CHECK_FALSE(Validate(document, ValidateLevel::Structural).hasErrors());

    Writer writer;
    const std::vector<u8> bytes = writer.write(document);
    REQUIRE_FALSE(bytes.empty());
    std::optional<Document> read = reread(bytes);
    REQUIRE(read.has_value());
    for (const char* name : {"Stand", "Walk"}) {
        INFO(name);
        Document original = document;
        const SubTrack* was = translationOf(original, name);
        const SubTrack* now = translationOf(*read, name);
        REQUIRE(now != nullptr);
        CHECK(now->tcb == was->tcb);
        CHECK(now->times == was->times);
        CHECK(now->values == was->values);
    }
}

TEST_CASE("wem a sub-track written before TCB reads with none", "[wem][anim][tcb]") {
    // Every STRK chunk stamped version 1, as a writer from before the field
    // would have: the reader must stop before the trailing reference rather
    // than read it as parameters. Each chunk holds one sub-track, so what
    // follows in it is only that reference.
    Document document = tcbDocument();
    for (const Clip& clip : document.clips) {
        for (const SubTrackContainer& container : clip.containers) {
            REQUIRE(container.subTracks.size() == 1u);
        }
    }
    Writer writer;
    std::vector<u8> bytes = writer.write(document);
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    u32 stamped = 0;
    for (u32 i = 0; i < header.indexCount; ++i) {
        IndexEntry entry{};
        u8* at = bytes.data() + header.indexOffset + i * sizeof(IndexEntry);
        std::memcpy(&entry, at, sizeof(entry));
        if (entry.tag != ChunkTagTraits<SubTrack>::value) {
            continue;
        }
        CHECK(entry.version == 2u);
        entry.version = 1;
        std::memcpy(at, &entry, sizeof(entry));
        ++stamped;
    }
    REQUIRE(stamped == 2u);

    std::optional<Document> read = reread(bytes);
    REQUIRE(read.has_value());
    for (const char* name : {"Stand", "Walk"}) {
        INFO(name);
        const SubTrack* was = translationOf(document, name);
        const SubTrack* now = translationOf(*read, name);
        REQUIRE(now != nullptr);
        CHECK(now->tcb.empty());
        // Everything before the field is read as it was.
        CHECK(now->interp == Interpolation::Hermite);
        CHECK(now->times == was->times);
        CHECK(now->values == was->values);
    }
    CHECK_FALSE(Validate(*read, ValidateLevel::Structural).hasErrors());
}

TEST_CASE("wem the text dump shows a sub-track's TCB parameters", "[wem][anim][tcb]") {
    Document document = tcbDocument();
    std::ostringstream out;
    TextDump(out, "document", document);
    CHECK(out.str().find("tcb") != std::string::npos);
}

TEST_CASE("wem Validate holds TCB to three numbers per key and to Hermite", "[wem][anim][tcb]") {
    SECTION("one number short") {
        Document document = tcbDocument();
        translationOf(document, "Stand")->tcb.pop_back();
        const Diagnostics report = Validate(document, ValidateLevel::Structural);
        CHECK(report.countOf(DiagCode::AttributeCountMismatch) == 1u);
        CHECK(report.countOf(DiagCode::MixedInterpolationInTrack) == 0u);
    }
    SECTION("on a Linear sub-track") {
        Document document = tcbDocument();
        SubTrack& track = *translationOf(document, "Walk");
        // Linear, and sized for it: only the parameters are wrong.
        std::vector<u8> values;
        const std::size_t stride = 3 * sizeof(Vector3f);
        for (std::size_t k = 0; k < track.keyCount(); ++k) {
            values.insert(values.end(), track.values.begin() + static_cast<std::ptrdiff_t>(k * stride),
                          track.values.begin() + static_cast<std::ptrdiff_t>(k * stride + sizeof(Vector3f)));
        }
        track.values = std::move(values);
        track.interp = Interpolation::Linear;
        const Diagnostics report = Validate(document, ValidateLevel::Structural);
        CHECK(report.countOf(DiagCode::MixedInterpolationInTrack) == 1u);
        CHECK(report.countOf(DiagCode::AttributeCountMismatch) == 0u);
    }
    SECTION("none at all is fine") {
        Document document = tcbDocument();
        translationOf(document, "Stand")->tcb.clear();
        CHECK_FALSE(Validate(document, ValidateLevel::Structural).hasErrors());
    }
}

TEST_CASE("wem toMdx writes TCB as its Hermite tangents and says so once per channel",
          "[wem][anim][tcb][mdx]") {
    const Document plain = convert(makeModel());
    const Document tcb = tcbDocument();
    MdxConverter converter;
    const Result<mdx::Model> without = converter.toMdx(plain, ProfileId::Wc3Classic);
    const Result<mdx::Model> with = converter.toMdx(tcb, ProfileId::Wc3Classic);
    REQUIRE(without.ok());
    REQUIRE(with.ok());
    CHECK(without.diagnostics.countOf(DiagCode::AnimTcbBaked) == 0u);
    // Two clips key the channel with TCB; it is one channel, and one report.
    CHECK(with.diagnostics.countOf(DiagCode::AnimTcbBaked) == 1u);

    // The curve is what was on screen: Hermite, with the tangents the document
    // holds, exactly as without the parameters.
    const auto& a = without->bones[0].node.translationTracks;
    const auto& b = with->bones[0].node.translationTracks;
    CHECK(b.interpolationType == mdx::InterpolationType::Hermite);
    CHECK(b.timestamps == a.timestamps);
    REQUIRE(b.keys_data.size() == a.keys_data.size());
    for (std::size_t i = 0; i < a.keys_data.size(); ++i) {
        CHECK(b.keys_data[i] == a.keys_data[i]);
    }
}
