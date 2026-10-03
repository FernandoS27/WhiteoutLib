// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Saving over an archive someone else wrote: a Warcraft III map behind its
// HM3W block, files no listfile names, a file encrypted against its offset,
// and a StarCraft II v4 archive whose HET/BET tables and MD5s are rebuilt.

#include <catch2/catch_all.hpp>

#include <whiteout/storages/mpq/storage.h>

#include "whiteout/common/checksum.h"
#include "whiteout/storages/common/md5.h"
#include "whiteout/storages/mpq/crypto.h"
#include "whiteout/storages/mpq/special_files.h"
#include "whiteout/storages/mpq/tables/het_bet.h"
#include "whiteout/storages/mpq/writer.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace mpq = whiteout::storages::mpq;
namespace common = whiteout::storages::common;

using whiteout::u16;
using whiteout::u32;
using whiteout::u64;
using whiteout::u8;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) : path(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<u8> readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeAll(const fs::path& path, std::span<const u8> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<u8> bytesOf(const std::string& text) {
    return {text.begin(), text.end()};
}

/// The archive's parsed header and where it sits.
mpq::HeaderParseResult headerOf(std::span<const u8> file) {
    auto parsed = mpq::findAndParseHeader(file);
    REQUIRE(parsed.has_value());
    return *parsed;
}

/// The archive's hash table, decrypted.
mpq::HashTable hashTableOf(std::span<const u8> file, const mpq::HeaderParseResult& at) {
    mpq::HashTable table;
    REQUIRE(table.parse(file.subspan(at.archiveOffset + at.header.hashTableOffset,
                                     size_t{at.header.hashTableEntries} * 16),
                        at.header.hashTableEntries));
    return table;
}

mpq::BlockTable blockTableOf(std::span<const u8> file, const mpq::HeaderParseResult& at) {
    mpq::BlockTable table;
    REQUIRE(table.parse(file.subspan(at.archiveOffset + at.header.blockTableOffset,
                                     size_t{at.header.blockTableEntries} * 16),
                        at.header.blockTableEntries));
    return table;
}

/// How many hash slots file @p name under any locale.
int slotsNaming(const mpq::HashTable& table, const std::string& name) {
    u32 const a = mpq::hashString(name, mpq::HashType::NameA);
    u32 const b = mpq::hashString(name, mpq::HashType::NameB);
    int count = 0;
    for (u32 i = 0; i < table.capacity(); ++i)
        count += table.entry(i).isOccupied() && table.entry(i).hashA == a && table.entry(i).hashB == b;
    return count;
}

fs::path corpusFile(const char* relative) {
    for (const char* root : {"Corpus/", "../Corpus/", "../../Corpus/", "C:/Projects/WhiteoutLib/Corpus/"}) {
        fs::path const p = fs::path(root) / relative;
        std::error_code ec;
        if (fs::exists(p, ec))
            return p;
    }
    return {};
}

} // namespace

TEST_CASE("A save keeps the preamble, nameless files and re-keys a fixed-key file", "[mpq][update]") {
    TempDir dir("whiteout_mpq_update_test");

    // A v1 archive built straight through the writer: a stored file the save
    // deletes, so everything after it moves; two files encrypted against their
    // offsets — one named and single-unit, one compressed that no listfile
    // names — a nameless plain file, and a named one.
    std::vector<u8> const padding(100, 0x50);
    std::vector<u8> const secret = bytesOf("fixed key payload, sixteen+ bytes long");
    std::vector<u8> keyedPlain;
    for (int i = 0; i < 10000; ++i)
        keyedPlain.push_back(static_cast<u8>("keyed and compressed "[i % 21]));
    std::vector<u8> const hidden = bytesOf("no listfile line names this file");
    std::vector<u8> const named = bytesOf("named file");

    u32 const secretOffset = 32 + static_cast<u32>(padding.size());
    u32 const secretKey = (mpq::hashString("secret.txt", mpq::HashType::FileKey) + secretOffset) ^
                          static_cast<u32>(secret.size());
    std::vector<u8> sealed = secret;
    mpq::encryptBlock(reinterpret_cast<u32*>(sealed.data()), sealed.size() / 4, secretKey);

    // Compressed in 4096-byte sectors, then encrypted the way a FIX_KEY file is:
    // its sector table with key - 1, sector i with key + i.
    mpq::EncodeOptions packOptions;
    packOptions.sectorSize = 4096;
    mpq::EncodedFile packed = mpq::encodeFileData(keyedPlain, packOptions);
    REQUIRE(mpq::hasFlag(packed.flags, mpq::FileFlag::kCompress));
    u32 const keyedOffset = secretOffset + static_cast<u32>(sealed.size());
    u32 const keyedKey = (mpq::hashString("keyed.bin", mpq::HashType::FileKey) + keyedOffset) ^
                         static_cast<u32>(keyedPlain.size());
    u32 const sectors = (static_cast<u32>(keyedPlain.size()) + 4095) / 4096;
    std::vector<u32> sectorTable(sectors + 1);
    std::memcpy(sectorTable.data(), packed.data.data(), sectorTable.size() * 4);
    for (u32 i = 0; i < sectors; ++i) {
        u32 const words = (sectorTable[i + 1] - sectorTable[i]) / 4;
        std::vector<u32> sector(words);
        std::memcpy(sector.data(), packed.data.data() + sectorTable[i], size_t{words} * 4);
        mpq::encryptBlock(sector.data(), words, keyedKey + i);
        std::memcpy(packed.data.data() + sectorTable[i], sector.data(), size_t{words} * 4);
    }
    mpq::encryptBlock(sectorTable.data(), sectorTable.size(), keyedKey - 1);
    std::memcpy(packed.data.data(), sectorTable.data(), sectorTable.size() * 4);

    mpq::HashTable layout;
    layout.createEmpty(16);
    auto const keyedSlot = layout.insert("keyed.bin", 0, 2);
    auto const hiddenSlot = layout.insert("hidden.bin", 0, 3);
    REQUIRE(keyedSlot.has_value());
    REQUIRE(hiddenSlot.has_value());

    std::vector<mpq::WriteEntry> entries(5);
    entries[0].filename = "padding.bin";
    entries[0].rawCopy = true;
    entries[0].rawSectors = padding;
    entries[0].sourceBlock.compressedSize = static_cast<u32>(padding.size());
    entries[0].sourceBlock.uncompressedSize = static_cast<u32>(padding.size());
    entries[0].sourceBlock.flags = mpq::FileFlag::kExists;
    entries[1].filename = "secret.txt";
    entries[1].rawCopy = true;
    entries[1].rawSectors = sealed;
    entries[1].sourceBlock.compressedSize = static_cast<u32>(sealed.size());
    entries[1].sourceBlock.uncompressedSize = static_cast<u32>(secret.size());
    entries[1].sourceBlock.flags = mpq::FileFlag::kExists | mpq::FileFlag::kSingleUnit |
                                   mpq::FileFlag::kEncrypted | mpq::FileFlag::kFixKey;
    entries[1].crc32 = whiteout::crc32(secret.data(), secret.size());
    entries[2].hashA = mpq::hashString("keyed.bin", mpq::HashType::NameA);
    entries[2].hashB = mpq::hashString("keyed.bin", mpq::HashType::NameB);
    entries[2].sourceSlot = *keyedSlot;
    entries[2].rawCopy = true;
    entries[2].rawSectors = packed.data;
    entries[2].sourceBlock.compressedSize = packed.compressedSize;
    entries[2].sourceBlock.uncompressedSize = static_cast<u32>(keyedPlain.size());
    entries[2].sourceBlock.flags = packed.flags | mpq::FileFlag::kEncrypted | mpq::FileFlag::kFixKey;
    entries[3].hashA = mpq::hashString("hidden.bin", mpq::HashType::NameA);
    entries[3].hashB = mpq::hashString("hidden.bin", mpq::HashType::NameB);
    entries[3].sourceSlot = *hiddenSlot;
    entries[3].rawData = hidden;
    entries[4].filename = "named.txt";
    entries[4].rawData = named;

    mpq::WritePlan plan;
    plan.header = mpq::buildHeader(0, 16, 3);
    plan.hashTableCapacity = 16;
    plan.sourceHashTable = &layout;
    plan.attributeFlags = mpq::AttributeFlag::kCrc32;
    auto const built = mpq::writeArchive(plan, entries);
    REQUIRE(built.error.empty());

    // Behind a 512-byte HM3W block, the way the World Editor saves a map.
    std::vector<u8> map(512, 0);
    std::memcpy(map.data(), "HM3W", 4);
    std::memcpy(map.data() + 8, "Test Map", 8);
    map.insert(map.end(), built.archive.begin(), built.archive.end());
    fs::path const mapPath = dir.path / "test.w3x";
    writeAll(mapPath, map);

    auto storage = mpq::Storage::open(mapPath.string());
    REQUIRE(storage.has_value());
    CHECK(storage->readFile("secret.txt") == std::optional(secret));
    CHECK(storage->readFile("keyed.bin") == std::optional(keyedPlain));
    CHECK(storage->readFile("hidden.bin") == std::optional(hidden));
    REQUIRE(storage->deleteFile("padding.bin"));

    std::vector<u8> const added = bytesOf("war3mapImported\\added.mdx");
    REQUIRE(storage->writeFile("war3mapImported\\added.mdx", added));
    REQUIRE(storage->writeFile("NAMED.TXT", bytesOf("replaced")));
    // Twice: the second save starts from the first one's own (listfile).
    REQUIRE(storage->save());
    REQUIRE(storage->writeFile("second.txt", bytesOf("second")));
    INFO(storage->lastError());
    REQUIRE(storage->save());

    std::vector<u8> const saved = readAll(mapPath);
    REQUIRE(saved.size() > 512);
    CHECK(std::equal(map.begin(), map.begin() + 512, saved.begin()));
    mpq::HeaderParseResult const at = headerOf(saved);
    CHECK(at.archiveOffset == 512);

    auto reopened = mpq::Storage::open(mapPath.string());
    REQUIRE(reopened.has_value());
    CHECK(reopened->readFile("secret.txt") == std::optional(secret));
    CHECK(reopened->readFile("keyed.bin") == std::optional(keyedPlain));
    CHECK(reopened->readFile("hidden.bin") == std::optional(hidden));
    CHECK_FALSE(reopened->fileExists("padding.bin"));
    CHECK(reopened->readFile("named.txt") == std::optional(bytesOf("replaced")));
    CHECK(reopened->readFile("war3mapImported\\added.mdx") == std::optional(added));
    CHECK(reopened->readFile("second.txt") == std::optional(bytesOf("second")));

    // One (listfile), naming every named file once and nothing special.
    mpq::HashTable const table = hashTableOf(saved, at);
    CHECK(slotsNaming(table, "(listfile)") == 1);
    CHECK(slotsNaming(table, "(attributes)") == 1);
    CHECK(slotsNaming(table, "named.txt") == 1);
    auto files = reopened->listFiles();
    std::sort(files.begin(), files.end());
    CHECK(files == std::vector<std::string>{"NAMED.TXT", "second.txt", "secret.txt", "war3mapImported\\added.mdx"});

    // The attributes keep the source's arrays: CRC32 alone, one per block.
    auto const attributes = reopened->readFile("(attributes)");
    REQUIRE(attributes.has_value());
    mpq::BlockTable const blocks = blockTableOf(saved, at);
    u32 flags = 0;
    std::memcpy(&flags, attributes->data() + 4, 4);
    CHECK(flags == static_cast<u32>(mpq::AttributeFlag::kCrc32));
    CHECK(attributes->size() == 8 + size_t{blocks.count()} * 4);
    auto const parsed = mpq::parseAttributes(*attributes, blocks.count());
    auto const secretSlot = table.lookup("secret.txt");
    REQUIRE(secretSlot.has_value());
    CHECK(parsed.crc32s[table.entry(*secretSlot).blockIndex] == whiteout::crc32(secret.data(), secret.size()));
}

TEST_CASE("Writing a name replaces every locale of it", "[mpq][update]") {
    TempDir dir("whiteout_mpq_update_locale");
    auto storage = mpq::Storage::create();
    mpq::WriteOptions german;
    german.locale = mpq::Locale::German;
    REQUIRE(storage.writeFile("greeting.txt", bytesOf("hallo"), german));
    REQUIRE(storage.writeFile("greeting.txt", bytesOf("hello")));
    REQUIRE(storage.writeFile("other.txt", bytesOf("other")));
    fs::path const path = dir.path / "locale.mpq";
    REQUIRE(storage.save(path.string()));

    auto reopened = mpq::Storage::open(path.string());
    REQUIRE(reopened.has_value());
    CHECK(reopened->readFile("greeting.txt", mpq::Locale::German) == std::optional(bytesOf("hallo")));
    REQUIRE(reopened->writeFile("greeting.txt", bytesOf("replaced")));
    REQUIRE(reopened->save());

    // A localized copy of the old file would shadow the new one in that locale.
    auto last = mpq::Storage::open(path.string());
    REQUIRE(last.has_value());
    CHECK(last->readFile("greeting.txt") == std::optional(bytesOf("replaced")));
    CHECK_FALSE(last->readFile("greeting.txt", mpq::Locale::German).has_value());
    CHECK(last->readFile("other.txt") == std::optional(bytesOf("other")));
}

TEST_CASE("A table that has to grow refuses to drop a nameless file", "[mpq][update]") {
    mpq::HashTable layout;
    layout.createEmpty(4);
    auto const slot = layout.insert("hidden.bin", 0, 0);
    REQUIRE(slot.has_value());

    std::vector<mpq::WriteEntry> entries(4);
    entries[0].hashA = mpq::hashString("hidden.bin", mpq::HashType::NameA);
    entries[0].hashB = mpq::hashString("hidden.bin", mpq::HashType::NameB);
    entries[0].sourceSlot = *slot;
    entries[0].rawData = bytesOf("hidden");
    for (int i = 1; i < 4; ++i) {
        entries[i].filename = "file" + std::to_string(i);
        entries[i].rawData = bytesOf("data");
    }
    mpq::WritePlan plan;
    plan.header = mpq::buildHeader(0, 4, 3);
    plan.hashTableCapacity = 4;
    plan.sourceHashTable = &layout;
    auto const result = mpq::writeArchive(plan, entries);
    CHECK(result.archive.empty());
    CHECK_FALSE(result.error.empty());

    // With room to spare it keeps the slot.
    layout.createEmpty(16);
    entries[0].sourceSlot = *layout.insert("hidden.bin", 0, 0);
    plan.hashTableCapacity = 16;
    CHECK(mpq::writeArchive(plan, entries).error.empty());
}

TEST_CASE("A StarCraft II map saved unchanged rebuilds the HET and BET Blizzard wrote", "[mpq][update][corpus]") {
    fs::path const source = corpusFile("Conversions/Footman/test.SC2Map");
    if (source.empty())
        SKIP("Corpus/Conversions/Footman/test.SC2Map is not here");
    TempDir dir("whiteout_mpq_update_sc2");
    fs::path const copy = dir.path / "copy.SC2Map";
    fs::copy_file(source, copy);

    std::vector<u8> const before = readAll(copy);
    auto storage = mpq::Storage::open(copy.string());
    REQUIRE(storage.has_value());
    auto names = storage->listFiles();
    std::vector<std::vector<u8>> contents;
    for (const auto& name : names) {
        auto data = storage->readFile(name);
        REQUIRE(data.has_value());
        contents.push_back(std::move(*data));
    }
    INFO(storage->lastError());
    REQUIRE(storage->save());
    std::vector<u8> const after = readAll(copy);

    mpq::HeaderParseResult const was = headerOf(before);
    mpq::HeaderParseResult const now = headerOf(after);
    REQUIRE(now.header.formatVersion == 3);
    CHECK(now.header.headerSize == 208);
    CHECK(now.header.rawChunkSize == was.header.rawChunkSize);
    CHECK(now.header.blockTableEntries == was.header.blockTableEntries);

    auto const extTable = [](std::span<const u8> file, const mpq::HeaderParseResult& at, u64 offset, u64 size) {
        return file.subspan(at.archiveOffset + offset, size);
    };
    auto const hetWas = mpq::parseHetTable(extTable(before, was, was.header.hetTableOffset, was.header.hetTableSize64));
    auto const hetNow = mpq::parseHetTable(extTable(after, now, now.header.hetTableOffset, now.header.hetTableSize64));
    auto const betWas = mpq::parseBetTable(extTable(before, was, was.header.betTableOffset, was.header.betTableSize64));
    auto const betNow = mpq::parseBetTable(extTable(after, now, now.header.betTableOffset, now.header.betTableSize64));
    REQUIRE(hetWas.has_value());
    REQUIRE(hetNow.has_value());
    REQUIRE(betWas.has_value());
    REQUIRE(betNow.has_value());

    // Same names in the same block order: the HET is Blizzard's to the bit,
    // and so are the BET's layout and name hashes.
    CHECK(hetNow->totalCount == hetWas->totalCount);
    CHECK(hetNow->indexSizeTotal == hetWas->indexSizeTotal);
    CHECK(hetNow->nameHashes1 == hetWas->nameHashes1);
    CHECK(hetNow->betIndexes == hetWas->betIndexes);
    CHECK(betNow->unknown08 == betWas->unknown08);
    REQUIRE(betNow->entries.size() == betWas->entries.size());
    CHECK(mpq::blockNameHashes(*hetNow, *betNow) == mpq::blockNameHashes(*hetWas, *betWas));

    // Every block the BET states is the one the block table states.
    mpq::BlockTable const blocks = blockTableOf(after, now);
    for (u32 i = 0; i < blocks.count(); ++i) {
        const auto& e = betNow->entries[i];
        CHECK(e.filePos == blocks.entry(i).fileOffset);
        CHECK(e.fileSize == blocks.entry(i).uncompressedSize);
        CHECK(e.cmpSize == blocks.entry(i).compressedSize);
        CHECK(betNow->flags[e.flagIndex] == static_cast<u32>(blocks.entry(i).flags));
    }

    // The header's MD5s, and each file's chunk MD5s after its data.
    const auto& h = now.header;
    auto const stored = [&](u64 offset, u64 size) { return std::span<const u8>(after).subspan(now.archiveOffset + offset, size); };
    CHECK(common::md5Hash(stored(h.hashTableOffset, h.hashTableSize64)) == h.hashTableMd5);
    CHECK(common::md5Hash(stored(h.blockTableOffset, h.blockTableSize64)) == h.blockTableMd5);
    CHECK(common::md5Hash(stored(h.hetTableOffset, h.hetTableSize64)) == h.hetTableMd5);
    CHECK(common::md5Hash(stored(h.betTableOffset, h.betTableSize64)) == h.betTableMd5);
    CHECK(common::md5Hash(std::span<const u8>(after).subspan(now.archiveOffset, 192)) == h.mpqHeaderMd5);
    for (u32 i = 0; i < blocks.count(); ++i) {
        const auto& be = blocks.entry(i);
        INFO("block " << i << " of " << blocks.count() << " flags " << static_cast<u32>(be.flags) << " cmp " << be.compressedSize);
        if (be.compressedSize == 0)
            continue; // no chunks, so no MD5s
        auto const data = stored(be.fileOffset, be.compressedSize);
        auto const tail = stored(u64{be.fileOffset} + be.compressedSize, 16);
        CHECK(common::md5Hash(data.subspan(0, std::min<size_t>(data.size(), h.rawChunkSize))) ==
              *reinterpret_cast<const std::array<u8, 16>*>(tail.data()));
    }

    // Every file reads back as it was, its attributes stating its content.
    auto reopened = mpq::Storage::open(copy.string());
    REQUIRE(reopened.has_value());
    auto const attributes = reopened->readFile("(attributes)");
    REQUIRE(attributes.has_value());
    auto const parsed = mpq::parseAttributes(*attributes, blocks.count());
    REQUIRE(parsed.crc32s.size() == blocks.count());
    REQUIRE(parsed.md5s.size() == blocks.count());
    mpq::HashTable const table = hashTableOf(after, now);
    for (size_t i = 0; i < names.size(); ++i) {
        auto data = reopened->readFile(names[i]);
        REQUIRE(data.has_value());
        CHECK(*data == contents[i]);
        u32 const block = table.entry(*table.lookup(names[i])).blockIndex;
        INFO(names[i] << " block " << block);
        CHECK(parsed.crc32s[block] == whiteout::crc32(data->data(), data->size()));
        // An empty file's MD5 is zero, as Blizzard writes it.
        CHECK(parsed.md5s[block] == (data->empty() ? std::array<u8, 16>{} : common::md5Hash(*data)));
    }

    // A file added later is found through the HET as well as the hash table.
    std::vector<u8> const texture(70000, 0x5A);
    REQUIRE(reopened->writeFile("Assets\\Textures\\Committed.dds", texture));
    REQUIRE(reopened->save());
    std::vector<u8> const grown = readAll(copy);
    mpq::HeaderParseResult const last = headerOf(grown);
    auto const het = mpq::parseHetTable(extTable(grown, last, last.header.hetTableOffset, last.header.hetTableSize64));
    auto const bet = mpq::parseBetTable(extTable(grown, last, last.header.betTableOffset, last.header.betTableSize64));
    REQUIRE(het.has_value());
    REQUIRE(bet.has_value());
    mpq::HashTable const grownTable = hashTableOf(grown, last);
    auto const slot = grownTable.lookup("Assets\\Textures\\Committed.dds");
    REQUIRE(slot.has_value());
    CHECK(mpq::blockNameHashes(*het, *bet)[grownTable.entry(*slot).blockIndex] ==
          mpq::hetNameHash("Assets\\Textures\\Committed.dds"));
    auto final = mpq::Storage::open(copy.string());
    REQUIRE(final.has_value());
    CHECK(final->readFile("Assets\\Textures\\Committed.dds") == std::optional(texture));
}

TEST_CASE("A Warcraft III map saved unchanged keeps every file and its CRC-only attributes", "[mpq][update][corpus]") {
    fs::path const source = corpusFile("MDL/WoWTest/test.w3m");
    if (source.empty())
        SKIP("Corpus/MDL/WoWTest/test.w3m is not here");
    TempDir dir("whiteout_mpq_update_w3m");
    fs::path const copy = dir.path / "copy.w3m";
    fs::copy_file(source, copy);

    auto storage = mpq::Storage::open(copy.string());
    REQUIRE(storage.has_value());
    auto const names = storage->listFiles();
    REQUIRE(names.size() > 10);
    std::vector<std::vector<u8>> contents;
    for (const auto& name : names)
        contents.push_back(storage->readFile(name).value_or(std::vector<u8>{}));
    INFO(storage->lastError());
    REQUIRE(storage->save());

    auto reopened = mpq::Storage::open(copy.string());
    REQUIRE(reopened.has_value());
    for (size_t i = 0; i < names.size(); ++i)
        CHECK(reopened->readFile(names[i]) == std::optional(contents[i]));
    std::vector<u8> const after = readAll(copy);
    mpq::HeaderParseResult const at = headerOf(after);
    CHECK(at.header.formatVersion == 0);
    auto const attributes = reopened->readFile("(attributes)");
    REQUIRE(attributes.has_value());
    u32 flags = 0;
    std::memcpy(&flags, attributes->data() + 4, 4);
    CHECK(flags == static_cast<u32>(mpq::AttributeFlag::kCrc32));
    CHECK(attributes->size() == 8 + size_t{at.header.blockTableEntries} * 4);
}
