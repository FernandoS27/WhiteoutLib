// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Archives other tools write: Warcraft III maps a protector has worked on,
// names StormLib hashed with their slashes, and patch files.

#include <catch2/catch_all.hpp>

#include <whiteout/storages/mpq/storage.h>

#include "whiteout/storages/mpq/codecs/compression.h"
#include "whiteout/storages/mpq/writer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace mpq = whiteout::storages::mpq;

using whiteout::u16;
using whiteout::u32;
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

std::optional<std::vector<u8>> some(const std::string& text) {
    return bytesOf(text);
}

void putU32(std::vector<u8>& bytes, size_t at, u32 value) {
    std::memcpy(bytes.data() + at, &value, 4);
}

void putU16(std::vector<u8>& bytes, size_t at, u16 value) {
    std::memcpy(bytes.data() + at, &value, 2);
}

/// A fresh archive holding @p files (name, text), as bytes; @p scratch is
/// where it is saved on the way.
std::vector<u8> archiveOf(const fs::path& scratch, const std::vector<std::pair<std::string, std::string>>& files) {
    {
        auto storage = mpq::Storage::create();
        for (const auto& [name, text] : files)
            REQUIRE(storage.writeFile(name, bytesOf(text)));
        REQUIRE(storage.save(scratch.string()));
    }
    return readAll(scratch);
}

/// @p archive behind the 512-byte HM3W block the World Editor writes.
std::vector<u8> asMap(const std::vector<u8>& archive) {
    std::vector<u8> map(512, 0);
    std::memcpy(map.data(), "HM3W", 4);
    map.insert(map.end(), archive.begin(), archive.end());
    return map;
}

/// @p tail appended at the next 512-byte boundary, where a header search
/// looks.
void appendAligned(std::vector<u8>& bytes, const std::vector<u8>& tail) {
    bytes.resize((bytes.size() + 511) / 512 * 512, 0);
    bytes.insert(bytes.end(), tail.begin(), tail.end());
}

std::optional<mpq::Storage> openAs(const TempDir& dir, const std::string& name, const std::vector<u8>& bytes) {
    fs::path const path = dir.path / name;
    writeAll(path, bytes);
    return mpq::Storage::open(path.string());
}

// MPQ header fields, by offset from the header.
constexpr size_t kHeaderSizeAt = 4;
constexpr size_t kVersionAt = 12;
constexpr size_t kBlockCountAt = 28;
/// What the header size field of the corpus's protected maps holds.
constexpr u32 kJunkHeaderSize = 0x504F7856;

} // namespace

TEST_CASE("A protected map's junk header size and version are ignored, as Warcraft III ignores them",
          "[mpq][compat]") {
    TempDir dir("whiteout_mpq_compat_header");
    auto const map = asMap(archiveOf(dir.path / "plain.mpq", {{"war3map.j", "script"}, {"war3map.w3e", "terrain"}}));

    auto junkSize = map;
    putU32(junkSize, 512 + kHeaderSizeAt, kJunkHeaderSize);
    auto opened = openAs(dir, "size.w3x", junkSize);
    REQUIRE(opened.has_value());
    CHECK(opened->readFile("war3map.j") == some("script"));

    // A "version 2" header with a junk size and a block count running past
    // the end of the file: read as version 1, the table cut where the file
    // ends.
    auto fakeV2 = junkSize;
    putU16(fakeV2, 512 + kVersionAt, 1);
    putU32(fakeV2, 512 + kBlockCountAt, 0x41EF01);
    opened = openAs(dir, "v2.w3x", fakeV2);
    REQUIRE(opened.has_value());
    CHECK(opened->archiveInfo().formatVersion == 0);
    CHECK(opened->readFile("war3map.w3e") == some("terrain"));

    // It saves as a plain map, its HM3W block kept.
    REQUIRE(opened->writeFile("war3map.wts", bytesOf("strings")));
    REQUIRE(opened->save());
    opened.reset();
    auto saved = mpq::Storage::open((dir.path / "v2.w3x").string());
    REQUIRE(saved.has_value());
    CHECK(saved->readFile("war3map.j") == some("script"));
    CHECK(saved->readFile("war3map.wts") == some("strings"));
    saved.reset();
    CHECK(readAll(dir.path / "v2.w3x")[0] == 'H');
}

