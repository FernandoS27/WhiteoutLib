// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// The TXTR format was reverse-engineered from the client, and txtr_synth_test
// checks the parser against headers this file builds to that reading.  That
// proves the parser matches the notes, not that the notes match Overwatch.
// This decodes textures out of an installed copy of the game instead.
//
// The install is found through the Battle.net game finder, so there is nothing
// to configure; WHITEOUT_OW_PATH overrides it. Without either, the test skips.

#include "../src/whiteout/storages/casc/roots/ow/ow_asset_types.h"

#include <whiteout/storages/casc/storage.h>
#include <whiteout/textures/txtr/txtr.h>
#include <whiteout/utils/blizzard_game_finder.h>
#include <whiteout/utils/simple_thread_pool.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;
using namespace whiteout::textures;
namespace ow = whiteout::storages::casc::ow;

namespace {

/// How many textures to decode. The install holds over a million, so this is a
/// sample; kStride spreads it across the whole walk rather than taking the
/// front, which would be one manifest's worth of one asset kind.
constexpr size_t kSampleSize = 400;
constexpr size_t kStride = 1500;

std::string installPath() {
    if (const char* env = std::getenv("WHITEOUT_OW_PATH"))
        return env;
    for (const auto& game : utils::findBlizzardGames()) {
        if (game.game == utils::BlizzardGame::Overwatch2)
            return game.path;
    }
    return {};
}

std::string hex16(u64 value) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i, value >>= 4)
        out[size_t(i)] = kDigits[value & 0xF];
    return out;
}

/// The GUID an asset path ends with, or 0 when it carries none. Asset paths are
/// `<manifest>\<16 hex digits>.<type>`; manifest rows are plain names.
u64 trailingGuid(std::string_view path) {
    if (auto const dot = path.find_last_of('.'); dot != std::string_view::npos)
        path = path.substr(0, dot);
    if (path.size() < 16)
        return 0;
    u64 value = 0;
    for (char c : path.substr(path.size() - 16)) {
        if (c >= '0' && c <= '9')
            value = (value << 4) | u64(c - '0');
        else if (c >= 'a' && c <= 'f')
            value = (value << 4) | u64(c - 'a' + 10);
        else
            return 0;
    }
    return value;
}

/// A payload lives in the manifest its texture does, so its path is the
/// texture's with the GUID swapped. The type extension is left off — the root
/// matches an asset path without it, and this way the lookup does not depend on
/// the payload type carrying a name.
std::string siblingPath(std::string_view texturePath, u64 payloadGuid) {
    auto const slash = texturePath.find_last_of('\\');
    auto const dir =
        slash == std::string_view::npos ? std::string_view{} : texturePath.substr(0, slash + 1);
    return std::string(dir) + hex16(payloadGuid);
}

} // namespace

