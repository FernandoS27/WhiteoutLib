// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_ow_root_test: Validates Overwatch (Tank) root manifest parsing using
/// synthetic data.

#include "../src/whiteout/storages/casc/roots/ow_root.h"
#include "../src/whiteout/storages/casc/roots/root.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

// ============================================================================
// Binary builder helpers
// ============================================================================

static void writeLE32(std::vector<u8>& buf, u32 v) {
    buf.push_back(u8(v));
    buf.push_back(u8(v >> 8));
    buf.push_back(u8(v >> 16));
    buf.push_back(u8(v >> 24));
}

static void writeLE64(std::vector<u8>& buf, u64 v) {
    writeLE32(buf, u32(v));
    writeLE32(buf, u32(v >> 32));
}

static void writeBytes(std::vector<u8>& buf, const void* data, size_t len) {
    auto p = static_cast<const u8*>(data);
    buf.insert(buf.end(), p, p + len);
}

/// Create a synthetic CKey: [seed, seed+1, ... seed+15].
static std::array<u8, 16> makeCKey(u8 seed) {
    std::array<u8, 16> key;
    for (int i = 0; i < 16; ++i) key[i] = u8(seed + i);
    return key;
}

/// Convert a 16-byte key to a 32-character hex string.
static std::string hexStr(const std::array<u8, 16>& key) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (auto b : key) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0xF]);
    }
    return s;
}

// ============================================================================
// Text root builder
// ============================================================================

/// Build a synthetic Overwatch text root file.
static std::vector<u8> buildTextRoot(
    const std::vector<OwRootFileEntry>& entries,
    bool includeFileId = true,
    bool includeInstallPath = true) {

    std::string text;

    // Header line
    text += "#";
    if (includeFileId) text += "FILEID|";
    text += "MD5|CHUNK_ID|PRIORITY|MPRIORITY|FILENAME";
    if (includeInstallPath) text += "|INSTALLPATH";
    text += "\n";

    // Data rows
    for (auto& e : entries) {
        if (includeFileId) {
            text += e.fileId;
            text += "|";
        }
        text += hexStr(e.md5);
        text += "|";
        text += std::to_string(e.chunkId);
        text += "|";
        text += std::to_string(e.priority);
        text += "|";
        text += std::to_string(e.mPriority);
        text += "|";
        text += e.fileName;
        if (includeInstallPath) {
            text += "|";
            text += e.installPath;
        }
        text += "\n";
    }

    return std::vector<u8>(text.begin(), text.end());
}

// ============================================================================
// CMF builder
// ============================================================================

/// Build a minimal unencrypted CMF v25 blob.
/// @param hashEntries GUID + CKey pairs.
/// @param buildVersion Build version to encode in header.
static std::vector<u8> buildCmfV25(
    const std::vector<std::pair<u64, std::array<u8, 16>>>& hashEntries,
    u32 buildVersion = 60000) {

    i32 dataCount = i32(hashEntries.size());
    i32 entryCount = dataCount; // entries match data for simplicity

    std::vector<u8> buf;

    // V25 header (40 bytes):
    // buildVersion(4) + unk04(4) + unk08(4) + unk0C(4) + unk10(4) + unk14(4)
    // + dataCount(4) + unk1C(4) + entryCount(4) + magic(4)
    writeLE32(buf, buildVersion);   // buildVersion
    writeLE32(buf, 0);              // unk04
    writeLE32(buf, 0);              // unk08
    writeLE32(buf, 0);              // unk0C
    writeLE32(buf, 0);              // unk10
    writeLE32(buf, 0);              // unk14
    writeLE32(buf, u32(dataCount)); // dataCount
    writeLE32(buf, 0);              // unk1C
    writeLE32(buf, u32(entryCount));// entryCount
    // Magic: version 25, non-encrypted = (25 << 24) | lower 3 bytes
    writeLE32(buf, 0x19000000u);    // magic: version 25, non-encrypted

    // CMF entries (20 bytes each): index(4) + hashA(8) + hashB(8)
    for (i32 i = 0; i < entryCount; ++i) {
        writeLE32(buf, u32(i + 1));              // index
        writeLE64(buf, hashEntries[size_t(i)].first); // hashA = GUID
        writeLE64(buf, 0);                       // hashB
    }

    // Hash data v25 (29 bytes each): GUID(8) + Size(4) + Unknown(1) + CKey(16)
    for (auto& [guid, ckey] : hashEntries) {
        writeLE64(buf, guid);
        writeLE32(buf, 1024); // size = 1024 (arbitrary)
        buf.push_back(0);     // unknown
        writeBytes(buf, ckey.data(), 16);
    }

    return buf;
}

