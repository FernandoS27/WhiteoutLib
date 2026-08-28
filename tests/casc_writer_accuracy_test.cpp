// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file casc_writer_accuracy_test.cpp
/// @brief Tests for CASC writer accuracy improvements based on the
///        CASC_Write_Report reference implementation analysis.
///
/// Validates:
///   1. .build.info CDN Key field uses the actual CDN config hash
///   2. Shmem field order: archiveCount(4) + version(4) per reference
///   3. Archive entry header format: EKey(16 reversed) + size(4 LE) + flags(2) + checksum(8)
///   4. Archive entry header key bytes are reversed per MakeFileHeader convention
///   5. Archive entry header encoded size is stored as LE32
///   6. Build config encoding-size line has both decoded and encoded sizes
///   7. Index file guarded-block checksums cover the full headerDataSize bytes

#include <whiteout/storages/casc/storage.h>
#include <whiteout/storages/casc/storage_writable.h>

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Internal headers needed for low-level validation.
#include "../src/whiteout/storages/casc/tables/config.h"
#include "../src/whiteout/storages/common/byte_order.h"
#include "../src/whiteout/storages/common/hex.h"
#include "../src/whiteout/storages/common/jenkins.h"
#include "../src/whiteout/storages/common/md5.h"

using namespace whiteout;
using namespace whiteout::storages::casc;

namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

static std::vector<u8> makeTestData(size_t size, u8 seed = 0) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>((i + seed) & 0xFF);
    return data;
}

static void cleanDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

/// Read a file's entire contents as bytes.
static std::vector<u8> readAllBytes(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return {};
    auto size = ifs.tellg();
    ifs.seekg(0);
    std::vector<u8> data(static_cast<size_t>(size));
    ifs.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

/// Read a file's entire contents as string.
static std::string readAllString(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return {};
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
}

/// Find the config file path given a 16-byte key.
static std::string findConfigFile(const std::string& dataDir,
                                  const std::array<u8, 16>& key) {
    auto hex = storages::common::hexEncode16(key);
    return dataDir + "/config/" + hex.substr(0, 2) + "/" + hex.substr(2, 2) + "/" + hex;
}

/// Parse the Build Key and CDN Key from a .build.info file.
struct BuildInfoKeys {
    std::array<u8, 16> buildKey{};
    std::array<u8, 16> cdnKey{};
};

static BuildInfoKeys parseBuildInfoKeys(const std::string& buildInfoPath) {
    BuildInfoKeys result;
    auto content = readAllString(buildInfoPath);
    if (content.empty()) return result;

    // Find header line and data line.
    auto lines = std::vector<std::string>();
    {
        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty())
                lines.push_back(line);
        }
    }
    if (lines.size() < 2) return result;

    // Parse column indices from header.
    auto splitPipe = [](const std::string& s) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start < s.size()) {
            auto end = s.find('|', start);
            if (end == std::string::npos) end = s.size();
            parts.push_back(s.substr(start, end - start));
            start = end + 1;
        }
        return parts;
    };

    auto headers = splitPipe(lines[0]);
    auto values = splitPipe(lines[1]);

    int iBuildKey = -1, iCdnKey = -1;
    for (size_t i = 0; i < headers.size(); ++i) {
        auto& h = headers[i];
        if (h.find("Build Key") != std::string::npos) iBuildKey = static_cast<int>(i);
        if (h.find("CDN Key") != std::string::npos) iCdnKey = static_cast<int>(i);
    }

    if (iBuildKey >= 0 && iBuildKey < static_cast<int>(values.size()))
        result.buildKey = storages::common::hexDecode16(values[iBuildKey]);
    if (iCdnKey >= 0 && iCdnKey < static_cast<int>(values.size()))
        result.cdnKey = storages::common::hexDecode16(values[iCdnKey]);

    return result;
}

// ============================================================================
// Test 1: .build.info CDN Key correctness
// ============================================================================

