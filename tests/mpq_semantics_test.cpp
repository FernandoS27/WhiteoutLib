// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// What a name means: which locale and which of several entries a read
// returns (as Storm and StormLib pick it), how pending writes and deletes
// read before a save, and where a save may write.

#include <catch2/catch_all.hpp>

#include <whiteout/storages/mpq/storage.h>

#include "whiteout/storages/mpq/crypto.h"
#include "whiteout/storages/mpq/tables/hash_table.h"
#include "whiteout/storages/mpq/tables/header.h"

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

/// Saves @p files (name, locale, text) as a fresh archive at @p path.
void makeArchive(const fs::path& path, const std::vector<std::tuple<std::string, u16, std::string>>& files) {
    auto storage = mpq::Storage::create();
    for (const auto& [name, locale, text] : files) {
        mpq::WriteOptions opts;
        opts.locale = locale;
        REQUIRE(storage.writeFile(name, bytesOf(text), opts));
    }
    REQUIRE(storage.save(path.string()));
}

/// Adds a hash entry naming @p name to the end of its probe chain, pointing
/// at the block that entry @p donor's slot points at: the way a protector
/// plants a second entry for one name.
void addEntry(const fs::path& path, const std::string& name, const std::string& donor, u16 locale,
              u16 platform = 0, std::optional<u32> blockOverride = std::nullopt) {
    auto file = readAll(path);
    auto at = mpq::findAndParseHeader(file);
    REQUIRE(at.has_value());
    size_t const offset = at->archiveOffset + at->header.hashTableOffset;
    u32 const capacity = at->header.hashTableEntries;
    mpq::HashTable table;
    REQUIRE(table.parse(std::span<const u8>(file).subspan(offset, size_t{capacity} * 16), capacity));
    auto const donorSlot = table.lookup(donor);
    REQUIRE(donorSlot.has_value());
    u32 slot = mpq::hashString(name, mpq::HashType::TableOffset) & (capacity - 1);
    while (!table.entry(slot).isEmpty())
        slot = (slot + 1) & (capacity - 1);
    mpq::HashEntry& e = table.entry(slot);
    e.hashA = mpq::hashString(name, mpq::HashType::NameA);
    e.hashB = mpq::hashString(name, mpq::HashType::NameB);
    e.locale = locale;
    e.platform = platform;
    e.blockIndex = blockOverride.value_or(table.entry(*donorSlot).blockIndex);
    auto const sealed = table.serialize();
    std::copy(sealed.begin(), sealed.end(), file.begin() + static_cast<std::ptrdiff_t>(offset));
    writeAll(path, file);
}

} // namespace

TEST_CASE("A name reads the last neutral entry of its chain, as the game does", "[mpq][semantics]") {
    TempDir dir("whiteout_mpq_semantics_decoy");
    fs::path const path = dir.path / "decoy.mpq";
    makeArchive(path, {{"x.txt", 0, "decoy"}, {"real.bin", 0, "real"}});
    addEntry(path, "x.txt", "real.bin", 0);

    auto storage = mpq::Storage::open(path.string());
    REQUIRE(storage.has_value());
    CHECK(storage->readFile("x.txt") == some("real"));
    CHECK(storage->readFile("x.txt", mpq::Locale::English) == some("real"));
    CHECK(storage->fileInfo("x.txt")->uncompressedSize == 4);

    // An entry whose block index runs past the block table is no candidate.
    addEntry(path, "x.txt", "real.bin", 0, 0, 999);
    auto again = mpq::Storage::open(path.string());
    REQUIRE(again.has_value());
    CHECK(again->readFile("x.txt") == some("real"));
}

TEST_CASE("A locale reads its own entry, else the neutral one", "[mpq][semantics]") {
    TempDir dir("whiteout_mpq_semantics_locale");
    fs::path const path = dir.path / "locale.mpq";
    makeArchive(path, {{"both.txt", 0, "neutral"},
                       {"both.txt", mpq::Locale::German, "german"},
                       {"neutral.txt", 0, "only neutral"},
                       {"german.txt", mpq::Locale::German, "only german"}});

    auto storage = mpq::Storage::open(path.string());
    REQUIRE(storage.has_value());
    CHECK(storage->readFile("both.txt") == some("neutral"));
    CHECK(storage->readFile("both.txt", mpq::Locale::German) == some("german"));
    CHECK(storage->readFile("both.txt", mpq::Locale::French) == some("neutral"));
    CHECK(storage->readFile("neutral.txt", mpq::Locale::German) == some("only neutral"));
    // Without a locale only a neutral entry answers, as in StormLib.
    CHECK_FALSE(storage->readFile("german.txt").has_value());
    CHECK_FALSE(storage->fileExists("german.txt"));
    CHECK_FALSE(storage->fileInfo("german.txt").has_value());
    CHECK(storage->readFile("german.txt", mpq::Locale::German) == some("only german"));
    CHECK_FALSE(storage->readFile("german.txt", mpq::Locale::French).has_value());
}

TEST_CASE("An entry for another platform is never read", "[mpq][semantics]") {
    TempDir dir("whiteout_mpq_semantics_platform");
    fs::path const path = dir.path / "platform.mpq";
    makeArchive(path, {{"donor.bin", 0, "other platform"}});
    addEntry(path, "p.txt", "donor.bin", 0, 1);

    auto storage = mpq::Storage::open(path.string());
    REQUIRE(storage.has_value());
    CHECK_FALSE(storage->readFile("p.txt").has_value());
    CHECK_FALSE(storage->fileExists("p.txt"));
}