/// Build a minimal unencrypted CMF v24 blob (no Unknown byte in hash data).
static std::vector<u8> buildCmfV24(
    const std::vector<std::pair<u64, std::array<u8, 16>>>& hashEntries,
    u32 buildVersion = 55000) {

    i32 dataCount = i32(hashEntries.size());
    i32 entryCount = dataCount;

    std::vector<u8> buf;

    // V25 header layout but with version 24 magic.
    writeLE32(buf, buildVersion);
    writeLE32(buf, 0);
    writeLE32(buf, 0);
    writeLE32(buf, 0);
    writeLE32(buf, 0);
    writeLE32(buf, 0);
    writeLE32(buf, u32(dataCount));
    writeLE32(buf, 0);
    writeLE32(buf, u32(entryCount));
    writeLE32(buf, 0x18000000u);    // magic: version 24, non-encrypted

    // CMF entries.
    for (i32 i = 0; i < entryCount; ++i) {
        writeLE32(buf, u32(i + 1));
        writeLE64(buf, hashEntries[size_t(i)].first);
        writeLE64(buf, 0);
    }

    // Hash data v24 (28 bytes each): GUID(8) + Size(4) + CKey(16) — no Unknown byte.
    for (auto& [guid, ckey] : hashEntries) {
        writeLE64(buf, guid);
        writeLE32(buf, 512);
        writeBytes(buf, ckey.data(), 16);
    }

    return buf;
}

/// Build a minimal unencrypted CMF v26 blob (1.48+).
static std::vector<u8> buildCmfV26(
    const std::vector<std::pair<u64, std::array<u8, 16>>>& hashEntries,
    u32 buildVersion = 70000) {

    i32 dataCount = i32(hashEntries.size());
    i32 entryCount = dataCount;

    std::vector<u8> buf;

    // V26 header (48 bytes):
    // buildVersion(4) + unk04(4) + unk08(4) + unk0C(4) + unk10(4) + unk14(4)
    // + unk18(4) + dataPatchRecordCount(4) + dataCount(4) + entryPatchRecordCount(4)
    // + entryCount(4) + magic(4)
    writeLE32(buf, buildVersion);
    writeLE32(buf, 0);              // unk04
    writeLE32(buf, 0);              // unk08
    writeLE32(buf, 0);              // unk0C
    writeLE32(buf, 0);              // unk10
    writeLE32(buf, 0);              // unk14
    writeLE32(buf, 0);              // unk18
    writeLE32(buf, 0);              // dataPatchRecordCount
    writeLE32(buf, u32(dataCount)); // dataCount
    writeLE32(buf, 0);              // entryPatchRecordCount
    writeLE32(buf, u32(entryCount));// entryCount
    writeLE32(buf, 0x1A000000u);    // magic: version 26, non-encrypted

    // CMF entries.
    for (i32 i = 0; i < entryCount; ++i) {
        writeLE32(buf, u32(i + 1));
        writeLE64(buf, hashEntries[size_t(i)].first);
        writeLE64(buf, 0);
    }

    // Hash data v25+ (29 bytes each).
    for (auto& [guid, ckey] : hashEntries) {
        writeLE64(buf, guid);
        writeLE32(buf, 2048);
        buf.push_back(0);
        writeBytes(buf, ckey.data(), 16);
    }

    return buf;
}

// ============================================================================
// Tests — Text root parsing
// ============================================================================