TEST_CASE("Build info has distinct CDN key", "[casc][writer_accuracy]") {
    const std::string testDir = "test_writer_accuracy_cdn_key";
    cleanDir(testDir);

    auto storage = StorageWritable::create();
    storage.writeFile("hello.txt", makeTestData(128, 0x42));
    REQUIRE(storage.save(testDir));

    // Parse .build.info to get build key and CDN key.
    auto keys = parseBuildInfoKeys(testDir + "/.build.info");

    // Keys must be non-zero.
    bool buildKeyZero = true, cdnKeyZero = true;
    for (auto b : keys.buildKey) if (b != 0) { buildKeyZero = false; break; }
    for (auto b : keys.cdnKey) if (b != 0) { cdnKeyZero = false; break; }
    REQUIRE_FALSE(buildKeyZero);
    REQUIRE_FALSE(cdnKeyZero);

    // Build key and CDN key must be DIFFERENT (build config != CDN config).
    CHECK(keys.buildKey != keys.cdnKey);

    // Verify both config files exist at their respective paths.
    std::string dataDir = testDir + "/Data";
    auto buildConfigPath = findConfigFile(dataDir, keys.buildKey);
    auto cdnConfigPath = findConfigFile(dataDir, keys.cdnKey);

    CHECK(fs::exists(buildConfigPath));
    CHECK(fs::exists(cdnConfigPath));

    // Verify the CDN config at cdnKey path actually parses as a CDN config
    // (should contain "archives = ").
    auto cdnContent = readAllString(cdnConfigPath);
    CHECK(cdnContent.find("archives") != std::string::npos);

    // Verify the build config at buildKey path actually parses as a build config
    // (should contain "root = ").
    auto buildContent = readAllString(buildConfigPath);
    CHECK(buildContent.find("root") != std::string::npos);

    cleanDir(testDir);
}

// ============================================================================
// Test 2: Reopen with CDN config — archive EKeys loaded from CDN config
// ============================================================================

TEST_CASE("CDN config loadable from build info key", "[casc][writer_accuracy]") {
    const std::string testDir = "test_writer_accuracy_cdn_load";
    cleanDir(testDir);

    auto storage = StorageWritable::create();
    storage.writeFile("file1.bin", makeTestData(1024, 0x10));
    storage.writeFile("file2.bin", makeTestData(2048, 0x20));
    REQUIRE(storage.save(testDir));

    // Open the storage — this will use .build.info CDN Key to find CDN config.
    auto reopened = Storage::open(testDir);
    REQUIRE(reopened.has_value());

    // Verify files can be read (proves the full open path works).
    auto r1 = reopened->readFile("file1.bin");
    REQUIRE(r1.has_value());
    CHECK(*r1 == makeTestData(1024, 0x10));

    auto r2 = reopened->readFile("file2.bin");
    REQUIRE(r2.has_value());
    CHECK(*r2 == makeTestData(2048, 0x20));

    cleanDir(testDir);
}

// ============================================================================
// Test 3: Shmem round-trip (write then parse matches)
// ============================================================================

TEST_CASE("Shmem round-trip", "[casc][writer_accuracy]") {
    const std::string testDir = "test_writer_accuracy_shmem";
    cleanDir(testDir);

    auto storage = StorageWritable::create();
    // Write enough files to be non-trivial.
    storage.writeFile("a.txt", makeTestData(64, 0x01));
    storage.writeFile("b.txt", makeTestData(64, 0x02));
    REQUIRE(storage.save(testDir));

    // Read the shmem file.
    auto shmemData = readAllBytes(testDir + "/Data/shmem");
    REQUIRE(shmemData.size() >= 8);

    auto shmem = parseShmem(shmemData);

    // Archive count must be >= 1 (we wrote files).
    CHECK(shmem.archiveCount >= 1);

    // Version must be 7 (kShmemVersion).
    CHECK(shmem.version == 7);

    // Count actual archive files on disk.
    u32 actualArchiveCount = 0;
    std::string dataSubdir = testDir + "/Data/data";
    for (auto& entry : fs::directory_iterator(dataSubdir)) {
        if (entry.is_regular_file()) {
            auto name = entry.path().filename().string();
            if (name.size() >= 8 && name.substr(0, 5) == "data.")
                ++actualArchiveCount;
        }
    }

    CHECK(shmem.archiveCount == actualArchiveCount);

    cleanDir(testDir);
}

// ============================================================================
// Test 4: Archive entry header checksum is non-zero
// ============================================================================

