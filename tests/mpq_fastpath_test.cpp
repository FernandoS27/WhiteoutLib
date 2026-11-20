// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// The paths that make reads and saves fast must give what the plain ones
// give: sectors decoded straight into the file's buffer, work shared with a
// pool in runs and in batches, checksums taken where a file is encoded.

#include <catch2/catch_all.hpp>

#include <whiteout/storages/mpq/storage.h>
#include <whiteout/utils/simple_thread_pool.h>

#include "whiteout/common/checksum.h"
#include "whiteout/storages/common/codecs/bzip2.h"
#include "whiteout/storages/common/inflate_fast.h"
#include "whiteout/storages/common/md5.h"
#include "whiteout/storages/common/zlib.h"
#include "whiteout/storages/mpq/codecs/adpcm.h"
#include "whiteout/storages/mpq/codecs/compression.h"
#include "whiteout/storages/mpq/codecs/huffman.h"
#include "whiteout/storages/mpq/codecs/pkware.h"
#include "whiteout/storages/mpq/codecs/sparse.h"
#include "whiteout/storages/mpq/file_data.h"
#include "whiteout/storages/mpq/special_files.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
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

/// Bytes that compress to about a third: runs, repeats and some noise.
std::vector<u8> compressible(size_t size, u32 seed) {
    std::mt19937 rng(seed);
    std::vector<u8> data(size);
    for (size_t at = 0; at < size;) {
        size_t const run = 1 + rng() % 40;
        u8 const value = static_cast<u8>(rng() % 3 == 0 ? rng() : 'a' + rng() % 6);
        for (size_t i = 0; i < run && at < size; ++i)
            data[at++] = (rng() % 5 == 0) ? static_cast<u8>(rng()) : value;
    }
    return data;
}

/// 16-bit samples of a slow wave, for the audio codecs.
std::vector<u8> waveform(size_t samples) {
    std::vector<u8> data(samples * 2);
    for (size_t i = 0; i < samples; ++i) {
        auto const sample = static_cast<whiteout::i16>(8000.0 * std::sin(static_cast<double>(i) * 0.05));
        data[i * 2] = static_cast<u8>(sample & 0xFF);
        data[i * 2 + 1] = static_cast<u8>((sample >> 8) & 0xFF);
    }
    return data;
}

/// @p decode into buffers one byte short, exact and one byte long, each
/// between guard bytes. The exact one succeeds and the long one fails. The
/// short one fails for a codec whose stream says where it ends; one that
/// simply decodes until told to stop (@p stopsWherePut) fills it with the
/// first bytes, as its vector form and StormLib's do. None may write outside
/// what it was given.
template <class Decode>
void checkInto(const std::vector<u8>& plain, bool stopsWherePut, Decode decode) {
    for (int delta : {-1, 0, 1}) {
        size_t const size = plain.size() + static_cast<size_t>(delta);
        std::vector<u8> block(size + 32, u8{0xA5});
        bool const ok = decode(std::span<u8>(block.data() + 16, size));
        INFO("buffer of " << size << " for " << plain.size() << " bytes");
        CHECK(ok == (delta == 0 || (delta < 0 && stopsWherePut)));
        if (ok)
            CHECK(std::memcmp(block.data() + 16, plain.data(), std::min(size, plain.size())) == 0);
        for (size_t i = 0; i < 16; ++i) {
            REQUIRE(block[i] == 0xA5);
            REQUIRE(block[16 + size + i] == 0xA5);
        }
    }
}

/// A sector of @p plain compressed with @p mask, its mask byte in front.
std::vector<u8> sectorOf(const std::vector<u8>& plain, mpq::CompressionFlag mask) {
    std::vector<u8> sector = mpq::mpqCompress(plain, mask);
    REQUIRE(!sector.empty());
    return sector;
}

} // namespace

// ============================================================================
// Decoding into a caller's buffer
// ============================================================================