TEST_CASE("A protected map reads its own archive, not a decoy behind it", "[mpq][compat]") {
    TempDir dir("whiteout_mpq_compat_decoy");
    auto map = asMap(archiveOf(dir.path / "real.mpq", {{"war3map.j", "real script"}}));
    putU32(map, 512 + kHeaderSizeAt, kJunkHeaderSize);
    appendAligned(map, archiveOf(dir.path / "decoy.mpq", {{"war3map.j", "decoy"}}));

    auto opened = openAs(dir, "decoy.w3x", map);
    REQUIRE(opened.has_value());
    CHECK(opened->readFile("war3map.j") == some("real script"));
}

TEST_CASE("A map's user data is ignored, as Warcraft III never reads it", "[mpq][compat]") {
    TempDir dir("whiteout_mpq_compat_userdata");
    // User data at the start pointing at a decoy, the map's archive at 0x200.
    std::vector<u8> file(512, 0);
    std::memcpy(file.data(), "MPQ\x1B", 4);
    putU32(file, 4, 16);
    putU32(file, 12, 16);
    appendAligned(file, archiveOf(dir.path / "real.mpq", {{"war3map.j", "real script"}}));
    file.resize((file.size() + 511) / 512 * 512, 0);
    putU32(file, 8, static_cast<u32>(file.size()));
    appendAligned(file, archiveOf(dir.path / "decoy.mpq", {{"war3map.j", "decoy"}}));

    auto map = openAs(dir, "userdata.w3x", file);
    REQUIRE(map.has_value());
    CHECK(map->readFile("war3map.j") == some("real script"));

    // Outside a map, user data is honoured.
    auto archive = openAs(dir, "userdata.mpq", file);
    REQUIRE(archive.has_value());
    CHECK(archive->readFile("war3map.j") == some("decoy"));
}

TEST_CASE("A version 1 header whose tables lie past the end of the file is passed over", "[mpq][compat]") {
    TempDir dir("whiteout_mpq_compat_fake_header");
    std::vector<u8> file(512, 0);
    std::memcpy(file.data(), "HM3W", 4);
    std::vector<u8> fake(32, 0);
    std::memcpy(fake.data(), "MPQ\x1A", 4);
    putU32(fake, kHeaderSizeAt, 32);
    putU16(fake, 14, 3);                // Sector size shift.
    putU32(fake, 16, 0x7FFFFF00);       // Hash table offset.
    putU32(fake, 20, 0x7FFFFF00);       // Block table offset.
    putU32(fake, 24, 16);
    putU32(fake, kBlockCountAt, 1);
    file.insert(file.end(), fake.begin(), fake.end());
    appendAligned(file, archiveOf(dir.path / "real.mpq", {{"war3map.j", "real script"}}));

    for (const char* name : {"fake.w3x", "fake.mpq"}) {
        auto opened = openAs(dir, name, file);
        REQUIRE(opened.has_value());
        CHECK(opened->readFile("war3map.j") == some("real script"));
    }
}

