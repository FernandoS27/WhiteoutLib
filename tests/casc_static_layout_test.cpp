// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Static-build-config installs — the shape Steam ships. They carry no `.idx`
// files: a `.build.config` sits beside `data.<chunk>.<uid>` archives and every
// EKey ends in the coordinates that an index entry would otherwise hold.
//
// The layout parser is checked against literal config text, so that part always
// runs. The end-to-end half needs such an install; it is found through the game
// finder, WHITEOUT_STATIC_CASC_PATH overrides it, and without either it skips.

#include "../src/whiteout/storages/casc/tables/config.h"
#include "../src/whiteout/storages/common/md5.h"

#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/blizzard_game_finder.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

namespace {

std::span<const u8> asBytes(std::string_view s) {
    return {reinterpret_cast<const u8*>(s.data()), s.size()};
}

/// A static-layout install is one whose data directory holds a `.build.config`.
bool isStaticLayout(const std::string& installPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::exists(fs::path(installPath) / "data" / ".build.config", ec) ||
           fs::exists(fs::path(installPath) / ".build.config", ec);
}

std::string findStaticInstall() {
    if (const char* env = std::getenv("WHITEOUT_STATIC_CASC_PATH"))
        return env;
    for (const auto& game : utils::findBlizzardGames()) {
        if (isStaticLayout(game.path))
            return game.path;
    }
    return {};
}

/// How many files to read back. The install holds millions, so this is a
/// sample; the stride spreads it over the whole walk rather than taking the
/// front, which would all come from one manifest.
constexpr size_t kSampleSize = 300;
constexpr size_t kStride = 977;

} // namespace

TEST_CASE("parseStaticKeyLayout reads Overwatch's key layout", "[casc][static]") {
    static constexpr std::string_view kConfig = R"(# Static Build Configuration

client-version = 3.13.4
root = 0f9aa0493adbdd005043a2f5b8e6a111
encoding = abeb4384b181ddeb11a367102cf9de3c fc4bea055c46c622a200000000000000
encoding-size = 117015532 115985871
key-layout-index-bits = 0
key-layout-0 = 8 8 40 1
build-uid = pro
)";

    auto layout = parseStaticKeyLayout(asBytes(kConfig));
    REQUIRE(layout.valid);
    CHECK(layout.chunkBytes == 1);
    CHECK(layout.uidBytes == 1);
    CHECK(layout.offsetBytes == 5);
    CHECK(layout.hashBytes == 9);

    // The same file still parses as an ordinary build config.
    auto build = parseBuildConfig(asBytes(kConfig));
    CHECK(build.buildUid == "pro");
    CHECK(build.encodingSize == 117015532);
}

TEST_CASE("parseStaticKeyLayout rejects layouts it cannot decode", "[casc][static]") {
    // An ordinary build config has no key-layout line at all.
    CHECK_FALSE(parseStaticKeyLayout(asBytes("root = 00\nencoding = 00 00\n")).valid);

    // Several layouts selected by key content: guessing the rule would
    // mislocate every file.
    CHECK_FALSE(parseStaticKeyLayout(
                    asBytes("key-layout-index-bits = 2\nkey-layout-0 = 8 8 40 1\n"))
                    .valid);

    // Sub-byte fields, and a locator that leaves no room for a hash.
    CHECK_FALSE(parseStaticKeyLayout(asBytes("key-layout-0 = 4 8 40 1\n")).valid);
    CHECK_FALSE(parseStaticKeyLayout(asBytes("key-layout-0 = 8 8 112 1\n")).valid);
}

TEST_CASE("static-layout storage reads back what its content keys promise",
          "[casc][static][.corpus]") {
    std::string const path = findStaticInstall();
    if (path.empty()) {
        WARN("No static-build-config install found — skipping.");
        return;
    }
    std::cout << "Static-layout install: " << path << "\n";

    std::string error;
    auto storage = Storage::open(path, &error);
    if (!storage)
        std::cout << "  open failed: " << error << std::endl;
    REQUIRE(storage.has_value());

    auto product = storage->product();
    REQUIRE(product.has_value());
    std::cout << "  Product: " << product->name << " " << product->version << "\n";
    CHECK_FALSE(product->version.empty());

    struct Entry {
        std::array<u8, 16> cKey{};
        std::string path;
    };
    std::vector<Entry> sample;
    size_t seen = 0;
    storage->enumerate([&](const EnumerateEntry& fe) {
        if (seen++ % kStride == 0 && !fe.path.empty())
            sample.push_back({fe.cKey, std::string(fe.path)});
        return sample.size() < kSampleSize;
    });
    std::cout << "  Sampled " << sample.size() << " of " << seen << " listable entries.\n";
    REQUIRE(sample.size() > 10);

    size_t verified = 0;
    size_t unreadable = 0;
    for (const auto& e : sample) {
        auto data = storage->readFile(e.path);
        if (!data) {
            ++unreadable;
            continue;
        }
        // Whatever does come back has to be byte-for-byte what its content key
        // promises — that is what proves the locator decoded to the right blob.
        CHECK(storages::common::md5Hash(std::span<const u8>(*data)) == e.cKey);
        ++verified;
    }
    std::cout << "  Verified " << verified << ", unreadable " << unreadable << "\n";

    // Overwatch encrypts a small tail of its content with keys the client only
    // has at runtime; those never decode here and are not a layout failure.
    CHECK(verified * 20 > sample.size() * 19);
}