TEST_CASE("A sector decodes into exactly its place, whatever its codec", "[mpq][fastpath]") {
    std::vector<u8> const text = compressible(4096, 1);
    std::vector<u8> const wave = waveform(2048);
    struct Case {
        const char* name;
        mpq::CompressionFlag mask;
        const std::vector<u8>* plain;
        bool stopsWherePut;
    };
    Case const cases[] = {
        {"zlib", mpq::CompressionFlag::kZlib, &text, false},
        {"PKware", mpq::CompressionFlag::kPKware, &text, true},
        {"BZip2", mpq::CompressionFlag::kBZip2, &text, true},
        {"Huffman", mpq::CompressionFlag::kHuffman, &text, true},
        {"sparse", mpq::CompressionFlag::kSparse, &text, false},
    };
    mpq::DecodeScratch scratch;
    for (const Case& c : cases) {
        INFO(c.name);
        // Sparse only pays for zeros: give it some.
        std::vector<u8> plain = *c.plain;
        if (c.mask == mpq::CompressionFlag::kSparse)
            std::fill(plain.begin() + 100, plain.begin() + 3000, u8{0});
        std::vector<u8> const sector = sectorOf(plain, c.mask);
        CHECK(mpq::mpqDecompress(sector, plain.size()) == plain);
        checkInto(plain, c.stopsWherePut,
                  [&](std::span<u8> dst) { return mpq::mpqDecompressInto(sector, dst, scratch); });
    }

    // ADPCM is lossy: the decode-into form must give what the vector form gives.
    for (auto mask : {mpq::CompressionFlag::kAdpcmMono | mpq::CompressionFlag::kHuffman,
                      mpq::CompressionFlag::kAdpcmStereo | mpq::CompressionFlag::kHuffman}) {
        std::vector<u8> const sector = sectorOf(wave, mask);
        std::vector<u8> const decoded = mpq::mpqDecompress(sector, wave.size());
        REQUIRE(decoded.size() == wave.size());
        // One byte short is half a sample short: ADPCM writes whole ones.
        checkInto(decoded, false, [&](std::span<u8> dst) { return mpq::mpqDecompressInto(sector, dst, scratch); });
    }
}

TEST_CASE("Each decoder stops where its buffer ends", "[mpq][fastpath]") {
    std::vector<u8> const text = compressible(6000, 2);
    auto bounded = [&](const char* name, auto decode) {
        INFO(name);
        for (size_t size : {size_t{1}, size_t{7}, size_t{100}, size_t{5999}, size_t{6000}}) {
            std::vector<u8> block(size + 32, u8{0x3C});
            size_t const got = decode(std::span<u8>(block.data() + 16, size));
            CHECK(got == size);
            CHECK(std::memcmp(block.data() + 16, text.data(), size) == 0);
            for (size_t i = 0; i < 16; ++i) {
                REQUIRE(block[i] == 0x3C);
                REQUIRE(block[16 + size + i] == 0x3C);
            }
        }
    };
    std::vector<u8> const pk = mpq::pkwareImplode(text);
    bounded("PKware", [&](std::span<u8> dst) { return mpq::pkwareExplodeInto(pk, dst); });
    std::vector<u8> const huff = mpq::huffmanCompress(text);
    bounded("Huffman", [&](std::span<u8> dst) { return mpq::huffmanDecompressInto(huff, dst); });
    std::vector<u8> const bz = common::bzip2Compress(text);
    bounded("BZip2", [&](std::span<u8> dst) { return common::bzip2DecompressInto(bz, dst); });

    // A stream that ends first gives what it holds, not a failure.
    std::vector<u8> more(text.size() + 50);
    CHECK(mpq::pkwareExplodeInto(pk, more) == text.size());
    CHECK(mpq::pkwareExplode(pk, text.size() + 50) == text);
}