TEST_CASE("OW text root parse basic", "[casc][ow_root]") {
    std::vector<OwRootFileEntry> entries = {
        {"1", makeCKey(0x10), 0, 1, 0, "Win_SPWin_RCN_LenUS_TenUS.cmf", ""},
        {"2", makeCKey(0x20), 0, 1, 0, "Win_SPWin_RCN_LenUS_TenUS.apm", ""},
        {"3", makeCKey(0x30), 0, 1, 0, "Win_SPWin_RCN_LenUS_TenUS.trg", ""},
    };

    auto blob = buildTextRoot(entries);
    auto root = OwRoot::parse(blob);
    REQUIRE(root != nullptr);
    CHECK(root->format() == RootFormat::Overwatch);

    // Should have 3 manifest entries.
    CHECK(root->manifestEntries().size() == 3);
    CHECK(root->manifestEntries()[0].fileName == "Win_SPWin_RCN_LenUS_TenUS.cmf");
    CHECK(root->manifestEntries()[1].fileName == "Win_SPWin_RCN_LenUS_TenUS.apm");
    CHECK(root->manifestEntries()[2].fileName == "Win_SPWin_RCN_LenUS_TenUS.trg");

    // Should have 3 root entries for the manifest files themselves.
    CHECK(root->entryCount() == 3);
}

TEST_CASE("OW text root with multiple manifest files", "[casc][ow_root]") {
    std::vector<OwRootFileEntry> entries = {
        {"1", makeCKey(0x10), 0, 1, 0, "Win_SPWin_RCN.cmf", ""},
        {"2", makeCKey(0x20), 0, 1, 0, "Win_SPWin_RCN.apm", ""},
        {"3", makeCKey(0x30), 0, 1, 0, "Win_SPWin_RCN.trg", ""},
        {"4", makeCKey(0x40), 0, 1, 0, "Win_SPWin_RCN_LenUS.cmf", ""},
        {"5", makeCKey(0x50), 0, 1, 0, "Win_SPWin_RCN_LenUS.apm", ""},
        {"6", makeCKey(0x60), 0, 1, 0, "Win_SPWin_RCN_LenUS.trg", ""},
        {"7", makeCKey(0x70), 0, 1, 0, "Win_SPWin_RCN_speech_LenUS.cmf", ""},
    };

    auto blob = buildTextRoot(entries);
    auto root = OwRoot::parse(blob);
    REQUIRE(root != nullptr);

    CHECK(root->manifestEntries().size() == 7);
    CHECK(root->entryCount() == 7);
}

TEST_CASE("OW text root with CR+LF line endings", "[casc][ow_root]") {
    // Build a root with \r\n line endings.
    std::string text =
        "#MD5|FILENAME\r\n"
        + hexStr(makeCKey(0x10)) + "|test.cmf\r\n"
        + hexStr(makeCKey(0x20)) + "|test.apm\r\n";

    std::vector<u8> blob(text.begin(), text.end());
    auto root = OwRoot::parse(blob);
    REQUIRE(root != nullptr);

    CHECK(root->manifestEntries().size() == 2);
    CHECK(root->manifestEntries()[0].fileName == "test.cmf");
    CHECK(root->manifestEntries()[1].fileName == "test.apm");
}

TEST_CASE("OW text root minimal columns", "[casc][ow_root]") {
    // Only MD5 and FILENAME columns.
    std::string text =
        "#MD5|FILENAME\n"
        + hexStr(makeCKey(0xAA)) + "|minimal.cmf\n";

    std::vector<u8> blob(text.begin(), text.end());
    auto root = OwRoot::parse(blob);
    REQUIRE(root != nullptr);
    CHECK(root->manifestEntries().size() == 1);
    CHECK(root->manifestEntries()[0].md5 == makeCKey(0xAA));
}

TEST_CASE("OW text root rejects non-text data", "[casc][ow_root]") {
    // Binary data that doesn't start with '#'.
    std::vector<u8> binary = {0x00, 0x01, 0x02, 0x03};
    CHECK(OwRoot::parse(binary) == nullptr);

    // Empty data.
    std::vector<u8> empty;
    CHECK(OwRoot::parse(empty) == nullptr);
}

TEST_CASE("OW text root rejects header without required columns", "[casc][ow_root]") {
    // Header missing FILENAME.
    std::string text = "#MD5|PRIORITY\n" + hexStr(makeCKey(0x10)) + "|1\n";
    std::vector<u8> blob(text.begin(), text.end());
    CHECK(OwRoot::parse(blob) == nullptr);

    // Header missing MD5.
    std::string text2 = "#FILENAME|PRIORITY\ntest.cmf|1\n";
    std::vector<u8> blob2(text2.begin(), text2.end());
    CHECK(OwRoot::parse(blob2) == nullptr);
}