TEST_CASE("Pending writes and deletes read as the saved archive will", "[mpq][semantics]") {
    TempDir dir("whiteout_mpq_semantics_overlay");
    fs::path const path = dir.path / "overlay.mpq";
    makeArchive(path, {{"a.txt", 0, "old neutral"}, {"b.txt", 0, "old b"}});

    auto storage = mpq::Storage::open(path.string());
    REQUIRE(storage.has_value());
    mpq::WriteOptions german;
    german.locale = mpq::Locale::German;
    // Writing a name drops every source copy of it at save, so the old
    // neutral copy is gone already.
    REQUIRE(storage->writeFile("a.txt", bytesOf("new german"), german));
    CHECK_FALSE(storage->readFile("a.txt").has_value());
    CHECK_FALSE(storage->fileExists("a.txt"));
    CHECK(storage->readFile("a.txt", mpq::Locale::German) == some("new german"));
    CHECK_FALSE(storage->readFile("a.txt", mpq::Locale::French).has_value());

    // A delete takes every locale, pending ones included.
    REQUIRE(storage->writeFile("b.txt", bytesOf("new b german"), german));
    REQUIRE(storage->deleteFile("b.txt"));
    CHECK_FALSE(storage->readFile("b.txt", mpq::Locale::German).has_value());
    CHECK_FALSE(storage->deleteFile("b.txt"));

    REQUIRE(storage->save());
    CHECK_FALSE(storage->readFile("a.txt").has_value());
    CHECK(storage->readFile("a.txt", mpq::Locale::German) == some("new german"));
    CHECK_FALSE(storage->readFile("b.txt", mpq::Locale::German).has_value());
    CHECK_FALSE(storage->readFile("b.txt").has_value());
}

TEST_CASE("Saving to another spelling of the source path goes through the temp file", "[mpq][semantics]") {
    TempDir dir("whiteout_mpq_semantics_alias");
    fs::path const path = dir.path / "alias.mpq";
    auto preamble = std::vector<u8>(512, 0x48);
    makeArchive(path, {{"keep.txt", 0, "kept"}});
    auto bytes = readAll(path);
    bytes.insert(bytes.begin(), preamble.begin(), preamble.end());
    writeAll(path, bytes);

    auto storage = mpq::Storage::open(path.string());
    REQUIRE(storage.has_value());
    REQUIRE(storage->writeFile("new.txt", bytesOf("added")));
    std::string const other = (dir.path / "." / "alias.mpq").string();
    REQUIRE(storage->save(other));
    CHECK(storage->readFile("keep.txt") == some("kept"));
    CHECK(storage->readFile("new.txt") == some("added"));

    auto reopened = mpq::Storage::open(path.string());
    REQUIRE(reopened.has_value());
    CHECK(reopened->readFile("new.txt") == some("added"));
    auto const saved = readAll(path);
    CHECK(std::equal(preamble.begin(), preamble.end(), saved.begin()));
}

TEST_CASE("A name written with slashes is listed as it was hashed", "[mpq][semantics]") {
    // Storm and StormLib hash '/' as itself: a (listfile) line with slashes
    // names no file of an archive that hashed the name with backslashes.
    TempDir dir("whiteout_mpq_semantics_slash");
    fs::path const path = dir.path / "slash.mpq";
    makeArchive(path, {{"Units/Human/Footman.mdx", 0, "model"}, {"war3map.j", 0, "script"}});

    auto storage = mpq::Storage::open(path.string());
    REQUIRE(storage.has_value());
    auto const listfile = storage->readFile("(listfile)");
    REQUIRE(listfile.has_value());
    std::string const text(listfile->begin(), listfile->end());
    CHECK(text.find("Units\\Human\\Footman.mdx") != std::string::npos);
    CHECK(text.find('/') == std::string::npos);
    CHECK(storage->readFile("Units/Human/Footman.mdx") == some("model"));
}

TEST_CASE("A save past 128 MiB streams to disk and reads back", "[mpq][semantics]") {
    TempDir dir("whiteout_mpq_semantics_spill");
    fs::path const path = dir.path / "big.mpq";
    std::vector<u8> big((size_t{129} << 20) + 12345);
    for (size_t i = 0; i < big.size(); i += 4096)
        big[i] = static_cast<u8>(i >> 12);
    mpq::WriteOptions stored;
    stored.compression = mpq::Compression::None;
    {
        auto storage = mpq::Storage::create();
        REQUIRE(storage.writeFile("small.txt", bytesOf("before the big one")));
        REQUIRE(storage.writeFile("zbig.bin", big, stored));
        REQUIRE(storage.save(path.string()));
        CHECK(storage.lastError().empty());
    }
    auto reopened = mpq::Storage::open(path.string());
    REQUIRE(reopened.has_value());
    CHECK(reopened->readFile("small.txt") == some("before the big one"));
    CHECK(reopened->readFile("zbig.bin") == std::optional(big));
    CHECK(reopened->listFiles().size() == 2);
}

TEST_CASE("A save leaves a file of its temp name alone", "[mpq][semantics]") {
    TempDir dir("whiteout_mpq_semantics_temp");
    fs::path const path = dir.path / "temp.mpq";
    makeArchive(path, {{"a.txt", 0, "a"}});
    fs::path const bystander = dir.path / "temp.mpq.tmp";
    writeAll(bystander, bytesOf("not yours"));

    auto storage = mpq::Storage::open(path.string());
    REQUIRE(storage.has_value());
    REQUIRE(storage->writeFile("b.txt", bytesOf("b")));
    REQUIRE(storage->save());
    CHECK(readAll(bystander) == bytesOf("not yours"));
    CHECK(storage->readFile("b.txt") == some("b"));
}