TEST_CASE("Archive entry header has checksum", "[casc][writer_accuracy]") {
    const std::string testDir = "test_writer_accuracy_header";
    cleanDir(testDir);

    auto storage = StorageWritable::create();
    storage.writeFile("test.bin", makeTestData(256, 0xAB));
    REQUIRE(storage.save(testDir));

    // Read data.000 archive file.
    auto archiveData = readAllBytes(testDir + "/Data/data/data.000");
    REQUIRE(archiveData.size() >= 30);

    // The first 30 bytes are the first file's entry header.
    // Layout: EKey(16 reversed) + encodedSize(4 LE) + flags(2) + checksum(8)

    // EKey (first 16 bytes) must be non-zero.
    bool ekeyZero = true;
    for (int i = 0; i < 16; ++i)
        if (archiveData[i] != 0) { ekeyZero = false; break; }
    CHECK_FALSE(ekeyZero);

    // Encoded size at offset 16 (4 bytes LE per reference) must be > 0.
    u32 encodedSize = storages::common::readLE32(archiveData.data() + 16);
    CHECK(encodedSize > 0);

    // Flags at offset 20 (2 bytes) — typically 0 for data channel.
    // Just verify those two bytes exist; value depends on implementation.

    // Checksum at offset 22 (8 bytes) must be non-zero.
    bool checksumZero = true;
    for (int i = 22; i < 30; ++i)
        if (archiveData[i] != 0) { checksumZero = false; break; }
    CHECK_FALSE(checksumZero);

    // Verify checksum matches expected value: MD5(header[0:22] + LE32(offset=0))[0:8].
    storages::common::MD5 hasher;
    hasher.update(archiveData.data(), 22);
    u8 offsetLE[4] = {0, 0, 0, 0}; // First entry is at offset 0.
    hasher.update(offsetLE, 4);
    auto expectedHash = hasher.finalize();
    CHECK(std::memcmp(archiveData.data() + 22, expectedHash.data(), 8) == 0);

    cleanDir(testDir);
}

// ============================================================================
// Test 5: Archive checksum validates for non-zero-offset entries
// ============================================================================

TEST_CASE("Archive header checksum at non-zero offset", "[casc][writer_accuracy]") {
    const std::string testDir = "test_writer_accuracy_header_offset";
    cleanDir(testDir);

    auto storage = StorageWritable::create();
    storage.writeFile("first.bin", makeTestData(128, 0x01));
    storage.writeFile("second.bin", makeTestData(256, 0x02));
    REQUIRE(storage.save(testDir));

    auto archiveData = readAllBytes(testDir + "/Data/data/data.000");
    REQUIRE(archiveData.size() >= 60); // At least 2 entries.

    // Parse first entry header.
    u32 firstEncodedSize = storages::common::readLE32(archiveData.data() + 16);
    REQUIRE(firstEncodedSize > 0);

    // Second entry starts after first header (30) + first BLTE data (firstEncodedSize).
    u32 secondOffset = 30 + firstEncodedSize;
    REQUIRE(archiveData.size() >= secondOffset + 30);

    // Verify second entry checksum.
    const u8* secondHeader = archiveData.data() + secondOffset;

    storages::common::MD5 hasher;
    hasher.update(secondHeader, 22);
    u8 offsetLE[4];
    offsetLE[0] = static_cast<u8>(secondOffset & 0xFF);
    offsetLE[1] = static_cast<u8>((secondOffset >> 8) & 0xFF);
    offsetLE[2] = static_cast<u8>((secondOffset >> 16) & 0xFF);
    offsetLE[3] = static_cast<u8>((secondOffset >> 24) & 0xFF);
    hasher.update(offsetLE, 4);
    auto expectedHash = hasher.finalize();

    CHECK(std::memcmp(secondHeader + 22, expectedHash.data(), 8) == 0);

    cleanDir(testDir);
}

// ============================================================================
// Test 6: Full round-trip still works after accuracy changes
// ============================================================================

TEST_CASE("Full round-trip post accuracy changes", "[casc][writer_accuracy]") {
    const std::string testDir = "test_writer_accuracy_roundtrip";
    cleanDir(testDir);

    auto storage = StorageWritable::create();
    auto small = makeTestData(100, 0x11);
    auto medium = makeTestData(65536, 0x22); // Multi-frame BLTE.
    auto empty = makeTestData(0);

    storage.writeFile("small.dat", small);
    storage.writeFile("medium.dat", medium);
    storage.writeFile("empty.dat", empty);
    REQUIRE(storage.save(testDir));

    auto reopened = Storage::open(testDir);
    REQUIRE(reopened.has_value());

    auto r1 = reopened->readFile("small.dat");
    REQUIRE(r1.has_value());
    CHECK(*r1 == small);

    auto r2 = reopened->readFile("medium.dat");
    REQUIRE(r2.has_value());
    CHECK(*r2 == medium);

    auto r3 = reopened->readFile("empty.dat");
    REQUIRE(r3.has_value());
    CHECK(*r3 == empty);

    cleanDir(testDir);
}

// ============================================================================
// Test 7: Archive header stores EKey bytes reversed
// ============================================================================

