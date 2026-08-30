// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_wow_tvfs_root_test
///
/// Verifies the WoW retail TVFS decorator end-to-end:
///   1. The encoded-path decoder (`wow_tvfs_path::tryDecode`) accepts the
///      53- and 57-char retail forms, strips sub-manifest container prefixes,
///      and rejects ill-formed inputs.
///   2. `WowTvfsRoot::looksLikeWowTvfs` recognises a synthetic WoW TVFS blob
///      whose first entries are sub-manifest containers (matching the real
///      retail layout where vfs-root references vfs-1, vfs-2, …).
///   3. `WowTvfsRoot::create` with a community-listfile populates each
///      decoded entry's path from FileDataId → string, and leaves entries
///      that don't match the listfile path-less so `listFiles()` filters
///      them out (no hashed strings should leak through).
///
/// We don't depend on any corpus data — every TVFS blob in this file is
/// hand-built from the spec.

#include "../src/whiteout/storages/casc/roots/common/wow_tvfs_path.h"
#include "../src/whiteout/storages/casc/roots/tvfs_root.h"
#include "../src/whiteout/storages/casc/roots/wow_tvfs_root.h"
#include "../src/whiteout/storages/casc/storage/constants.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

// ============================================================================
// Path decoder unit tests
// ============================================================================

