// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P4 step 2 — forward compatibility (§11.4) and native-tag skippability
/// (§11.3).
///
/// These are one mechanism wearing two names. "A v3.0 tool opens a v3.2 file,
/// changes a material and saves without destroying what it never understood" and
/// "a wasm build compiled without D3 can still edit the WC3 sets of a mixed
/// document" are the same code path: an index entry this build cannot account
/// for is captured whole and put back where it was.
///
/// The part worth testing hardest is the *where*. A preserved chunk's bytes hold
/// `Reference`s of their own, and a `Reference` names an index-table **slot**, so
/// preservation that renumbers the table produces a file that parses and means
/// something else. The slot-numbering assertions below are the ones that would
/// catch that; a byte-equality check on the chunk alone would not.

#include <cstring>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/writer.h>

#include "test_helpers.h"
#include "whiteout/common/binary_reader.h"
#include "whiteout/common/binary_writer.h"
#include "whiteout/common/streams.h"
#include "whiteout/models/wem/binary_read_visitor.h"
#include "whiteout/models/wem/binary_write_visitor.h"
#include "whiteout/models/wem/text_dump.h"

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

std::vector<u8> writeDocument(const Document& document, std::span<const UnknownChunk> unknown = {},
                              std::vector<u32>* unreferenced = nullptr) {
    std::vector<u8> buffer;
    buffer.reserve(64 * 1024);
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    common::BinaryWriter writer(out);
    BinaryWriteVisitor visitor(writer);
    visitor.write(document, kCurrentVersion, unknown);
    if (unreferenced != nullptr) {
        *unreferenced = visitor.unreferencedUnknownSlots();
    }
    buffer.shrink_to_fit();
    return buffer;
}

struct ReadResult {
    Document document;
    std::vector<UnknownChunk> unknown;
    std::vector<std::string> issues;
};

ReadResult readDocument(const std::vector<u8>& bytes, u32 skipTag = 0) {
    common::span_streambuf streambuf(std::span<const u8>(bytes.data(), bytes.size()));
    std::istream in(&streambuf);
    common::BinaryReader reader(in);
    BinaryReadVisitor visitor(reader);
    if (skipTag != 0) {
        visitor.skipTag(skipTag);
    }
    ReadResult result;
    visitor.read(result.document, kCurrentVersion);
    result.unknown = visitor.unknownChunks();
    result.issues = visitor.issues();
    return result;
}

std::string dump(const Document& document) {
    std::ostringstream out;
    TextDump(out, "document", const_cast<Document&>(document));
    return out.str();
}

/// A D3 material with enough shape that its block is not trivially small — the
/// preserved bytes have to include the nested runs, not just the header.
native::D3Material makeD3Block() {
    native::D3Material block;
    block.sourceVersion = 260;
    block.uber.shaderMap.id = 4242;
    block.baseMaterial.id = 8921;

    native::D3TextureEntry diffuse;
    diffuse.type = 0;
    diffuse.texture.id = 100;
    diffuse.uvTransformMode = 2;
    block.uber.textures.push_back(diffuse);

    native::D3TextureEntry normal;
    normal.type = 1;
    normal.texture.id = 101;
    block.uber.textures.push_back(normal);

    native::D3TagValue tag;
    tag.tagId = 0xA000F;
    tag.value = 1;
    block.shaderParams.push_back(tag);
    return block;
}

