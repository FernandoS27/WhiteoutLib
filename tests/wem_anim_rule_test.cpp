// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// A clip's read rule (WEM_ANIMATION_RUNTIME_DESIGN.md §3.1): stored from CLIP
/// v4, and derived once from the import's markers for a clip written before.

#include <cstring>
#include <sstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/anim/clip.h>
#include <whiteout/models/wem/document.h>

#include "whiteout/common/binary_reader.h"
#include "whiteout/common/binary_writer.h"
#include "whiteout/common/streams.h"
#include "whiteout/models/wem/binary_read_visitor.h"
#include "whiteout/models/wem/binary_write_visitor.h"
#include "whiteout/models/wem/chunk_tags.h"

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

std::vector<u8> writeDocument(const Document& document) {
    std::vector<u8> buffer;
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    common::BinaryWriter writer(out);
    BinaryWriteVisitor visitor(writer);
    visitor.write(document, kCurrentVersion, {});
    return buffer;
}

Document readDocument(const std::vector<u8>& bytes) {
    common::span_streambuf streambuf(std::span<const u8>(bytes.data(), bytes.size()));
    std::istream in(&streambuf);
    common::BinaryReader reader(in);
    BinaryReadVisitor visitor(reader);
    Document document;
    visitor.read(document, kCurrentVersion);
    return document;
}

/// Stamps the CLIP chunk of @p bytes as @p version. The rule is a clip's last
/// field, so a v3 reader of a one-clip chunk stops before it and the rest reads
/// as written. (Clips share one chunk, packed, so a document of several would
/// misread every clip after the first: a real v3 file has no byte to skip.)
void stampClips(std::vector<u8>& bytes, u32 version) {
    WEMHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    for (u32 e = 0; e < header.indexCount; ++e) {
        IndexEntry entry;
        u8* at = bytes.data() + header.indexOffset + e * sizeof(IndexEntry);
        std::memcpy(&entry, at, sizeof(entry));
        if (entry.tag == ChunkTagTraits<Clip>::value) {
            entry.version = version;
            std::memcpy(at, &entry, sizeof(entry));
        }
    }
}

Clip clipWith(const char* marker, i64 value, ReadRule stored) {
    Clip clip;
    clip.name = marker;
    clip.model = 0;
    clip.duration = 1.0f;
    clip.containers.emplace_back();
    clip.native.set(marker, value);
    clip.readRule = stored;
    return clip;
}

Document documentWith(std::vector<Clip> clips) {
    Document document;
    document.models.emplace_back();
    document.clips = std::move(clips);
    return document;
}

} // namespace

TEST_CASE("wem a clip written before v4 derives its rule from its import's markers",
          "[wem][anim][rule]") {
    // Stored rules that the derivation must override, so a pass proves the
    // rule was derived rather than read.
    struct Case {
        const char* marker;
        i64 value;
        ReadRule stored;
        ReadRule derived;
    };
    const Case cases[] = {
        {"intervalStart", 0, ReadRule::Sc2, ReadRule::Wc3},
        {"globalSequenceId", 0, ReadRule::Sc2, ReadRule::Wc3},
        {"sequenceId", 7, ReadRule::Wc3, ReadRule::Sc2},
        {"startFrame", 1000, ReadRule::Wow, ReadRule::Sc2},
        {"animationId", 4, ReadRule::Wc3, ReadRule::Wow},
        {"globalLoop", 0, ReadRule::Sc2, ReadRule::Wow},
        {"moveSpeed", 270000, ReadRule::Sc2, ReadRule::Wc3},
    };
    for (const Case& c : cases) {
        INFO(c.marker);
        std::vector<u8> bytes = writeDocument(documentWith({clipWith(c.marker, c.value, c.stored)}));
        stampClips(bytes, 3);
        const Document read = readDocument(bytes);
        REQUIRE(read.clips.size() == 1);
        CHECK(read.clips[0].readRule == c.derived);
        // Everything before the rule reads as it was written.
        CHECK(read.clips[0].native.value(c.marker, -1) == c.value);
        CHECK(read.clips[0].duration == 1.0f);
    }
}

TEST_CASE("wem a v4 clip keeps the rule it was written with", "[wem][anim][rule]") {
    // A clip made under StarCraft II has no marker at all: only the stored
    // rule says how its keys are read.
    Document document = documentWith({
        clipWith("none", 0, ReadRule::Sc2),
        clipWith("intervalStart", 0, ReadRule::Wow),
        clipWith("none", 0, ReadRule::Wc3),
    });
    const Document read = readDocument(writeDocument(document));
    REQUIRE(read.clips.size() == 3);
    CHECK(read.clips[0].readRule == ReadRule::Sc2);
    CHECK(read.clips[1].readRule == ReadRule::Wow);
    CHECK(read.clips[2].readRule == ReadRule::Wc3);
}

TEST_CASE("wem a clip made for a game reads by that game's rule", "[wem][anim][rule]") {
    CHECK(RuleOf(GameOf(ProfileId::Generic)) == ReadRule::Wc3);
    CHECK(RuleOf(GameOf(ProfileId::Wc3Classic)) == ReadRule::Wc3);
    CHECK(RuleOf(GameOf(ProfileId::Wc3Reforged)) == ReadRule::Wc3);
    CHECK(RuleOf(GameOf(ProfileId::Sc2)) == ReadRule::Sc2);
    CHECK(RuleOf(GameOf(ProfileId::Heroes)) == ReadRule::Sc2);
    CHECK(RuleOf(GameOf(ProfileId::Wow)) == ReadRule::Wow);
    CHECK(RuleOf(GameOf(ProfileId::Diablo3)) == ReadRule::Wc3);
}