namespace {

/// Build the canonical encoded leaf name for a WoW retail file.
///   length 53 (build 46144+):  LLLLLLLL CCCC : IIIIIIII <32-hex CKey>
///   length 57 (build 63728+):  LLLLLLLL CCCCCCCC : IIIIIIII <32-hex CKey>
std::string makeEncodedName(u32 locale, u32 content, u32 fdid,
                            const std::array<u8, 16>& cKey,
                            bool wideContent) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    auto pushHex = [&](u32 v, size_t nibbles) {
        for (size_t i = nibbles; i-- > 0;)
            out.push_back(kHex[(v >> (i * 4)) & 0xF]);
    };
    pushHex(locale, 8);
    pushHex(content, wideContent ? 8 : 4);
    out.push_back(':');
    pushHex(fdid, 8);
    for (auto b : cKey) {
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

std::array<u8, 16> makeCKey(u8 seed) {
    std::array<u8, 16> k{};
    for (int i = 0; i < 16; ++i) k[i] = u8(seed + i);
    return k;
}

} // namespace

TEST_CASE("wow_tvfs_path: decode 53-char retail form", "[casc][wow_tvfs_path]") {
    // Reference encoding: locale=0x00000002, content=0x0000, fdid=0x000C472F,
    // cKey=02BA924C604A670B253AA02DBCD9441C (taken from CascRootFile_TVFS.cpp:739).
    const std::array<u8, 16> cKey = {
        0x02, 0xBA, 0x92, 0x4C, 0x60, 0x4A, 0x67, 0x0B,
        0x25, 0x3A, 0xA0, 0x2D, 0xBC, 0xD9, 0x44, 0x1C,
    };
    auto encoded = makeEncodedName(0x00000002, 0x0000, 0x000C472F, cKey, /*wide=*/false);
    REQUIRE(encoded.size() == 53);
    REQUIRE(encoded == "000000020000:000c472f02ba924c604a670b253aa02dbcd9441c");
    // (canonical-form sanity above; rest of suite exercises the decoder.)

    wow_tvfs_path::Info info;
    REQUIRE(wow_tvfs_path::tryDecode(encoded, info));
    CHECK(info.localeFlags == 0x00000002);
    CHECK(info.contentFlags == 0x0000);
    CHECK(info.fileDataId == 0x000C472F);
    CHECK(info.cKey == cKey);
}

TEST_CASE("wow_tvfs_path: decode 57-char retail form (build 63728+)",
          "[casc][wow_tvfs_path]") {
    auto cKey = makeCKey(0x40);
    auto encoded = makeEncodedName(0xDEADBEEF, 0x12345678, 0x01020304,
                                    cKey, /*wide=*/true);
    REQUIRE(encoded.size() == 57);
    REQUIRE(encoded[16] == ':');

    wow_tvfs_path::Info info;
    REQUIRE(wow_tvfs_path::tryDecode(encoded, info));
    CHECK(info.localeFlags == 0xDEADBEEF);
    CHECK(info.contentFlags == 0x12345678);
    CHECK(info.fileDataId == 0x01020304);
    CHECK(info.cKey == cKey);
}

TEST_CASE("wow_tvfs_path: decode under sub-manifest prefix", "[casc][wow_tvfs_path]") {
    auto cKey = makeCKey(0x10);
    auto encoded = makeEncodedName(0xFF, 0x100, 0x42, cKey, false);

    SECTION("single prefix") {
        std::string path = "vfs-1:" + encoded;
        wow_tvfs_path::Info info;
        REQUIRE(wow_tvfs_path::tryDecode(path, info));
        CHECK(info.fileDataId == 0x42);
        CHECK(info.cKey == cKey);
    }

    SECTION("nested prefix") {
        std::string path = "top:vfs-1:" + encoded;
        wow_tvfs_path::Info info;
        REQUIRE(wow_tvfs_path::tryDecode(path, info));
        CHECK(info.localeFlags == 0xFF);
        CHECK(info.contentFlags == 0x100);
    }

    SECTION("57-char form with prefix") {
        auto encoded57 = makeEncodedName(1, 2, 0x12345678, cKey, true);
        std::string path = "vfs-2:" + encoded57;
        wow_tvfs_path::Info info;
        REQUIRE(wow_tvfs_path::tryDecode(path, info));
        CHECK(info.fileDataId == 0x12345678);
    }
}

TEST_CASE("wow_tvfs_path: rejects non-encoded inputs", "[casc][wow_tvfs_path]") {
    wow_tvfs_path::Info info;

    SECTION("empty path") { CHECK_FALSE(wow_tvfs_path::tryDecode("", info)); }

    SECTION("container entry (trailing colon, no encoded segment)") {
        CHECK_FALSE(wow_tvfs_path::tryDecode("vfs-1:", info));
    }

    SECTION("plain filename") {
        CHECK_FALSE(wow_tvfs_path::tryDecode("dbfilesclient/spell.db2", info));
    }

    SECTION("buggy 52-char build-45779 form (CascLib also rejects)") {
        // 52 chars: missing last CKey nibble.
        std::string s = "000000020000:000c472f02ba924c604a670b253aa02dbcd9441";
        REQUIRE(s.size() == 52);
        CHECK_FALSE(wow_tvfs_path::tryDecode(s, info));
    }

    SECTION("colon at wrong position (53 chars, colon at 11)") {
        // 11-char field + ':' + 41-char field — both 53 total but colon mis-aligned.
        std::string s(11, '0');
        s.push_back(':');
        s.append(41, '0');
        REQUIRE(s.size() == 53);
        REQUIRE(s[11] == ':');
        REQUIRE(s[12] != ':');
        CHECK_FALSE(wow_tvfs_path::tryDecode(s, info));
    }

    SECTION("non-hex character in locale") {
        // 53-char path with ' ' (non-hex) at position 0.
        auto good = makeEncodedName(0, 0, 1, makeCKey(0), false);
        REQUIRE(good.size() == 53);
        good[0] = ' ';
        CHECK_FALSE(wow_tvfs_path::tryDecode(good, info));
    }

    SECTION("missing colon entirely") {
        // 53 chars, all hex, no colon.
        std::string s(53, 'a');
        CHECK_FALSE(wow_tvfs_path::tryDecode(s, info));
    }
}

TEST_CASE("wow_tvfs_path: matches() agrees with tryDecode()", "[casc][wow_tvfs_path]") {
    auto cKey = makeCKey(7);
    auto good = makeEncodedName(0, 0, 1234, cKey, false);
    CHECK(wow_tvfs_path::matches(good));
    CHECK(wow_tvfs_path::matches("vfs-1:" + good));
    CHECK_FALSE(wow_tvfs_path::matches(""));
    CHECK_FALSE(wow_tvfs_path::matches("interface/glue/loadingbar.blp"));
}

// ============================================================================
// Synthetic TVFS blob builder
// ============================================================================
//
// Builds a minimal flat TVFS root (no sub-manifest references) whose leaf
// names are arbitrary strings supplied by the test. This exercises the same
// path-table format as `serializeTvfsRoot` in writer.cpp, but stripped down
// to just what the parser needs.

namespace {

constexpr u8 kEKeySize = 9;

void writeLE32(std::vector<u8>& buf, u32 v) {
    buf.push_back(u8(v));
    buf.push_back(u8(v >> 8));
    buf.push_back(u8(v >> 16));
    buf.push_back(u8(v >> 24));
}
void writeBE32(std::vector<u8>& buf, u32 v) {
    buf.push_back(u8(v >> 24));
    buf.push_back(u8(v >> 16));
    buf.push_back(u8(v >> 8));
    buf.push_back(u8(v));
}
void writeBE16(std::vector<u8>& buf, u16 v) {
    buf.push_back(u8(v >> 8));
    buf.push_back(u8(v));
}

/// Build a TVFS blob containing one VFS leaf per supplied path, each backed
/// by a synthetic 9-byte EKey and 9-byte CKey. No sub-manifest references —
/// the entry list is exactly the file list, in order.
std::vector<u8> buildFlatTvfs(const std::vector<std::string>& paths) {
    // CFT: per-file EKey(9) + CKey(9).
    std::vector<u8> cft;
    cft.reserve(paths.size() * kEKeySize * 2);
    for (size_t i = 0; i < paths.size(); ++i) {
        for (u32 b = 0; b < kEKeySize; ++b)
            cft.push_back(u8(0xA0 + i * 17 + b));  // distinctive EKey
        for (u32 b = 0; b < kEKeySize; ++b)
            cft.push_back(u8(0xC0 + i * 31 + b));  // distinctive CKey
    }

    // CFT offset width — depends on table size.
    auto cftOffsSize = [](u32 sz) -> u32 {
        if (sz <= 0xFF) return 1;
        if (sz <= 0xFFFF) return 2;
        if (sz <= 0xFFFFFF) return 3;
        return 4;
    };
    const u32 cftOffWidth = cftOffsSize(static_cast<u32>(cft.size()));

    // VFS table: spanCount(1) + fileOffset(4 BE) + spanSize(4 BE) + cftOffset(var BE).
    std::vector<u8> vfs;
    const u32 vfsEntrySize = 1 + 4 + 4 + cftOffWidth;
    vfs.reserve(paths.size() * vfsEntrySize);
    for (size_t i = 0; i < paths.size(); ++i) {
        u32 cftOff = static_cast<u32>(i) * (kEKeySize * 2);
        vfs.push_back(1);
        writeBE32(vfs, 0);              // fileOffset (unused for read)
        writeBE32(vfs, 1024);            // spanSize
        for (int b = static_cast<int>(cftOffWidth) - 1; b >= 0; --b)
            vfs.push_back(static_cast<u8>((cftOff >> (b * 8)) & 0xFF));
    }

    // Path table: anonymous root folder containing one file node per path.
    // Inner format per file: [nameLen][name bytes][0xFF][nodeValue BE u32 = vfsOffset]
    std::vector<u8> innerPaths;
    u32 vfsOffset = 0;
    for (auto& p : paths) {
        REQUIRE(p.size() < 256);
        innerPaths.push_back(static_cast<u8>(p.size()));
        innerPaths.insert(innerPaths.end(), p.begin(), p.end());
        innerPaths.push_back(kTvfsNodeValueMarker);
        writeBE32(innerPaths, vfsOffset);   // file node value (bit 31 = 0)
        vfsOffset += vfsEntrySize;
    }
    // Wrap in anonymous root folder.
    std::vector<u8> pathTable;
    pathTable.push_back(kTvfsNodeValueMarker);
    u32 folderDataLen = 4 + static_cast<u32>(innerPaths.size());
    writeBE32(pathTable, kTvfsFolderNodeBit | folderDataLen);
    pathTable.insert(pathTable.end(), innerPaths.begin(), innerPaths.end());

    // Header (46 bytes).
    constexpr u32 kHeaderSize = 46;
    u32 pathOff = kHeaderSize;
    u32 pathSz  = static_cast<u32>(pathTable.size());
    u32 vfsOff  = pathOff + pathSz;
    u32 vfsSz   = static_cast<u32>(vfs.size());
    u32 cftOff  = vfsOff + vfsSz;
    u32 cftSz   = static_cast<u32>(cft.size());

    std::vector<u8> blob;
    blob.reserve(kHeaderSize + pathSz + vfsSz + cftSz);
    writeLE32(blob, RootSignature::kTVFS);
    blob.push_back(kTvfsFormatVersion);
    blob.push_back(static_cast<u8>(kHeaderSize));
    blob.push_back(kEKeySize);
    blob.push_back(kEKeySize);   // patchKeySize
    writeLE32(blob, 0x0001);     // flags: INCLUDE_CKEY
    writeBE32(blob, pathOff);
    writeBE32(blob, pathSz);
    writeBE32(blob, vfsOff);
    writeBE32(blob, vfsSz);
    writeBE32(blob, cftOff);
    writeBE32(blob, cftSz);
    writeBE16(blob, 4);          // maxDepth
    writeBE32(blob, 0);          // estTableOffset
    writeBE32(blob, 0);          // estTableSize

    blob.insert(blob.end(), pathTable.begin(), pathTable.end());
    blob.insert(blob.end(), vfs.begin(), vfs.end());
    blob.insert(blob.end(), cft.begin(), cft.end());
    return blob;
}

} // namespace

// ============================================================================
// looksLikeWowTvfs detection
// ============================================================================

TEST_CASE("WowTvfsRoot::looksLikeWowTvfs accepts a real WoW manifest layout",
          "[casc][wow_tvfs_root]") {
    // Synthetic vfs-1 contents: 8 encoded leaves with assorted FileDataIds.
    std::vector<std::string> paths;
    for (u32 fdid = 1; fdid <= 8; ++fdid) {
        paths.push_back(makeEncodedName(0xFFFF, 0x0000, fdid, makeCKey(u8(fdid)), false));
    }

    auto blob = buildFlatTvfs(paths);
    auto tvfs = TvfsRoot::parse(blob);
    REQUIRE(tvfs);
    REQUIRE(tvfs->entryCount() == paths.size());

    CHECK(WowTvfsRoot::looksLikeWowTvfs(*tvfs));
}

TEST_CASE("WowTvfsRoot::looksLikeWowTvfs rejects WC3-style plain TVFS",
          "[casc][wow_tvfs_root]") {
    std::vector<std::string> paths = {
        "war3.w3mod/units/critters/sheep.mdx",
        "war3.w3mod/units/critters/sheep_portrait.mdx",
        "war3.w3mod/textures/sheep.blp",
        "war3.w3mod/sound/units/sheep/sheep1.flac",
        "ui/glues/loadingscreens/loadingscreen.blp",
    };
    auto blob = buildFlatTvfs(paths);
    auto tvfs = TvfsRoot::parse(blob);
    REQUIRE(tvfs);
    CHECK_FALSE(WowTvfsRoot::looksLikeWowTvfs(*tvfs));
}

TEST_CASE("WowTvfsRoot::looksLikeWowTvfs handles container-prefixed leaves",
          "[casc][wow_tvfs_root]") {
    // Mirror what the TVFS sub-manifest path produces: each encoded leaf
    // prefixed by "vfs-1:". (The TVFS parser would emit container entries
    // *first* in real life — the lookahead in `looksLikeWowTvfs` is what
    // makes detection survive that.)
    std::vector<std::string> paths;
    paths.push_back("vfs-1:");                                     // container marker
    for (u32 fdid = 1; fdid <= 5; ++fdid) {
        paths.push_back("vfs-1:" + makeEncodedName(2, 0, fdid, makeCKey(u8(fdid)), false));
    }

    auto blob = buildFlatTvfs(paths);
    auto tvfs = TvfsRoot::parse(blob);
    REQUIRE(tvfs);
    CHECK(WowTvfsRoot::looksLikeWowTvfs(*tvfs));
}

// ============================================================================
// End-to-end: WowTvfsRoot::create with listfile
// ============================================================================

TEST_CASE("WowTvfsRoot::create surfaces only listfile-matched entries",
          "[casc][wow_tvfs_root]") {
    // Build six WoW-encoded leaves. The listfile only names three of them;
    // the other three (and the container marker) must be hidden by listFiles().
    struct Sample {
        u32 fdid;
        u32 locale;
        u32 content;
        std::array<u8, 16> cKey;
        std::string listfilePath;  // empty = not in listfile
    };
    std::vector<Sample> samples = {
        { 100, 0x000000FF, 0x0000, makeCKey(0x10), "world/maps/azeroth/azeroth.wdt" },
        { 200, 0x00000002, 0x0000, makeCKey(0x20), "interface/glue/loadingbar.blp" },
        { 300, 0x000000FF, 0x0000, makeCKey(0x30), "" },                         // hashed
        { 400, 0x000000FF, 0x0000, makeCKey(0x40), "creature/orc/grunt.m2" },
        { 500, 0x00000002, 0x0000, makeCKey(0x50), "" },                         // hashed
        { 600, 0x00000002, 0x0000, makeCKey(0x60), "" },                         // hashed
    };

    std::vector<std::string> tvfsPaths;
    tvfsPaths.push_back("vfs-1:");  // container marker — must be filtered
    for (auto& s : samples) {
        tvfsPaths.push_back("vfs-1:" + makeEncodedName(s.locale, s.content, s.fdid,
                                                         s.cKey, /*wide=*/false));
    }

    auto blob = buildFlatTvfs(tvfsPaths);
    auto tvfs = TvfsRoot::parse(blob);
    REQUIRE(tvfs);

    // Build a listfile (FileDataId;path).
    std::string listfile;
    for (auto& s : samples) {
        if (!s.listfilePath.empty()) {
            listfile += std::to_string(s.fdid);
            listfile += ';';
            listfile += s.listfilePath;
            listfile += '\n';
        }
    }
    std::span<const u8> listfileSpan(reinterpret_cast<const u8*>(listfile.data()),
                                      listfile.size());

    auto wow = WowTvfsRoot::create(std::move(tvfs), nullptr, listfileSpan);
    REQUIRE(wow);

    // listFiles-equivalent: enumerate all entries, collect non-empty paths.
    std::vector<std::string> visiblePaths;
    wow->enumerate([&](const RootEntry& e) {
        if (!e.path.empty()) visiblePaths.push_back(e.path);
        return true;
    });

    SECTION("visible set equals listfile-matched set") {
        std::vector<std::string> expected = {
            "world/maps/azeroth/azeroth.wdt",
            "interface/glue/loadingbar.blp",
            "creature/orc/grunt.m2",
        };
        std::sort(visiblePaths.begin(), visiblePaths.end());
        std::sort(expected.begin(), expected.end());
        CHECK(visiblePaths == expected);
    }

    SECTION("no leaked encoded paths") {
        for (auto& p : visiblePaths) {
            CAPTURE(p);
            CHECK_FALSE(wow_tvfs_path::matches(p));
            CHECK(p.find("vfs-1:") == std::string::npos);
        }
    }

    SECTION("findByFileDataId resolves all decoded entries (even hashed ones)") {
        for (auto& s : samples) {
            CAPTURE(s.fdid);
            auto hits = wow->findByFileDataId(s.fdid);
            REQUIRE(hits.size() == 1);
            CHECK(hits[0]->fileDataId == s.fdid);
            CHECK(hits[0]->localeFlags == s.locale);
            CHECK(hits[0]->contentFlags == s.content);
            CHECK(hits[0]->cKey == s.cKey);
        }
    }

    SECTION("findByPath works for listfile-matched entries") {
        auto hits = wow->findByPath("world/maps/azeroth/azeroth.wdt");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0]->fileDataId == 100);
    }
}

