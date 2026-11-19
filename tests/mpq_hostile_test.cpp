// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Archives built to break readers: header fields at the values Storm or
// StormLib refuse, and the ones protected maps rely on Storm accepting.

#include <catch2/catch_all.hpp>

#include <whiteout/storages/mpq/storage.h>

#include "whiteout/storages/mpq/crypto.h"
#include "whiteout/storages/mpq/tables/block_table.h"
#include "whiteout/storages/mpq/tables/hash_table.h"
#include "whiteout/storages/mpq/tables/header.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace mpq = whiteout::storages::mpq;

using whiteout::u16;
using whiteout::u32;
using whiteout::u8;

// The largest single allocation the process makes: a hostile archive must not
// make a reader set aside memory its data does not fill.
namespace {
std::atomic<size_t> g_largestAllocation{0};
} // namespace

void* operator new(size_t size) {
    size_t seen = g_largestAllocation.load();
    while (size > seen && !g_largestAllocation.compare_exchange_weak(seen, size)) {
    }
    void* p = std::malloc(size ? size : 1);
    if (!p)
        std::abort();
    return p;
}
void* operator new[](size_t size) {
    return operator new(size);
}
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    return operator new(size);
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    return operator new(size);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p, size_t) noexcept {
    std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}

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

/// Bytes that compress, spanning several 4 KiB sectors.
std::vector<u8> sectoredData() {
    std::vector<u8> data(20000);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<u8>((i * 7) % 13);
    return data;
}

/// Saves @p data as big.bin, then sets its block's claimed size to @p claimed
/// and, when given, the first word of its stored bytes to @p firstWord.
void claimSize(const fs::path& path, const std::vector<u8>& data, bool singleUnit, u32 claimed,
               std::optional<u32> firstWord) {
    {
        auto storage = mpq::Storage::create();
        mpq::WriteOptions opts;
        opts.singleUnit = singleUnit;
        REQUIRE(storage.writeFile("big.bin", data, opts));
        REQUIRE(storage.save(path.string()));
    }
    auto file = readAll(path);
    auto const at = mpq::findAndParseHeader(file);
    REQUIRE(at.has_value());
    mpq::HashTable hashes;
    REQUIRE(hashes.parse(std::span<const u8>(file).subspan(at->archiveOffset + at->header.hashTableOffset,
                                                           size_t{at->header.hashTableEntries} * 16),
                         at->header.hashTableEntries));
    size_t const blockAt = at->archiveOffset + at->header.blockTableOffset;
    mpq::BlockTable blocks;
    REQUIRE(blocks.parse(std::span<const u8>(file).subspan(blockAt, size_t{at->header.blockTableEntries} * 16),
                         at->header.blockTableEntries));
    auto const slot = hashes.lookup("big.bin");
    REQUIRE(slot.has_value());
    mpq::BlockEntry& block = blocks.entry(hashes.entry(*slot).blockIndex);
    REQUIRE(block.isCompressed());
    block.uncompressedSize = claimed;
    if (firstWord)
        std::memcpy(file.data() + at->archiveOffset + block.fileOffset, &*firstWord, 4);
    auto const sealed = blocks.serialize();
    std::copy(sealed.begin(), sealed.end(), file.begin() + static_cast<std::ptrdiff_t>(blockAt));
    writeAll(path, file);
}

} // namespace

TEST_CASE("A block claiming gigabytes for a few bytes fails without setting them aside", "[mpq][hostile]") {
    TempDir dir("whiteout_mpq_hostile_claim");
    fs::path const path = dir.path / "claim.mpq";
    auto const data = sectoredData();
    constexpr size_t kLimit = size_t{64} << 20;

    SECTION("a single-unit file") {
        claimSize(path, data, true, 0xF0000000u, std::nullopt);
    }
    SECTION("a sector count that wraps 32 bits") {
        // (0xFFFFFFFF + 4095) / 4096 wraps to 0 sectors in 32 bits: an offset
        // table of one word then passed, and 4 GiB of zeros came back.
        claimSize(path, data, false, 0xFFFFFFFFu, 4u);
    }
    auto storage = mpq::Storage::open(path.string());
    REQUIRE(storage.has_value());
    g_largestAllocation = 0;
    CHECK_FALSE(storage->readFile("big.bin").has_value());
    CHECK(g_largestAllocation.load() < kLimit);
}

TEST_CASE("A sector size shift that leaves no sector size is refused, not divided by", "[mpq][hostile]") {
    TempDir dir("whiteout_mpq_hostile_shift");
    fs::path const path = dir.path / "shift.mpq";
    auto const data = sectoredData();
    auto storage = mpq::Storage::create();
    REQUIRE(storage.writeFile("big.bin", data));
    REQUIRE(storage.save(path.string()));
    storage.close(); // Its mapping would keep the patches below off the disk.
    auto const base = readAll(path);

    auto withShift = [&](u16 shift) {
        auto bytes = base;
        std::memcpy(bytes.data() + 14, &shift, 2);
        writeAll(path, bytes);
        return mpq::Storage::open(path.string());
    };

    // 512 << 23 and up overflows 32 bits: StormLib refuses such an archive.
    for (u16 const shift : {u16{23}, u16{31}}) {
        CAPTURE(shift);
        CHECK_FALSE(withShift(shift).has_value());
    }
    // Storm shifts as x86 does, by the low five bits: 0x103 and 0x23 mean 3.
    for (u16 const shift : {u16{0x103}, u16{0x23}}) {
        CAPTURE(shift);
        auto opened = withShift(shift);
        REQUIRE(opened.has_value());
        CHECK(opened->archiveInfo().sectorSize == 4096);
        CHECK(opened->readFile("big.bin") == std::optional(data));
    }
}

TEST_CASE("Creating with a sector size shift that leaves no sector size fails the save", "[mpq][hostile]") {
    TempDir dir("whiteout_mpq_hostile_create_shift");
    mpq::CreateOptions opts;
    opts.sectorSizeShift = 23;
    auto storage = mpq::Storage::create(opts);
    REQUIRE(storage.writeFile("big.bin", sectoredData()));
    CHECK_FALSE(storage.save((dir.path / "bad.mpq").string()));
    CHECK_FALSE(storage.lastError().empty());
}