TEST_CASE("The checked inflate refuses a wrong checksum and a cut stream", "[mpq][fastpath]") {
    std::vector<u8> const plain = compressible(5000, 3);
    std::vector<u8> stream = common::zlibCompress(plain);
    std::vector<u8> out(plain.size());
    auto const written = common::zlibInflateCheckedInto(out, stream);
    REQUIRE(written.has_value());
    CHECK(*written == plain.size());
    CHECK(out == plain);

    std::vector<u8> badSum = stream;
    badSum.back() ^= 0x01;
    CHECK_FALSE(common::zlibInflateCheckedInto(out, badSum).has_value());

    for (size_t cut : {stream.size() / 2, stream.size() - 5, size_t{8}}) {
        std::vector<u8> const shorter(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(cut));
        INFO("cut at " << cut);
        CHECK_FALSE(common::zlibInflateCheckedInto(out, shorter).has_value());
    }
    std::vector<u8> small(plain.size() - 1);
    CHECK_FALSE(common::zlibInflateCheckedInto(small, stream).has_value());
}

TEST_CASE("Huffman streams of every weight table decode, short and long", "[mpq][fastpath]") {
    // A long stream leans on the decoder's quick links; a short one on the
    // tree as each new byte reshapes it.
    for (u32 type = 0; type <= 8; ++type) {
        for (size_t size : {size_t{1}, size_t{300}, size_t{4096}, size_t{200000}}) {
            std::vector<u8> const plain = compressible(size, type * 31 + static_cast<u32>(size));
            std::vector<u8> const stream = mpq::huffmanCompress(plain, type);
            REQUIRE(!stream.empty());
            INFO("type " << type << ", " << size << " bytes");
            CHECK(mpq::huffmanDecompress(stream, size) == plain);
        }
    }
}

// ============================================================================
// Files through a pool
// ============================================================================

namespace {

/// Encodes @p plain as a multi-sector file and extracts it with and without
/// @p pool; both must give it back.
void checkExtract(const std::vector<u8>& plain, mpq::EncodeOptions opts, whiteout::utils::SimpleThreadPool& pool) {
    mpq::EncodedFile const encoded = mpq::encodeFileData(plain, opts, nullptr);
    mpq::BlockEntry block{0, encoded.compressedSize, static_cast<u32>(plain.size()), encoded.flags};
    u32 const key = block.isEncrypted() ? mpq::deriveFileKey(opts.filename, block) : 0;
    auto const serial = mpq::extractFileData(encoded.data, 0, block, opts.sectorSize, key);
    auto const pooled = mpq::extractFileData(encoded.data, 0, block, opts.sectorSize, key, nullptr, &pool);
    REQUIRE(serial.has_value());
    REQUIRE(pooled.has_value());
    CHECK(*serial == plain);
    CHECK(*pooled == plain);
}

} // namespace

TEST_CASE("A large file read through a pool is the file", "[mpq][fastpath]") {
    whiteout::utils::SimpleThreadPool pool{4};
    std::vector<u8> const plain = compressible((size_t{3} << 20) + 1234, 4);
    mpq::EncodeOptions opts;
    opts.sectorSize = 4096;
    opts.filename = "big.bin";
    checkExtract(plain, opts, pool);
    opts.encrypt = true;
    checkExtract(plain, opts, pool);
    opts.encrypt = false;
    opts.compression = mpq::CompressionFlag::kPKware;
    checkExtract(plain, opts, pool);

    // One bad sector fails the whole read, and says which.
    mpq::EncodedFile encoded = mpq::encodeFileData(plain, mpq::EncodeOptions{}, nullptr);
    mpq::BlockEntry block{0, encoded.compressedSize, static_cast<u32>(plain.size()), encoded.flags};
    u32 offsets[2];
    std::memcpy(offsets, encoded.data.data() + 300 * 4, 8); // Sector 300's bounds.
    encoded.data[offsets[0] + (offsets[1] - offsets[0]) / 2] ^= 0xFF;
    std::string error;
    CHECK_FALSE(mpq::extractFileData(encoded.data, 0, block, 4096, 0, &error, &pool).has_value());
    CHECK(error.find("sector 300") != std::string::npos);
    CHECK_FALSE(mpq::extractFileData(encoded.data, 0, block, 4096, 0).has_value());
}

