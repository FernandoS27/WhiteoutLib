// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// mapped_file_test: Validates MappedFile's path(), error output, and AccessHint.

#include "../src/whiteout/storages/common/mapped_file.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace whiteout::storages::common;

TEST_CASE("MappedFile open real file", "[mapped_file]") {
    auto mf = MappedFile::open("CMakeLists.txt");
    REQUIRE(mf.has_value());
    CHECK(mf->path() == "CMakeLists.txt");
    CHECK(mf->size() > 0);
    CHECK(mf->ptr() != nullptr);
    CHECK(static_cast<bool>(*mf));
}

TEST_CASE("MappedFile open nonexistent", "[mapped_file]") {
    std::string error;
    auto mf = MappedFile::open("this_file_does_not_exist_at_all.xyz",
                               AccessHint::Normal, &error);
    CHECK_FALSE(mf.has_value());
    CHECK_FALSE(error.empty());
}

TEST_CASE("MappedFile access hints", "[mapped_file]") {
    auto mf1 = MappedFile::open("CMakeLists.txt", AccessHint::Sequential);
    CHECK(mf1.has_value());

    auto mf2 = MappedFile::open("CMakeLists.txt", AccessHint::Random);
    CHECK(mf2.has_value());

    if (mf1) {
        mf1->advise(AccessHint::Random);
        SUCCEED("advise(Random) does not crash");
    }
}

TEST_CASE("MappedFile default constructed", "[mapped_file]") {
    MappedFile mf;
    CHECK(mf.path().empty());
    CHECK(mf.size() == 0);
    CHECK(mf.ptr() == nullptr);
    CHECK_FALSE(static_cast<bool>(mf));
}

TEST_CASE("MappedFile move semantics", "[mapped_file]") {
    auto mf = MappedFile::open("CMakeLists.txt");
    REQUIRE(mf.has_value());

    MappedFile moved(std::move(*mf));
    CHECK(moved.path() == "CMakeLists.txt");
    CHECK(moved.size() > 0);
    CHECK(mf->path().empty());
    CHECK(mf->ptr() == nullptr);
}