TEST_CASE("Archive header stores reversed EKey", "[casc][writer_accuracy]") {
    const std::string testDir = "test_writer_accuracy_ekey_rev";
    cleanDir(testDir);

    auto storage = StorageWritable::create();
    storage.writeFile("file.bin", makeTestData(512, 0x55));
    REQUIRE(storage.save(testDir));

    auto archiveData = readAllBytes(testDir + "/Data/data/data.000");
    REQUIRE(archiveData.size() >= 60); // At least one entry with header + data.

    // First entry: header(30) + BLTE data.
    // Encoded size at offset 16 (LE32) tells us the BLTE blob size.
    u32 blteSize = storages::common::readLE32(archiveData.data() + 16);
    REQUIRE(blteSize > 0);
    REQUIRE(archiveData.size() >= 30u + blteSize);

    // Compute the expected EKey = MD5(BLTE blob).
    auto expectedEKey = storages::common::md5Hash(
        std::span<const u8>(archiveData.data() + 30, blteSize));

    // Archive header stores the EKey with bytes reversed.
    std::array<u8, 16> storedKey;
    std::memcpy(storedKey.data(), archiveData.data(), 16);

    std::array<u8, 16> expectedReversed;
    for (int i = 0; i < 16; ++i)
        expectedReversed[i] = expectedEKey[15 - i];

    CHECK(storedKey == expectedReversed);

    cleanDir(testDir);
}

// ============================================================================
// Test 8: Archive header encoded size is stored as LE32
// ============================================================================

TEST_CASE("Archive header encoded size is LE32", "[casc][writer_accuracy]") {
    const std::string testDir = "test_writer_accuracy_le32";
    cleanDir(testDir);

    auto storage = StorageWritable::create();
    storage.writeFile("data.bin", makeTestData(1024, 0xCC));
    REQUIRE(storage.save(testDir));

    auto archiveData = readAllBytes(testDir + "/Data/data/data.000");
    REQUIRE(archiveData.size() >= 30);

    // Read encoded size as LE32 at offset 16.
    u32 sizeLE = storages::common::readLE32(archiveData.data() + 16);

    // The BLTE data follows the 30-byte header, so the archive total
    // minus 30 should equal this size.
    // (For a single-file archive with one entry.)
    // Actually, the next entry header follows, so we need to be precise.
    // For a single-entry archive, archiveData.size() - 30 == sizeLE.
    // But the archive may have 3+ entries (root + encoding + file).
    // Just verify the LE value makes sense (> 0. < archiveSize).
    CHECK(sizeLE > 0);
    CHECK(sizeLE < archiveData.size());

    // Cross-check: reading as BE32 would give a different value
    // (unless by coincidence, e.g. all bytes equal).
    u32 sizeBE = storages::common::readBE32(archiveData.data() + 16);
    // For typical BLTE sizes ~50-1100 bytes, the BE interpretation
    // would be a very large number. Just verify the LE interpretation
    // is \"reasonable\" (matches actual data following the header).
    // The BLTE data right after the 30-byte header should start with
    // BLTE magic = {0x42, 0x4C, 0x54, 0x45}.
    CHECK(archiveData[30] == 0x42); // 'B'
    CHECK(archiveData[31] == 0x4C); // 'L'
    CHECK(archiveData[32] == 0x54); // 'T'
    CHECK(archiveData[33] == 0x45); // 'E'

    // Verify: offset 30 + sizeLE should be the start of the next entry
    // (or the end of the archive for the last entry).
    // We verify that if there's a next entry, it also starts with
    // a valid pattern (non-zero EKey + BLTE after its header).
    u32 nextOffset = 30 + sizeLE;
    if (nextOffset + 30 < archiveData.size()) {
        // Another entry follows; verify BLTE magic after its 30-byte header.
        CHECK(archiveData[nextOffset + 30] == 0x42); // 'B'
    }

    cleanDir(testDir);
}

// ============================================================================
// Test 9: Build config encoding-size has two values
// ============================================================================