TEST_CASE("A file its stored bytes could not plausibly hold still reads", "[mpq][fastpath]") {
    // Zeros pack more than 64 to 1: the output is not set aside up front, it
    // grows as the sectors decode.
    whiteout::utils::SimpleThreadPool pool{4};
    std::vector<u8> plain(size_t{6} << 20, u8{0});
    plain[12345] = 9;
    mpq::EncodeOptions opts;
    opts.sectorSize = 4096;
    mpq::EncodedFile const encoded = mpq::encodeFileData(plain, opts, nullptr);
    REQUIRE(encoded.compressedSize * 64 < plain.size());
    mpq::BlockEntry block{0, encoded.compressedSize, static_cast<u32>(plain.size()), encoded.flags};
    CHECK(mpq::extractFileData(encoded.data, 0, block, 4096, 0).value_or(std::vector<u8>{}) == plain);
    CHECK(mpq::extractFileData(encoded.data, 0, block, 4096, 0, nullptr, &pool).value_or(std::vector<u8>{}) == plain);

    // As one unit, too.
    opts.singleUnit = true;
    mpq::EncodedFile const whole = mpq::encodeFileData(plain, opts, nullptr);
    mpq::BlockEntry wholeBlock{0, whole.compressedSize, static_cast<u32>(plain.size()), whole.flags};
    CHECK(mpq::extractFileData(whole.data, 0, wholeBlock, 4096, 0).value_or(std::vector<u8>{}) == plain);
}

TEST_CASE("Encoding does not depend on the pool, and takes the checksums asked for", "[mpq][fastpath]") {
    whiteout::utils::SimpleThreadPool pool{4};
    std::vector<u8> const large = compressible((size_t{5} << 20) + 77, 5);
    std::vector<u8> const small = compressible(30000, 6);
    std::vector<u8> const noise = [] {
        std::mt19937 rng(7);
        std::vector<u8> data(size_t{5} << 20);
        for (auto& b : data)
            b = static_cast<u8>(rng());
        return data;
    }();

    mpq::EncodeOptions opts;
    opts.sectorSize = 4096;
    opts.filename = "dir\\file.bin";
    opts.crc32 = true;
    opts.md5 = true;
    for (bool encrypt : {false, true}) {
        opts.encrypt = encrypt;
        for (const std::vector<u8>* plain : {&large, &small, &noise}) {
            mpq::EncodedFile const serial = mpq::encodeFileData(*plain, opts, nullptr);
            mpq::EncodedFile const pooled = mpq::encodeFileData(*plain, opts, &pool);
            CHECK(serial.data == pooled.data);
            CHECK(serial.flags == pooled.flags);
            CHECK(serial.compressedSize == pooled.compressedSize);
            CHECK(serial.crc32 == whiteout::crc32(plain->data(), plain->size()));
            CHECK(pooled.crc32 == serial.crc32);
            CHECK(serial.md5 == common::md5Hash(*plain));
            CHECK(pooled.md5 == serial.md5);
        }
    }
    // Noise is stored: no sector table in front of it.
    opts.encrypt = false;
    CHECK(mpq::encodeFileData(noise, opts, &pool).data == noise);

    // A batch is its files one by one.
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;
    std::vector<std::vector<u8>> files;
    for (u32 i = 0; i < 40; ++i)
        files.push_back(compressible(500 + i * 9000, 100 + i));
    files.push_back(large);
    files.push_back({});
    for (const auto& file : files)
        items.emplace_back(file, opts);
    mpq::BatchEncodeResult const batch = mpq::encodeBatch(items, &pool);
    REQUIRE(batch.files.size() == files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        mpq::EncodedFile const one = mpq::encodeFileData(files[i], opts, nullptr);
        INFO("file " << i);
        CHECK(batch.files[i].data == one.data);
        CHECK(batch.files[i].flags == one.flags);
        CHECK(batch.files[i].crc32 == one.crc32);
        CHECK(batch.files[i].md5 == one.md5);
    }
}