/// A document with one WC3 material and one D3 material, so skipping `ND3_`
/// leaves something behind to edit.
Document makeMixedDocument() {
    Document document;
    document.name = "mixed";
    document.declare(ProfileId::Wc3Classic);
    document.declare(ProfileId::Diablo3);

    TextureRef texture;
    texture.key = TexturePath{"textures/base.blp"};
    texture.path = "textures/base.blp";
    document.textures.push_back(texture);

    Model model;
    model.name = "model0";
    model.addSlot("body");

    ProfileMaterialSet wc3;
    wc3.profile = ProfileId::Wc3Classic;
    wc3.looks.add("A", 0);
    Material classic;
    classic.name = "classic";
    classic.InitCommon().setKind(MaterialKind::Composite);
    classic.MutableCommon().composite()->layers.push_back(CompositeLayer{});
    wc3.materials.push_back(std::move(classic));
    wc3.resizeBindings(model.materialSlots.size());
    wc3.slotBindings[0].byLook[0] = 0;
    model.profileSets.push_back(std::move(wc3));

    ProfileMaterialSet d3;
    d3.profile = ProfileId::Diablo3;
    d3.looks.add("A", 0);
    Material diablo;
    diablo.name = "diablo";
    diablo.InitCommon().setKind(MaterialKind::LegacyDeferred);
    diablo.SetNativeAuthoritative(makeD3Block());
    d3.materials.push_back(std::move(diablo));
    d3.resizeBindings(model.materialSlots.size());
    d3.slotBindings[0].byLook[0] = 0;
    model.profileSets.push_back(std::move(d3));

    document.models.push_back(std::move(model));
    return document;
}

/// The index table of @p bytes, read straight out of the file.
std::vector<IndexEntry> indexTableOf(const std::vector<u8>& bytes) {
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::vector<IndexEntry> table(header.indexCount);
    std::memcpy(table.data(), bytes.data() + header.indexOffset,
                header.indexCount * sizeof(IndexEntry));
    return table;
}