TEST_CASE("OW text root skips entries with invalid MD5", "[casc][ow_root]") {
    std::string text =
        "#MD5|FILENAME\n"
        "not_a_valid_hex|bad.cmf\n"
        + hexStr(makeCKey(0x50)) + "|good.cmf\n";

    std::vector<u8> blob(text.begin(), text.end());
    auto root = OwRoot::parse(blob);
    REQUIRE(root != nullptr);

    // Only the valid entry should be present.
    CHECK(root->manifestEntries().size() == 1);
    CHECK(root->manifestEntries()[0].fileName == "good.cmf");
}

// ============================================================================
// Tests — CMF resolution
// ============================================================================

TEST_CASE("OW root resolves CMF v25 entries", "[casc][ow_root]") {
    auto cmfCKey = makeCKey(0xA0);

    std::vector<std::pair<u64, std::array<u8, 16>>> cmfHashEntries = {
        {0x0000000100000001ULL, makeCKey(0xB0)},
        {0x0000000100000002ULL, makeCKey(0xB1)},
        {0x0000000100000003ULL, makeCKey(0xB2)},
    };

    auto cmfBlob = buildCmfV25(cmfHashEntries);

    CKeyResolver resolver = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
        if (std::memcmp(cKey.data(), cmfCKey.data(), 16) == 0)
            return cmfBlob;
        return {};
    };

    std::vector<OwRootFileEntry> manifestEntries = {
        {"1", cmfCKey, 0, 1, 0, "Win_SPWin_RCN.cmf", ""},
    };

    auto root = OwRoot::fromManifestEntries(std::move(manifestEntries), resolver);
    REQUIRE(root != nullptr);

    // 1 manifest entry + 3 CMF hash entries = 4 total.
    CHECK(root->entryCount() == 4);

    // Look up by GUID.
    auto found = root->findByGuid(0x0000000100000001ULL);
    REQUIRE_FALSE(found.empty());
    CHECK(found[0]->cKey == makeCKey(0xB0));
    CHECK(found[0]->fileSize == 1024);

    auto found2 = root->findByGuid(0x0000000100000003ULL);
    REQUIRE_FALSE(found2.empty());
    CHECK(found2[0]->cKey == makeCKey(0xB2));

    // GUID not in CMF.
    auto missing = root->findByGuid(0xDEADDEADDEADDEADULL);
    CHECK(missing.empty());
}

TEST_CASE("OW root resolves CMF v24 entries", "[casc][ow_root]") {
    auto cmfCKey = makeCKey(0xC0);

    std::vector<std::pair<u64, std::array<u8, 16>>> cmfHashEntries = {
        {0x0000000200000001ULL, makeCKey(0xD0)},
        {0x0000000200000002ULL, makeCKey(0xD1)},
    };

    auto cmfBlob = buildCmfV24(cmfHashEntries);

    CKeyResolver resolver = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
        if (std::memcmp(cKey.data(), cmfCKey.data(), 16) == 0)
            return cmfBlob;
        return {};
    };

    std::vector<OwRootFileEntry> manifestEntries = {
        {"1", cmfCKey, 0, 1, 0, "Win_SPWin_RCN.cmf", ""},
    };

    auto root = OwRoot::fromManifestEntries(std::move(manifestEntries), resolver);
    REQUIRE(root != nullptr);

    // 1 manifest + 2 CMF = 3 entries.
    CHECK(root->entryCount() == 3);

    auto found = root->findByGuid(0x0000000200000001ULL);
    REQUIRE_FALSE(found.empty());
    CHECK(found[0]->cKey == makeCKey(0xD0));
    CHECK(found[0]->fileSize == 512);
}