TEST_CASE("TXTR decodes textures out of an Overwatch install", "[txtr][overwatch][corpus]") {
    const std::string path = installPath();
    if (path.empty())
        SKIP("no Overwatch install found; set WHITEOUT_OW_PATH to override");

    utils::SimpleThreadPool pool(8);

    OpenOptions opts;
    opts.path = path;
    opts.product = "pro";
    opts.localeMask = LocaleMasks::enUS;
    opts.pool = &pool;
    std::string error;
    opts.errorOut = &error;

    auto storage = Storage::open(opts);
    if (!storage) {
        WARN("open failed: " << error);
        SKIP("could not open the install at " + path);
    }

    std::cout << "Overwatch at " << path;
    if (auto product = storage->product())
        std::cout << " (" << product->version << ")";
    std::cout << "\n";

    std::vector<std::string> headers;
    size_t seen = 0;
    storage->enumerate([&](const EnumerateEntry& e) {
        u64 const guid = trailingGuid(e.path);
        if (guid == 0 || !txtr::isTextureGuid(guid))
            return true;
        if ((seen++ % kStride) == 0)
            headers.emplace_back(e.path);
        return headers.size() < kSampleSize;
    });

    INFO("textures walked: " << seen);
    REQUIRE(headers.size() == kSampleSize);

    struct Stats {
        size_t decoded = 0;
        size_t selfContained = 0;
        size_t withPayloads = 0;
        size_t payloadsRead = 0;
        size_t payloadsMissing = 0;
        size_t noEncoding = 0;
        size_t notInstalled = 0;
        u64 pixelBytes = 0;
        u32 widest = 0;
    };
    Stats stats;
    std::map<PixelFormat, size_t> formats;
    std::map<u32, size_t> rejectedCodes;

    for (const auto& headerPath : headers) {
        INFO(headerPath);
        u64 const guid = trailingGuid(headerPath);

        // The manifests name every asset the product has, not every one this
        // machine downloaded, so a listed texture need not be on disk.
        auto header = storage->readFile(headerPath);
        if (!header) {
            ++stats.notInstalled;
            continue;
        }

        // Every surface format the install ships now has a library encoding,
        // so this should stay empty; a code turning up here means either a new
        // format in a patch or a regression in the mapping table.
        txtr::Parser detector;
        if (!detector.detect(*header)) {
            ++stats.noEncoding;
            ++rejectedCodes[(*header)[3]];
            continue;
        }

        // Payload 0 is the block inside the header, so a count of 0 or 1 means
        // the texture is self-contained.
        auto const guids = txtr::Parser::payloadGuids(*header, guid);
        REQUIRE(guids.size() == (txtr::Parser::payloadCount(*header) > 0
                                     ? txtr::Parser::payloadCount(*header) - 1
                                     : 0));

        std::vector<std::vector<u8>> owned;
        for (u64 payloadGuid : guids) {
            CHECK(ow::assetTypeId(payloadGuid) == txtr::kAssetTypeTexturePayload);
            auto data = storage->readFile(siblingPath(headerPath, payloadGuid));
            if (!data) {
                ++stats.payloadsMissing;
                continue;
            }
            ++stats.payloadsRead;
            owned.push_back(std::move(*data));
        }

        std::vector<std::span<const u8>> payloads(owned.begin(), owned.end());

        txtr::Parser parser;
        txtr::TxtrInfo info;
        auto texture = parser.parse(*header, payloads, &info);

        INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
        REQUIRE(texture.has_value());

        // Every payload the header named was found, so the whole chain decoded
        // and the texture is the size the header declares.
        REQUIRE(info.missingPayloads == 0);
        CHECK(info.baseMip == 0);
        CHECK(texture->width() == info.width);
        CHECK(texture->height() == info.height);
        REQUIRE(texture->mipCount() >= 1);

        // The mip chain is what the payloads carried, so a wrong reassembly
        // shows up as a level that is not the size its dimensions imply.
        for (u32 mip = 0; mip < texture->mipCount(); ++mip) {
            u32 const w = std::max(texture->width() >> mip, 1u);
            u32 const h = std::max(texture->height() >> mip, 1u);
            INFO("mip " << mip << " of " << texture->mipCount());
            CHECK(texture->mipData(mip).size() == computeImageSize(texture->format(), w, h));
        }

        ++stats.decoded;
        if (guids.empty())
            ++stats.selfContained;
        else
            ++stats.withPayloads;
        stats.pixelBytes += texture->mipData(0).size();
        stats.widest = std::max(stats.widest, texture->width());
        ++formats[texture->format()];
    }

    std::cout << "=== TXTR corpus ===\n  " << stats.decoded << " decoded (" << stats.withPayloads
              << " from payloads, " << stats.selfContained << " self-contained), "
              << stats.noEncoding << " in formats the library cannot hold\n  " << stats.payloadsRead
              << " payload files read, " << stats.payloadsMissing << " missing\n  widest "
              << stats.widest << "px, " << (stats.pixelBytes >> 20)
              << " MiB of mip 0 across the sample\n  formats:";
    for (const auto& [format, count] : formats)
        std::cout << " " << int(format) << "=" << count;
    std::cout << std::endl;

    std::cout << "  surface codes with no encoding:";
    for (const auto& [code, count] : rejectedCodes)
        std::cout << " " << code << "=" << count;
    std::cout << std::endl;

    CHECK(stats.decoded + stats.noEncoding + stats.notInstalled == kSampleSize);
    CHECK(stats.payloadsMissing == 0);
    CHECK(stats.withPayloads > 0);

    // A regression that rejected a whole class of texture would otherwise pass
    // on whatever still decoded, so both the count and the spread are pinned.
    CHECK(stats.noEncoding == 0);
    CHECK(formats.count(PixelFormat::RGBA16F) > 0);
    CHECK(formats.count(PixelFormat::BC1) > 0);
    CHECK(formats.count(PixelFormat::BC7) > 0);
}