// ============================================================================
// The Storage calls
// ============================================================================

TEST_CASE("readFiles gives what readFile gives, name by name", "[mpq][fastpath]") {
    TempDir const dir("whiteout_mpq_fastpath_read");
    std::string const path = (dir.path / "batch.mpq").string();
    std::vector<std::string> names;
    std::vector<std::vector<u8>> contents;
    {
        mpq::Storage storage = mpq::Storage::create();
        for (u32 i = 0; i < 60; ++i) {
            names.push_back("dir\\file" + std::to_string(i) + ".bin");
            contents.push_back(compressible(i * 7000, 200 + i)); // The first is empty.
            mpq::WriteOptions opts;
            opts.encrypt = i % 3 == 0;
            opts.singleUnit = i % 5 == 0;
            opts.compression = i % 4 == 0 ? mpq::Compression::PKware : mpq::Compression::Zlib;
            REQUIRE(storage.writeFile(names.back(), contents.back(), opts));
        }
        names.push_back("big.bin");
        contents.push_back(compressible((size_t{6} << 20) + 5, 8));
        REQUIRE(storage.writeFile(names.back(), contents.back()));
        REQUIRE(storage.save(path));
    }

    whiteout::utils::SimpleThreadPool pool{4};
    for (whiteout::interfaces::WorkerPool* use : {static_cast<whiteout::interfaces::WorkerPool*>(nullptr),
                                                  static_cast<whiteout::interfaces::WorkerPool*>(&pool)}) {
        auto storage = mpq::Storage::open(path, use);
        REQUIRE(storage.has_value());
        std::vector<std::string> asked = names;
        asked.insert(asked.begin() + 3, "no\\such\\file.txt");
        asked.push_back("DIR/FILE7.BIN"); // Another spelling of a name it holds.
        auto const got = storage->readFiles(asked);
        REQUIRE(got.size() == asked.size());
        for (size_t i = 0; i < asked.size(); ++i) {
            INFO(asked[i]);
            CHECK(got[i] == storage->readFile(asked[i]));
        }
        CHECK_FALSE(got[3].has_value());
        CHECK(got.back() == std::optional<std::vector<u8>>(contents[7]));
        CHECK(got[asked.size() - 2] == std::optional<std::vector<u8>>(contents.back()));

        // Pending writes and deletes count as saved.
        REQUIRE(storage->writeFile("new.txt", std::vector<u8>{1, 2, 3}));
        REQUIRE(storage->deleteFile(names[9]));
        auto const after = storage->readFiles({"new.txt", names[9], names[10]});
        CHECK(after[0] == std::optional<std::vector<u8>>(std::vector<u8>{1, 2, 3}));
        CHECK_FALSE(after[1].has_value());
        CHECK(after[2] == std::optional<std::vector<u8>>(contents[10]));
        CHECK(storage->readFiles({}).empty());
    }
}

TEST_CASE("writeFileOwned takes the buffer and writes what writeFile writes", "[mpq][fastpath]") {
    TempDir const dir("whiteout_mpq_fastpath_owned");
    std::string const path = (dir.path / "owned.mpq").string();
    std::vector<u8> const plain = compressible(100000, 9);
    mpq::Storage storage = mpq::Storage::create();
    std::vector<u8> given = plain;
    REQUIRE(storage.writeFileOwned("taken.bin", std::move(given)));
    CHECK(given.empty());
    REQUIRE(storage.writeFile("copied.bin", plain));
    CHECK(storage.readFile("taken.bin") == std::optional<std::vector<u8>>(plain));
    REQUIRE(storage.save(path));
    auto reopened = mpq::Storage::open(path);
    REQUIRE(reopened.has_value());
    CHECK(reopened->readFile("taken.bin") == std::optional<std::vector<u8>>(plain));
    CHECK(reopened->fileInfo("taken.bin")->compressedSize == reopened->fileInfo("copied.bin")->compressedSize);
}