TEST_CASE("OW root resolves CMF v26 entries", "[casc][ow_root]") {
    auto cmfCKey = makeCKey(0xE0);

    std::vector<std::pair<u64, std::array<u8, 16>>> cmfHashEntries = {
        {0x0000000300000001ULL, makeCKey(0xF0)},
        {0x0000000300000002ULL, makeCKey(0xF1)},
        {0x0000000300000003ULL, makeCKey(0xF2)},
        {0x0000000300000004ULL, makeCKey(0xF3)},
    };

    auto cmfBlob = buildCmfV26(cmfHashEntries);

    CKeyResolver resolver = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
        if (std::memcmp(cKey.data(), cmfCKey.data(), 16) == 0)
            return cmfBlob;
        return {};
    };

    std::vector<OwRootFileEntry> manifestEntries = {
        {"1", cmfCKey, 0, 1, 0, "Win_SPWin_RCN.cmf", ""},
    };

    auto root = OwRoot::fromManifestEntries(std::move(manifestEntries), resolver);
    REQUIRE(root != nullptr);

    // 1 manifest + 4 CMF = 5 entries.
    CHECK(root->entryCount() == 5);

    auto found = root->findByGuid(0x0000000300000004ULL);
    REQUIRE_FALSE(found.empty());
    CHECK(found[0]->cKey == makeCKey(0xF3));
    CHECK(found[0]->fileSize == 2048);
}

TEST_CASE("OW root resolves multiple CMF files", "[casc][ow_root]") {
    auto cmfCKey1 = makeCKey(0x10);
    auto cmfCKey2 = makeCKey(0x20);
    auto apmCKey = makeCKey(0x30); // Should not be resolved as CMF.

    std::vector<std::pair<u64, std::array<u8, 16>>> cmf1Entries = {
        {0x0001000000000001ULL, makeCKey(0x41)},
        {0x0001000000000002ULL, makeCKey(0x42)},
    };
    std::vector<std::pair<u64, std::array<u8, 16>>> cmf2Entries = {
        {0x0002000000000001ULL, makeCKey(0x51)},
    };

    auto cmfBlob1 = buildCmfV25(cmf1Entries);
    auto cmfBlob2 = buildCmfV25(cmf2Entries);

    CKeyResolver resolver = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
        if (std::memcmp(cKey.data(), cmfCKey1.data(), 16) == 0) return cmfBlob1;
        if (std::memcmp(cKey.data(), cmfCKey2.data(), 16) == 0) return cmfBlob2;
        return {};
    };

    std::vector<OwRootFileEntry> manifestEntries = {
        {"1", cmfCKey1, 0, 1, 0, "Win_SPWin_RCN.cmf", ""},
        {"2", cmfCKey2, 0, 1, 0, "Win_SPWin_RCN_speech_LenUS.cmf", ""},
        {"3", apmCKey, 0, 1, 0, "Win_SPWin_RCN.apm", ""},
    };

    auto root = OwRoot::fromManifestEntries(std::move(manifestEntries), resolver);
    REQUIRE(root != nullptr);

    // 3 manifest + 2 cmf1 + 1 cmf2 = 6 entries.
    CHECK(root->entryCount() == 6);

    // Entries from CMF 1.
    auto found1 = root->findByGuid(0x0001000000000001ULL);
    REQUIRE_FALSE(found1.empty());
    CHECK(found1[0]->cKey == makeCKey(0x41));

    // Entries from CMF 2.
    auto found2 = root->findByGuid(0x0002000000000001ULL);
    REQUIRE_FALSE(found2.empty());
    CHECK(found2[0]->cKey == makeCKey(0x51));
}

TEST_CASE("OW root without resolver only parses manifests", "[casc][ow_root]") {
    std::vector<OwRootFileEntry> manifestEntries = {
        {"1", makeCKey(0x10), 0, 1, 0, "Win_SPWin_RCN.cmf", ""},
        {"2", makeCKey(0x20), 0, 1, 0, "Win_SPWin_RCN.apm", ""},
    };

    auto blob = buildTextRoot(manifestEntries);
    auto root = OwRoot::parse(blob, nullptr); // No resolver.
    REQUIRE(root != nullptr);

    // Only manifest entries, no CMF data.
    CHECK(root->entryCount() == 2);
}

TEST_CASE("OW root with failing resolver is lenient", "[casc][ow_root]") {
    CKeyResolver resolver = [](std::span<const u8, 16>) -> std::vector<u8> {
        return {}; // Always fails.
    };

    std::vector<OwRootFileEntry> manifestEntries = {
        {"1", makeCKey(0x10), 0, 1, 0, "Win_SPWin_RCN.cmf", ""},
    };

    auto root = OwRoot::fromManifestEntries(std::move(manifestEntries), resolver);
    REQUIRE(root != nullptr);

    // Only the manifest entry itself (CMF resolution failed gracefully).
    CHECK(root->entryCount() == 1);
}