const IndexEntry* entryWithTag(const std::vector<IndexEntry>& table, u32 tag) {
    for (const IndexEntry& entry : table) {
        if (entry.tag == tag) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("wem a build without D3 preserves the D3 set byte for byte",
          "[wem][format][v3][unknown]") {
    const Document original = makeMixedDocument();
    const std::vector<u8> full = writeDocument(original);

    const u32 d3Tag = ChunkTagTraits<native::D3Material>::value;
    const std::vector<IndexEntry> originalTable = indexTableOf(full);
    const IndexEntry* originalD3 = entryWithTag(originalTable, d3Tag);
    REQUIRE(originalD3 != nullptr);

    // --- the build without D3 ---
    ReadResult limited = readDocument(full, d3Tag);
    CHECK(limited.issues.empty());
    REQUIRE(limited.unknown.size() == 1);
    CHECK(limited.unknown[0].tag == d3Tag);

    // It read everything else: two profile sets, and the WC3 material intact.
    REQUIRE(limited.document.models.size() == 1);
    REQUIRE(limited.document.models[0].profileSets.size() == 2);
    const ProfileMaterialSet* wc3 = limited.document.models[0].setFor(ProfileId::Wc3Classic);
    REQUIRE(wc3 != nullptr);
    REQUIRE(wc3->materials.size() == 1);
    CHECK(wc3->materials[0].name == "classic");
    CHECK(wc3->materials[0].Common().kind() == MaterialKind::Composite);

    // The D3 material's *header* survived — only its native block was skipped.
    const ProfileMaterialSet* d3 = limited.document.models[0].setFor(ProfileId::Diablo3);
    REQUIRE(d3 != nullptr);
    REQUIRE(d3->materials.size() == 1);
    CHECK(d3->materials[0].name == "diablo");
    CHECK(!d3->materials[0].hasNative()); // this build has no D3 to hold

    // --- it edits something it does understand, and saves ---
    limited.document.models[0].profileSets[0].materials[0].name = "classic-edited";
    std::vector<u32> unreferenced;
    const std::vector<u8> rewritten =
        writeDocument(limited.document, limited.unknown, &unreferenced);

    // The D3 material's own reference still names the slot, so nothing is orphaned.
    CHECK(unreferenced.empty());

    // --- and a full build reads the D3 set back unchanged ---
    const ReadResult reread = readDocument(rewritten);
    CHECK(reread.issues.empty());
    CHECK(reread.unknown.empty());

    const ProfileMaterialSet* rereadD3 = reread.document.models[0].setFor(ProfileId::Diablo3);
    REQUIRE(rereadD3 != nullptr);
    REQUIRE(rereadD3->materials.size() == 1);
    REQUIRE(rereadD3->materials[0].hasNative());

    native::D3Material expected = makeD3Block();
    native::D3Material actual = std::get<native::D3Material>(rereadD3->materials[0].Native());
    std::ostringstream before;
    std::ostringstream after;
    TextDump(before, "d3", expected);
    TextDump(after, "d3", actual);
    CHECK(before.str() == after.str());

    // The edit landed.
    CHECK(reread.document.models[0].profileSets[0].materials[0].name == "classic-edited");

    // And the bytes of the block itself are identical, which is the claim.
    const std::vector<IndexEntry> rewrittenTable = indexTableOf(rewritten);
    const IndexEntry* rewrittenD3 = entryWithTag(rewrittenTable, d3Tag);
    REQUIRE(rewrittenD3 != nullptr);
    CHECK(rewrittenD3->count == originalD3->count);
    CHECK(rewrittenD3->version == originalD3->version);
    const std::size_t size = limited.unknown[0].data.size();
    CHECK(std::memcmp(full.data() + originalD3->offset, rewritten.data() + rewrittenD3->offset,
                      size) == 0);
}

TEST_CASE("wem a preserved chunk keeps its index-table slot", "[wem][format][v3][unknown]") {
    const std::vector<u8> full = writeDocument(makeMixedDocument());
    const u32 d3Tag = ChunkTagTraits<native::D3Material>::value;

    ReadResult limited = readDocument(full, d3Tag);
    REQUIRE(limited.unknown.size() == 1);
    const u32 originalSlot = limited.unknown[0].index;

    const std::vector<u8> rewritten = writeDocument(limited.document, limited.unknown);
    const std::vector<IndexEntry> table = indexTableOf(rewritten);
    REQUIRE(originalSlot < table.size());

    // The whole point: the slot number is the same one, because references
    // *inside* the preserved bytes name slots and cannot be rewritten.
    CHECK(table[originalSlot].tag == d3Tag);

    // And the material's reference points at it.
    const ReadResult reread = readDocument(rewritten);
    REQUIRE(reread.document.models[0].setFor(ProfileId::Diablo3)->materials[0].hasNative());
}

TEST_CASE("wem a chunk with a tag from the future survives a round trip",
          "[wem][format][v3][unknown]") {
    // Forge a chunk this build has never heard of by taking a real file and
    // relabelling one index entry. Cheaper and more honest than inventing a
    // writer for a format version that does not exist: the bytes on the other
    // end are real bytes, laid out the way a real writer lays them out.
    std::vector<u8> forged = writeDocument(makeMixedDocument());
    const u32 d3Tag = ChunkTagTraits<native::D3Material>::value;
    const u32 futureTag = kTag("ZZZZ");
    CHECK(!IsKnownChunkTag(futureTag));

    WEMHeader header{};
    std::memcpy(&header, forged.data(), sizeof(header));
    u32 slot = 0;
    for (u32 i = 0; i < header.indexCount; ++i) {
        IndexEntry entry{};
        const std::size_t at = header.indexOffset + i * sizeof(IndexEntry);
        std::memcpy(&entry, forged.data() + at, sizeof(entry));
        if (entry.tag == d3Tag) {
            entry.tag = futureTag;
            std::memcpy(forged.data() + at, &entry, sizeof(entry));
            slot = i;
            break;
        }
    }
    REQUIRE(slot != 0);

    ReadResult result = readDocument(forged);
    REQUIRE(result.unknown.size() == 1);
    CHECK(result.unknown[0].tag == futureTag);
    CHECK(result.unknown[0].index == slot);

    // The material that pointed at it reports the mismatch and keeps its header
    // rather than taking the document down.
    REQUIRE(result.issues.size() == 1);
    CHECK(result.issues[0].find("Tag mismatch") != std::string::npos);

    const std::vector<u8> rewritten = writeDocument(result.document, result.unknown);
    const std::vector<IndexEntry> table = indexTableOf(rewritten);
    REQUIRE(slot < table.size());
    CHECK(table[slot].tag == futureTag);

    const IndexEntry* forgedEntry = entryWithTag(indexTableOf(forged), futureTag);
    REQUIRE(forgedEntry != nullptr);
    CHECK(std::memcmp(forged.data() + forgedEntry->offset, rewritten.data() + table[slot].offset,
                      result.unknown[0].data.size()) == 0);
}

TEST_CASE("wem the runtime tag set agrees with the traits", "[wem][format][v3][unknown]") {
    // A tag missing from `kKnownChunkTags` is preserved as unknown *and* parsed
    // as known, so it would be written twice — once by its record and once
    // verbatim. Nothing else in the suite would notice.
    CHECK(IsKnownChunkTag(ChunkTagTraits<Document>::value));
    CHECK(IsKnownChunkTag(ChunkTagTraits<Model>::value));
    CHECK(IsKnownChunkTag(ChunkTagTraits<Mesh>::value));
    CHECK(IsKnownChunkTag(ChunkTagTraits<Material>::value));
    CHECK(IsKnownChunkTag(ChunkTagTraits<CompositeBody>::value));
    CHECK(IsKnownChunkTag(ChunkTagTraits<PbrDeferredBody>::value));
    CHECK(IsKnownChunkTag(ChunkTagTraits<native::MdxMaterial>::value));
    CHECK(IsKnownChunkTag(ChunkTagTraits<native::M2Material>::value));
    CHECK(IsKnownChunkTag(ChunkTagTraits<native::M3Material>::value));
    CHECK(IsKnownChunkTag(ChunkTagTraits<native::D3Material>::value));

    // A round trip of a fully-understood document must leave nothing over.
    const ReadResult result = readDocument(writeDocument(makeMixedDocument()));
    CHECK(result.unknown.empty());
    CHECK(result.issues.empty());
}

TEST_CASE("wem the public API preserves without being asked", "[wem][format][v3][unknown]") {
    // P5 moved §11.4's other half onto the document: the parser fills
    // `Document::unknownChunks` and the writer reads it, so a host that reads,
    // edits and writes preserves by doing nothing. The visitor-level cases above
    // prove the mechanism; this proves the mechanism is *reachable* through the
    // API a host actually calls, which is where the previous shape -- a span the
    // caller had to remember to pass back -- would silently drop everything.
    const std::vector<u8> original = writeDocument(makeMixedDocument());

    Parser parser;
    ReadOptions options;
    options.skipNativeKinds.push_back(NativeKind::D3);
    const std::optional<Document> read =
        parser.parse(std::span<const u8>(original.data(), original.size()), options);
    REQUIRE(read.has_value());
    REQUIRE_FALSE(read->unknownChunks.empty());
    CHECK(read->unknownChunks.size() == parser.unknownChunks().size());

    Document edited = *read;
    edited.name = "edited";

    Writer writer;
    const std::vector<u8> rewritten = writer.write(edited);
    REQUIRE_FALSE(rewritten.empty());

    Parser again;
    const std::optional<Document> reloaded =
        again.parse(std::span<const u8>(rewritten.data(), rewritten.size()), options);
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->name == "edited");
    // The skipped D3 block survived a read this build could not parse, an edit
    // it was not part of, and a write that never mentioned it.
    REQUIRE(reloaded->unknownChunks.size() == read->unknownChunks.size());
    CHECK(reloaded->unknownChunks[0].data == read->unknownChunks[0].data);
    CHECK(reloaded->unknownChunks[0].index == read->unknownChunks[0].index);

    // And clearing the vector is how a caller drops them -- an act, not an
    // omission.
    Document dropped = *read;
    dropped.unknownChunks.clear();
    Writer bare;
    const std::vector<u8> without = bare.write(dropped);
    Parser third;
    const std::optional<Document> reloadedBare =
        third.parse(std::span<const u8>(without.data(), without.size()), options);
    REQUIRE(reloadedBare.has_value());
    CHECK(reloadedBare->unknownChunks.empty());
}