TEST_CASE("WowTvfsRoot::create without listfile keeps the encoded paths",
          "[casc][wow_tvfs_root]") {
    // No listfile → encoded form should still be queryable so headless tools
    // (e.g. dumping by FileDataId) can still see something.
    std::vector<std::string> tvfsPaths;
    for (u32 fdid = 1; fdid <= 6; ++fdid) {
        tvfsPaths.push_back(makeEncodedName(0xFF, 0, fdid, makeCKey(u8(fdid)), false));
    }
    auto blob = buildFlatTvfs(tvfsPaths);
    auto tvfs = TvfsRoot::parse(blob);
    REQUIRE(tvfs);

    auto wow = WowTvfsRoot::create(std::move(tvfs), nullptr, /*listfile=*/{});
    REQUIRE(wow);

    size_t visible = 0;
    wow->enumerate([&](const RootEntry& e) {
        if (!e.path.empty()) ++visible;
        return true;
    });
    CHECK(visible == tvfsPaths.size());
}

// ============================================================================
// Leaf decoding inside the traversal
// ============================================================================

TEST_CASE("TvfsLeafDecode decodes WoW leaves during the traversal",
          "[casc][wow_tvfs_root]") {
    struct Sample {
        u32 fdid;
        u32 locale;
        u32 content;
        std::array<u8, 16> cKey;
    };
    std::vector<Sample> samples = {
        { 100, 0x000000FF, 0x0008, makeCKey(0x10) },
        { 200, 0x00000002, 0x0000, makeCKey(0x20) },
        { 300, 0x000000FF, 0x0000, makeCKey(0x30) },
        { 400, 0x00000002, 0x0010, makeCKey(0x40) },
        { 500, 0x000000FF, 0x0000, makeCKey(0x50) },
    };

    std::vector<std::string> tvfsPaths;
    for (auto& s : samples)
        tvfsPaths.push_back(makeEncodedName(s.locale, s.content, s.fdid, s.cKey, /*wide=*/false));
    auto blob = buildFlatTvfs(tvfsPaths);

    std::string listfile;
    for (auto& s : samples)
        listfile += std::to_string(s.fdid) + ";content/" + std::to_string(s.fdid) + ".blp\n";
    std::span<const u8> listfileSpan(reinterpret_cast<const u8*>(listfile.data()),
                                      listfile.size());

    SECTION("Wow decodes the metadata and keeps the encoded name") {
        auto tvfs = TvfsRoot::parse(blob, nullptr, /*buildIdx=*/false, TvfsLeafDecode::Wow);
        REQUIRE(tvfs);
        REQUIRE(WowTvfsRoot::looksLikeWowTvfs(*tvfs));

        auto wow = WowTvfsRoot::create(std::move(tvfs), nullptr, /*listfile=*/{});
        REQUIRE(wow);
        for (auto& s : samples) {
            CAPTURE(s.fdid);
            auto hits = wow->findByFileDataId(s.fdid);
            REQUIRE(hits.size() == 1);
            CHECK(hits[0]->localeFlags == s.locale);
            CHECK(hits[0]->contentFlags == s.content);
            CHECK(hits[0]->cKey == s.cKey);
            CHECK(wow_tvfs_path::matches(hits[0]->path));
        }
    }

    SECTION("WowDropPath never materialises the encoded name") {
        auto tvfs = TvfsRoot::parse(blob, nullptr, /*buildIdx=*/false, TvfsLeafDecode::WowDropPath);
        REQUIRE(tvfs);
        tvfs->enumerate([](const RootEntry& e) {
            CHECK(e.path.empty());
            return true;
        });
        // Detection has to survive the name being gone.
        REQUIRE(WowTvfsRoot::looksLikeWowTvfs(*tvfs));

        auto wow = WowTvfsRoot::create(std::move(tvfs), nullptr, listfileSpan);
        REQUIRE(wow);
        for (auto& s : samples) {
            CAPTURE(s.fdid);
            auto hits = wow->findByFileDataId(s.fdid);
            REQUIRE(hits.size() == 1);
            CHECK(hits[0]->localeFlags == s.locale);
            CHECK(hits[0]->contentFlags == s.content);
            CHECK(hits[0]->cKey == s.cKey);
            CHECK(hits[0]->path == "content/" + std::to_string(s.fdid) + ".blp");
        }
        auto hits = wow->findByPath("content/300.blp");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0]->fileDataId == 300);
    }

    SECTION("a plain TVFS keeps its paths even under WowDropPath") {
        std::vector<std::string> plain = {
            "war3.w3mod/units/critters/sheep.mdx",
            "war3.w3mod/textures/sheep.blp",
            "ui/glues/loadingscreens/loadingscreen.blp",
        };
        auto tvfs = TvfsRoot::parse(buildFlatTvfs(plain), nullptr, /*buildIdx=*/true,
                                    TvfsLeafDecode::WowDropPath);
        REQUIRE(tvfs);
        CHECK_FALSE(WowTvfsRoot::looksLikeWowTvfs(*tvfs));
        size_t visible = 0;
        tvfs->enumerate([&](const RootEntry& e) {
            if (!e.path.empty())
                ++visible;
            return true;
        });
        CHECK(visible == plain.size());
    }
}