// ============================================================================
// Tests — Path lookup
// ============================================================================

TEST_CASE("OW root findByPath for manifest entries", "[casc][ow_root]") {
    std::vector<OwRootFileEntry> entries = {
        {"1", makeCKey(0x10), 0, 1, 0, "Win_SPWin_RCN.cmf", ""},
        {"2", makeCKey(0x20), 0, 1, 0, "Win_SPWin_RCN.apm", ""},
    };

    auto blob = buildTextRoot(entries);
    auto root = OwRoot::parse(blob);
    REQUIRE(root != nullptr);

    // Path lookup should match (case-insensitive).
    auto found = root->findByPath("Win_SPWin_RCN.cmf");
    REQUIRE_FALSE(found.empty());
    CHECK(found[0]->cKey == makeCKey(0x10));

    // Case-insensitive.
    auto foundUpper = root->findByPath("WIN_SPWIN_RCN.CMF");
    REQUIRE_FALSE(foundUpper.empty());
    CHECK(foundUpper[0]->cKey == makeCKey(0x10));

    // Non-existent.
    auto missing = root->findByPath("nonexistent.cmf");
    CHECK(missing.empty());
}

TEST_CASE("OW root findByPath for CMF content entries", "[casc][ow_root]") {
    auto cmfCKey = makeCKey(0x10);
    std::vector<std::pair<u64, std::array<u8, 16>>> cmfHashEntries = {
        {12345ULL, makeCKey(0xB0)},
    };

    auto cmfBlob = buildCmfV25(cmfHashEntries);
    CKeyResolver resolver = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
        if (std::memcmp(cKey.data(), cmfCKey.data(), 16) == 0)
            return cmfBlob;
        return {};
    };

    std::vector<OwRootFileEntry> manifestEntries = {
        {"1", cmfCKey, 0, 1, 0, "Win_SPWin_RCN.cmf", ""},
    };

    auto root = OwRoot::fromManifestEntries(std::move(manifestEntries), resolver);
    REQUIRE(root != nullptr);

    // CMF entries get paths like "win_spwin_rcn\12345".
    auto found = root->findByPath("win_spwin_rcn\\12345");
    REQUIRE_FALSE(found.empty());
    CHECK(found[0]->cKey == makeCKey(0xB0));
}

// ============================================================================
// Tests — FileDataId (should return empty for OW)
// ============================================================================

TEST_CASE("OW root findByFileDataId always empty", "[casc][ow_root]") {
    std::vector<OwRootFileEntry> entries = {
        {"1", makeCKey(0x10), 0, 1, 0, "test.cmf", ""},
    };

    auto blob = buildTextRoot(entries);
    auto root = OwRoot::parse(blob);
    REQUIRE(root != nullptr);

    CHECK(root->findByFileDataId(1).empty());
    CHECK(root->findByFileDataId(0).empty());
    CHECK(root->findByFileDataId(999999).empty());
}

// ============================================================================
// Tests — Enumerate
// ============================================================================

TEST_CASE("OW root enumerate all entries", "[casc][ow_root]") {
    auto cmfCKey = makeCKey(0x10);
    std::vector<std::pair<u64, std::array<u8, 16>>> cmfHashEntries = {
        {100ULL, makeCKey(0xA0)},
        {200ULL, makeCKey(0xA1)},
        {300ULL, makeCKey(0xA2)},
    };

    auto cmfBlob = buildCmfV25(cmfHashEntries);
    CKeyResolver resolver = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
        if (std::memcmp(cKey.data(), cmfCKey.data(), 16) == 0) return cmfBlob;
        return {};
    };

    std::vector<OwRootFileEntry> manifestEntries = {
        {"1", cmfCKey, 0, 1, 0, "root.cmf", ""},
    };

    auto root = OwRoot::fromManifestEntries(std::move(manifestEntries), resolver);
    REQUIRE(root != nullptr);

    // 1 manifest + 3 CMF = 4.
    size_t count = 0;
    root->enumerate([&](const RootEntry&) { ++count; return true; });
    CHECK(count == 4);

    // Early termination.
    size_t partial = 0;
    root->enumerate([&](const RootEntry&) { ++partial; return partial < 2; });
    CHECK(partial == 2);
}

// ============================================================================
// Tests — Auto-detection via RootManifest::parse
// ============================================================================

