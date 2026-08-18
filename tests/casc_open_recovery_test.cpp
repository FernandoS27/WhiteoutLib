// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file casc_open_recovery_test.cpp
/// @brief Opening a local storage whose `.build.info` is missing or unusable.

#include <whiteout/storages/casc/storage.h>
#include <whiteout/storages/casc/storage_writable.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

namespace {

std::vector<u8> makeTestData(size_t size, u8 seed) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>((i + seed) & 0xFF);
    return data;
}

/// Write a small two-file storage to @p dir, `.build.info` and all.
bool buildStorage(const std::string& dir, const std::string& product = "test") {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    CreateOptions createOpts;
    createOpts.product = product;
    createOpts.version = "1.0.0";
    auto storage = StorageWritable::create(createOpts);
    if (!storage)
        return false;
    if (!storage.writeFile("dir/file1.txt", makeTestData(2048, 0x11)))
        return false;
    if (!storage.writeFile("dir/file2.bin", makeTestData(70000, 0x22)))
        return false;
    return storage.save(dir);
}

} // namespace

TEST_CASE("Open with .build.info missing", "[casc][open]") {
    const std::string testDir = "test_casc_open_recovery_missing";
    REQUIRE(buildStorage(testDir));
    REQUIRE(std::filesystem::exists(testDir + "/.build.info"));
    REQUIRE(Storage::open(testDir).has_value());

    std::filesystem::remove(testDir + "/.build.info");

    std::string error;
    auto storage = Storage::open(testDir, &error);
    INFO(error);
    REQUIRE(storage.has_value());

    // The build config supplies the product metadata, so nothing is lost.
    auto product = storage->product();
    REQUIRE(product.has_value());
    CHECK(product->name == "test");
    CHECK(product->version == "1.0.0");

    auto data1 = storage->readFile("dir/file1.txt");
    REQUIRE(data1.has_value());
    CHECK(*data1 == makeTestData(2048, 0x11));

    auto data2 = storage->readFile("dir/file2.bin");
    REQUIRE(data2.has_value());
    CHECK(*data2 == makeTestData(70000, 0x22));

    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
}

TEST_CASE("Open with unusable .build.info", "[casc][open]") {
    const std::string testDir = "test_casc_open_recovery_garbage";
    REQUIRE(buildStorage(testDir));

    SECTION("contents do not parse as a build-info table") {
        std::ofstream(testDir + "/.build.info", std::ios::binary | std::ios::trunc)
            << "not a build info file";
        CHECK(Storage::open(testDir).has_value());
    }

    SECTION("build/CDN keys name configs that are not on disk") {
        std::ofstream(testDir + "/.build.info", std::ios::binary | std::ios::trunc)
            << "Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|CDN Key!HEX:16|Version!STRING:0|"
               "Product!STRING:0\n"
               "master|1|00000000000000000000000000000000|00000000000000000000000000000000|"
               "1.0.0|test\n";
        CHECK(Storage::open(testDir).has_value());
    }

    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
}

TEST_CASE("Product selection without .build.info", "[casc][open]") {
    const std::string testDir = "test_casc_open_recovery_product";
    REQUIRE(buildStorage(testDir, "wow"));
    std::filesystem::remove(testDir + "/.build.info");

    // The recovered build config answers for the product instead of the
    // (absent) Product column — a mismatch must not silently open anyway.
    CHECK(Storage::open(testDir, std::string("wow")).has_value());
    CHECK_FALSE(Storage::open(testDir, std::string("d3")).has_value());

    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
}

TEST_CASE("Open finds an Overwatch-style data/casc storage", "[casc][open]") {
    const std::string testDir = "test_casc_open_layout_ow";
    REQUIRE(buildStorage(testDir, "pro"));

    // Overwatch nests the storage one level deeper than every other product.
    // The intervening "data" directory holds nothing else, so picking a data
    // directory by name alone lands on it and finds no index beneath.
    // Staged through a third name because "Data" and "data" are the same
    // directory on a case-insensitive filesystem.
    std::error_code ec;
    std::filesystem::rename(testDir + "/Data", testDir + "/casc", ec);
    REQUIRE_FALSE(ec);
    std::filesystem::create_directories(testDir + "/data", ec);
    std::filesystem::rename(testDir + "/casc", testDir + "/data/casc", ec);
    REQUIRE_FALSE(ec);
    REQUIRE(std::filesystem::exists(testDir + "/data/casc/config"));

    std::string error;
    auto storage = Storage::open(testDir, &error);
    INFO(error);
    REQUIRE(storage.has_value());
    CHECK(storage->readFile("dir/file1.txt") == makeTestData(2048, 0x11));

    // The data directory names itself just as well as the install root does.
    CHECK(Storage::open(testDir + "/data/casc").has_value());

    std::filesystem::remove_all(testDir, ec);
}

TEST_CASE("Open fails without .build.info and without configs", "[casc][open]") {
    const std::string testDir = "test_casc_open_recovery_nothing";
    REQUIRE(buildStorage(testDir));
    std::filesystem::remove(testDir + "/.build.info");

    std::error_code ec;
    std::filesystem::remove_all(testDir + "/Data/config", ec);

    std::string error;
    CHECK_FALSE(Storage::open(testDir, &error).has_value());
    CHECK_FALSE(error.empty());

    std::filesystem::remove_all(testDir, ec);
}