TEST_CASE("WowTvfsRoot path lookups ignore case and separator style with a listfile",
          "[casc][wow_tvfs_root]") {
    // Community listfiles are mixed-case with forward slashes; callers address
    // files in whatever spelling they have. Every spelling must resolve.
    struct Sample {
        u32 fdid;
        std::string listfilePath;
    };
    std::vector<Sample> samples = {
        { 100, "World/Maps/Azeroth/Azeroth.wdt" },
        { 200, "Interface\\Glue\\LoadingBar.BLP" },
        { 400, "creature/orc/grunt.m2" },
        { 500, "Sound/Music/ZoneMusic/Elwynn.mp3" },
    };

    std::vector<std::string> tvfsPaths;
    tvfsPaths.push_back("vfs-1:");
    for (auto& s : samples) {
        tvfsPaths.push_back("vfs-1:" + makeEncodedName(0xFF, 0, s.fdid, makeCKey(u8(s.fdid)),
                                                         /*wide=*/false));
    }
    auto tvfs = TvfsRoot::parse(buildFlatTvfs(tvfsPaths));
    REQUIRE(tvfs);

    std::string listfile;
    for (auto& s : samples)
        listfile += std::to_string(s.fdid) + ';' + s.listfilePath + '\n';
    std::span<const u8> listfileSpan(reinterpret_cast<const u8*>(listfile.data()),
                                      listfile.size());

    auto wow = WowTvfsRoot::create(std::move(tvfs), nullptr, listfileSpan);
    REQUIRE(wow);

    struct Query {
        std::string path;
        u32 expectFdid;
    };
    std::vector<Query> queries = {
        { "World/Maps/Azeroth/Azeroth.wdt", 100 },      // exact listfile spelling
        { "world/maps/azeroth/azeroth.wdt", 100 },      // lowercase
        { "WORLD\\MAPS\\AZEROTH\\AZEROTH.WDT", 100 },   // uppercase + backslashes
        { "/World\\Maps/Azeroth\\Azeroth.WDT", 100 },   // leading + mixed separators
        { "interface/glue/loadingbar.blp", 200 },       // listfile used backslashes + caps
        { "Interface\\Glue\\LoadingBar.BLP", 200 },
        { "CREATURE/ORC/GRUNT.M2", 400 },               // listfile was lowercase
        { "sound\\music\\zonemusic\\elwynn.MP3", 500 },
    };

    SECTION("findByPath") {
        for (auto& q : queries) {
            CAPTURE(q.path);
            auto hits = wow->findByPath(q.path);
            REQUIRE(hits.size() == 1);
            CHECK(hits[0]->fileDataId == q.expectFdid);
        }
    }

    SECTION("findByNormalizedPath / hasPath (Storage's entry points)") {
        CHECK(wow->hasPath("world\\maps\\azeroth\\azeroth.wdt"));
        CHECK(wow->hasPath("interface\\glue\\loadingbar.blp"));
        CHECK(wow->hasPath("creature\\orc\\grunt.m2"));
        auto hits = wow->findByNormalizedPath("interface\\glue\\loadingbar.blp");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0]->fileDataId == 200);
    }

    SECTION("enumerated paths keep the listfile's original spelling") {
        std::vector<std::string> seen;
        wow->enumerate([&](const RootEntry& e) {
            if (!e.path.empty()) seen.push_back(e.path);
            return true;
        });
        std::vector<std::string> expected;
        for (auto& s : samples) expected.push_back(s.listfilePath);
        std::sort(seen.begin(), seen.end());
        std::sort(expected.begin(), expected.end());
        CHECK(seen == expected);
    }

    SECTION("misses stay misses") {
        CHECK(wow->findByPath("world/maps/azeroth/azeroth.wdl").empty());
        CHECK(wow->findByPath("World/Maps/Azeroth").empty());
        CHECK_FALSE(wow->hasPath("world\\maps"));
    }
}