TEST_CASE("A name StormLib hashed with its slashes reads, lists and saves", "[mpq][compat]") {
    TempDir dir("whiteout_mpq_compat_slashes");
    auto const model = bytesOf("model");
    auto const script = bytesOf("script");
    std::vector<mpq::WriteEntry> entries(2);
    entries[0].filename = "Units/Footman.mdx";
    entries[0].slashHashed = true;
    entries[0].rawData = model;
    entries[1].filename = "war3map.j";
    entries[1].rawData = script;
    mpq::WritePlan plan;
    plan.header = mpq::buildHeader(0, 16, 3);
    plan.hashTableCapacity = 16;
    auto const built = mpq::writeArchive(plan, entries);
    REQUIRE(built.error.empty());
    fs::path const path = dir.path / "slashes.mpq";
    writeAll(path, built.archive);
    writeAll(dir.path / "delete.mpq", built.archive);

    auto storage = mpq::Storage::open(path.string());
    REQUIRE(storage.has_value());
    CHECK(storage->readFile("Units/Footman.mdx") == some("model"));
    // To Storm, the name with a backslash is another name.
    CHECK_FALSE(storage->readFile("Units\\Footman.mdx").has_value());
    auto const listed = storage->listFiles();
    CHECK(std::find(listed.begin(), listed.end(), "Units/Footman.mdx") != listed.end());

    // A save keeps its name, spelled as it was hashed.
    REQUIRE(storage->writeFile("other.txt", bytesOf("other")));
    REQUIRE(storage->save());
    CHECK(storage->readFile("Units/Footman.mdx") == some("model"));
    auto const listfile = storage->readFile("(listfile)");
    REQUIRE(listfile.has_value());
    CHECK(std::string(listfile->begin(), listfile->end()).find("Units/Footman.mdx") != std::string::npos);

    // Written over, it is hashed as this library hashes, and no old copy is
    // left to answer for the slashed spelling.
    REQUIRE(storage->writeFile("Units/Footman.mdx", bytesOf("new model")));
    REQUIRE(storage->save());
    CHECK(storage->readFile("Units/Footman.mdx") == some("new model"));
    CHECK(storage->readFile("Units\\Footman.mdx") == some("new model"));

    // Deleted, it goes.
    auto other = mpq::Storage::open((dir.path / "delete.mpq").string());
    REQUIRE(other.has_value());
    REQUIRE(other->deleteFile("Units/Footman.mdx"));
    REQUIRE(other->save());
    CHECK_FALSE(other->readFile("Units/Footman.mdx").has_value());
    CHECK(other->readFile("war3map.j") == some("script"));
}

TEST_CASE("A patch file reads as its patch data, behind its patch info", "[mpq][compat]") {
    TempDir dir("whiteout_mpq_compat_patch");
    std::string text;
    for (int i = 0; i < 40; ++i)
        text += "PTCH patch data " + std::to_string(i) + "\n";
    auto const patch = bytesOf(text);
    // The patch info: its length, flags (0x80000000 = valid), the data size,
    // and an MD5.
    std::vector<u8> info(28, 0);
    putU32(info, 0, 28);
    putU32(info, 4, 0x80000000);
    putU32(info, 8, static_cast<u32>(patch.size()));

    auto stored = info;
    stored.insert(stored.end(), patch.begin(), patch.end());
    auto packed = info;
    auto const zipped = mpq::mpqCompress(patch, mpq::CompressionFlag::kZlib);
    REQUIRE_FALSE(zipped.empty());
    packed.insert(packed.end(), zipped.begin(), zipped.end());

    std::vector<mpq::WriteEntry> entries(2);
    entries[0].filename = "stored.bin";
    entries[0].rawCopy = true;
    entries[0].rawSectors = stored;
    entries[0].sourceBlock = {0, static_cast<u32>(stored.size()), static_cast<u32>(patch.size()),
                              mpq::FileFlag::kExists | mpq::FileFlag::kPatchFile};
    entries[1].filename = "packed.bin";
    entries[1].rawCopy = true;
    entries[1].rawSectors = packed;
    entries[1].sourceBlock = {0, static_cast<u32>(packed.size()), static_cast<u32>(patch.size()),
                              mpq::FileFlag::kExists | mpq::FileFlag::kPatchFile | mpq::FileFlag::kSingleUnit |
                                  mpq::FileFlag::kCompress};
    mpq::WritePlan plan;
    plan.header = mpq::buildHeader(1, 16, 3);
    auto const built = mpq::writeArchive(plan, entries);
    REQUIRE(built.error.empty());

    auto storage = openAs(dir, "patch.mpq", built.archive);
    REQUIRE(storage.has_value());
    CHECK(storage->readFile("stored.bin") == std::optional(patch));
    CHECK(storage->readFile("packed.bin") == std::optional(patch));

    // A save carries both as they are.
    REQUIRE(storage->writeFile("other.txt", bytesOf("other")));
    REQUIRE(storage->save());
    CHECK(storage->readFile("stored.bin") == std::optional(patch));
    CHECK(storage->readFile("packed.bin") == std::optional(patch));

    // A patch info that is not marked valid reads as nothing.
    auto broken = built.archive;
    auto const at = std::search(broken.begin(), broken.end(), info.begin(), info.begin() + 8);
    REQUIRE(at != broken.end());
    putU32(broken, static_cast<size_t>(at - broken.begin()) + 4, 0);
    auto bad = openAs(dir, "broken.mpq", broken);
    REQUIRE(bad.has_value());
    CHECK_FALSE(bad->readFile("stored.bin").has_value());
}