TEST_CASE("Auto-detection routes OW text root to OwRoot", "[casc][ow_root]") {
    std::vector<OwRootFileEntry> entries = {
        {"1", makeCKey(0x10), 0, 1, 0, "Win_SPWin_RCN.cmf", ""},
    };

    auto blob = buildTextRoot(entries);
    auto root = RootManifest::parse(blob);
    REQUIRE(root != nullptr);
    CHECK(root->format() == RootFormat::Overwatch);
    CHECK(root->entryCount() == 1);
}

TEST_CASE("Auto-detection does not misidentify binary roots as OW", "[casc][ow_root]") {
    // WoW MFST root (starts with 'M', not '#').
    std::vector<u8> mfstBlob = {0x4D, 0x46, 0x53, 0x54, 0x01, 0x00, 0x00, 0x00};
    // RootManifest::parse should NOT return an OwRoot here.
    // (It will likely fail or return a WoW root attempt.)
    auto root = RootManifest::parse(mfstBlob);
    if (root) {
        CHECK(root->format() != RootFormat::Overwatch);
    }
}

// ============================================================================
// Tests — Edge cases
// ============================================================================

TEST_CASE("OW root edge cases", "[casc][ow_root]") {
    // Empty data.
    std::vector<u8> empty;
    CHECK(OwRoot::parse(empty) == nullptr);

    // Single byte '#' only.
    std::vector<u8> justHash = {'#'};
    CHECK(OwRoot::parse(justHash) == nullptr);

    // Header only, no data rows.
    std::string headerOnly = "#MD5|FILENAME\n";
    std::vector<u8> blob(headerOnly.begin(), headerOnly.end());
    CHECK(OwRoot::parse(blob) == nullptr);
}

TEST_CASE("OW root handles CMF with zero entries", "[casc][ow_root]") {
    auto cmfCKey = makeCKey(0x10);

    // Build a CMF with 0 entries.
    std::vector<std::pair<u64, std::array<u8, 16>>> noEntries;
    auto cmfBlob = buildCmfV25(noEntries);

    CKeyResolver resolver = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
        if (std::memcmp(cKey.data(), cmfCKey.data(), 16) == 0) return cmfBlob;
        return {};
    };

    std::vector<OwRootFileEntry> manifestEntries = {
        {"1", cmfCKey, 0, 1, 0, "empty.cmf", ""},
    };

    auto root = OwRoot::fromManifestEntries(std::move(manifestEntries), resolver);
    REQUIRE(root != nullptr);

    // Only the manifest entry.
    CHECK(root->entryCount() == 1);
}

TEST_CASE("OW root ignores non-CMF files during resolution", "[casc][ow_root]") {
    bool resolverCalled = false;

    CKeyResolver resolver = [&](std::span<const u8, 16>) -> std::vector<u8> {
        resolverCalled = true;
        return {};
    };

    // Only .apm and .trg files, no .cmf.
    std::vector<OwRootFileEntry> manifestEntries = {
        {"1", makeCKey(0x10), 0, 1, 0, "Win_SPWin_RCN.apm", ""},
        {"2", makeCKey(0x20), 0, 1, 0, "Win_SPWin_RCN.trg", ""},
    };

    auto root = OwRoot::fromManifestEntries(std::move(manifestEntries), resolver);
    REQUIRE(root != nullptr);

    // Resolver should not be called for non-CMF files.
    CHECK_FALSE(resolverCalled);
    CHECK(root->entryCount() == 2);
}

TEST_CASE("OW root handles truncated CMF data gracefully", "[casc][ow_root]") {
    auto cmfCKey = makeCKey(0x10);

    // Return only 10 bytes — too small for a CMF header.
    CKeyResolver resolver = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
        if (std::memcmp(cKey.data(), cmfCKey.data(), 16) == 0)
            return std::vector<u8>(10, 0);
        return {};
    };

    std::vector<OwRootFileEntry> manifestEntries = {
        {"1", cmfCKey, 0, 1, 0, "truncated.cmf", ""},
    };

    auto root = OwRoot::fromManifestEntries(std::move(manifestEntries), resolver);
    REQUIRE(root != nullptr);

    // Only the manifest entry (CMF parsing failed gracefully).
    CHECK(root->entryCount() == 1);
}