TEST_CASE("Build config encoding-size has two values", "[casc][writer_accuracy]") {
    const std::string testDir = "test_writer_accuracy_encsize";
    cleanDir(testDir);

    auto storage = StorageWritable::create();
    storage.writeFile("payload.bin", makeTestData(4096, 0x77));
    REQUIRE(storage.save(testDir));

    // Find and read the build config file.
    auto keys = parseBuildInfoKeys(testDir + "/.build.info");
    std::string dataDir = testDir + "/Data";
    auto buildConfigPath = findConfigFile(dataDir, keys.buildKey);
    auto content = readAllString(buildConfigPath);
    REQUIRE_FALSE(content.empty());

    // Find the encoding-size line.
    std::string encodingSizeLine;
    {
        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find("encoding-size") == 0) {
                encodingSizeLine = line;
                break;
            }
        }
    }
    REQUIRE_FALSE(encodingSizeLine.empty());

    // Parse: "encoding-size = <decoded> <encoded>"
    auto eqPos = encodingSizeLine.find('=');
    REQUIRE(eqPos != std::string::npos);
    std::string valPart = encodingSizeLine.substr(eqPos + 1);

    // Trim leading/trailing whitespace.
    while (!valPart.empty() && valPart.front() == ' ') valPart.erase(valPart.begin());
    while (!valPart.empty() && valPart.back() == ' ') valPart.pop_back();

    // Split by space — should have exactly 2 tokens.
    auto spacePos = valPart.find(' ');
    CHECK(spacePos != std::string::npos); // Must have two values.

    if (spacePos != std::string::npos) {
        std::string first = valPart.substr(0, spacePos);
        std::string second = valPart.substr(spacePos + 1);
        // Trim second.
        while (!second.empty() && second.front() == ' ') second.erase(second.begin());

        u64 decodedSize = 0, encodedSize = 0;
        std::from_chars(first.data(), first.data() + first.size(), decodedSize);
        std::from_chars(second.data(), second.data() + second.size(), encodedSize);

        // Both must be > 0.
        CHECK(decodedSize > 0);
        CHECK(encodedSize > 0);

        // Encoded size should differ from decoded (BLTE adds overhead/compression).
        // For most cases they won't be equal.
        // At minimum, both should be valid numbers.
    }

    cleanDir(testDir);
}

// ============================================================================
// Test 10: Index file header guarded-block checksum covers full headerDataSize
// ============================================================================

TEST_CASE("Index header checksum covers full block", "[casc][writer_accuracy]") {
    const std::string testDir = "test_writer_accuracy_idx_hash";
    cleanDir(testDir);

    auto storage = StorageWritable::create();
    // Write several files so that at least one idx bucket has entries.
    for (int i = 0; i < 4; ++i)
        storage.writeFile("file" + std::to_string(i) + ".bin",
                          makeTestData(256 + i * 100, u8(i)));
    REQUIRE(storage.save(testDir));

    // Find all .idx files in the data directory.
    std::string dataSubdir = testDir + "/Data/data";
    std::vector<std::string> idxPaths;
    for (auto& entry : fs::directory_iterator(dataSubdir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".idx")
            idxPaths.push_back(entry.path().string());
    }
    REQUIRE_FALSE(idxPaths.empty());

    for (auto& idxPath : idxPaths) {
        INFO(idxPath);
        auto data = readAllBytes(idxPath);
        REQUIRE(data.size() >= 40);

        // Guarded block 1: data[0:4] = headerDataSize, data[4:8] = hash.
        u32 headerDataSize = 0;
        std::memcpy(&headerDataSize, data.data(), 4);
        CHECK(headerDataSize == 16);

        // Hash must cover all headerDataSize bytes starting at offset 8.
        u32 headerPc = 0, headerPb = 0;
        storages::common::jenkinsHashlittle2(data.data() + 8, headerDataSize, headerPc, headerPb);

        u32 storedHash = 0;
        std::memcpy(&storedHash, data.data() + 4, 4);

        CHECK(storedHash == headerPc);

        // Guarded block 2: data[32:36] = dataSize, data[36:40] = hash. The
        // hash accumulates one hashlittle2 call per entry, not one over the
        // block, so it must cover every entry the size field claims.
        u32 dataSize = 0;
        std::memcpy(&dataSize, data.data() + 32, 4);
        CHECK(dataSize > 0);
        REQUIRE(data.size() >= 40u + dataSize);

        size_t const entrySize = size_t(data[14]) + data[13] + data[12] + data[11];
        REQUIRE(entrySize > 0);

        u32 dataPc = 0, dataPb = 0;
        for (size_t i = 0; i + entrySize <= dataSize; i += entrySize) {
            storages::common::jenkinsHashlittle2(data.data() + 40 + i, entrySize, dataPc, dataPb);
        }

        u32 storedDataHash = 0;
        std::memcpy(&storedDataHash, data.data() + 36, 4);

        CHECK(storedDataHash == dataPc);
    }

    cleanDir(testDir);
}