TEST_CASE("A created archive records CRC-32 and time, and MD5 when asked", "[mpq][fastpath]") {
    TempDir const dir("whiteout_mpq_fastpath_attr");
    std::vector<u8> const plain = compressible(20000, 10);
    for (bool md5 : {false, true}) {
        std::string const path = (dir.path / (md5 ? "md5.mpq" : "plain.mpq")).string();
        mpq::CreateOptions create;
        create.fileHashes = md5;
        mpq::Storage storage = mpq::Storage::create(create);
        REQUIRE(storage.writeFile("a.bin", plain));
        REQUIRE(storage.save(path));

        auto const attributes = storage.readFile("(attributes)");
        REQUIRE(attributes.has_value());
        REQUIRE(attributes->size() >= 8);
        u32 flags = 0;
        std::memcpy(&flags, attributes->data() + 4, 4);
        CHECK(flags == (md5 ? 7u : 3u));
        mpq::FileAttributes const parsed = mpq::parseAttributes(*attributes, storage.archiveInfo().blockTableEntries);
        REQUIRE(!parsed.crc32s.empty());
        CHECK(parsed.crc32s[0] == whiteout::crc32(plain.data(), plain.size()));
        CHECK(parsed.md5s.empty() == !md5);
        if (md5)
            CHECK(parsed.md5s[0] == common::md5Hash(plain));

        // Saved again, it keeps the arrays it has.
        REQUIRE(storage.writeFile("b.bin", plain));
        REQUIRE(storage.save());
        auto const again = storage.readFile("(attributes)");
        REQUIRE(again.has_value());
        std::memcpy(&flags, again->data() + 4, 4);
        CHECK(flags == (md5 ? 7u : 3u));
    }
}

TEST_CASE("Names are found before and after the (listfile) is read", "[mpq][fastpath]") {
    TempDir const dir("whiteout_mpq_fastpath_names");
    std::string const path = (dir.path / "names.mpq").string();
    {
        mpq::Storage storage = mpq::Storage::create();
        REQUIRE(storage.writeFile("Units\\Human\\Footman.mdx", std::vector<u8>{1}));
        REQUIRE(storage.writeFile("war3map.j", std::vector<u8>{2}));
        REQUIRE(storage.save(path));
    }
    auto storage = mpq::Storage::open(path);
    REQUIRE(storage.has_value());
    // Nothing has listed yet: a lookup needs no (listfile).
    CHECK(storage->fileExists("units\\human\\footman.MDX"));
    CHECK(storage->fileExists("Units/Human/Footman.mdx"));
    CHECK_FALSE(storage->fileExists("Units\\Human\\Footman.mdl"));
    CHECK(storage->listFiles().size() == 2);
    CHECK(storage->listFiles() == storage->listFiles());

    // With an overlay pending, every lookup asks it first.
    REQUIRE(storage->writeFile("Extra\\New.txt", std::vector<u8>{3}));
    REQUIRE(storage->deleteFile("war3map.j"));
    CHECK(storage->fileExists("EXTRA/new.TXT"));
    CHECK(storage->fileExists("Units\\Human\\Footman.mdx"));
    CHECK_FALSE(storage->fileExists("WAR3MAP.J"));
    CHECK(storage->listFiles().size() == 2);
    // A name too long for the overlay's stack buffer.
    std::string const longName(400, 'x');
    CHECK_FALSE(storage->fileExists(longName));
    REQUIRE(storage->writeFile(longName, std::vector<u8>{4}));
    CHECK(storage->fileExists(longName));
}
