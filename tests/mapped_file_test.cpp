// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// mapped_file_test: Validates MappedFile's path(), error output, and AccessHint.

#include "../src/whiteout/storages/common/mapped_file.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace whiteout::storages::common;

static int g_passed = 0;
static int g_failed = 0;

static void check(bool condition, const char* name) {
    if (condition) {
        std::cout << "  PASS: " << name << "\n";
        ++g_passed;
    } else {
        std::cout << "  FAIL: " << name << "\n";
        ++g_failed;
    }
}

/// Test 1: open() a real file → path() returns the path passed in.
static void testOpenRealFile() {
    std::cout << "[Test: Open real file]\n";
    // Use CMakeLists.txt as a known file that always exists.
    auto mf = MappedFile::open("CMakeLists.txt");
    check(mf.has_value(), "open() succeeds for existing file");
    if (mf) {
        check(mf->path() == "CMakeLists.txt", "path() returns the path passed in");
        check(mf->size() > 0, "size() is non-zero");
        check(mf->ptr() != nullptr, "ptr() is non-null");
        check(static_cast<bool>(*mf), "operator bool() returns true");
    }
}

/// Test 2: open() with a nonexistent path → returns nullopt, error is non-empty.
static void testOpenNonexistent() {
    std::cout << "[Test: Open nonexistent file]\n";
    std::string error;
    auto mf = MappedFile::open("this_file_does_not_exist_at_all.xyz",
                               AccessHint::Normal, &error);
    check(!mf.has_value(), "open() returns nullopt for nonexistent file");
    check(!error.empty(), "error string is non-empty");
    std::cout << "  (error message: " << error << ")\n";
}

/// Test 3: open() with AccessHint::Sequential and Random → succeeds.
static void testAccessHints() {
    std::cout << "[Test: AccessHint variants]\n";
    auto mf1 = MappedFile::open("CMakeLists.txt", AccessHint::Sequential);
    check(mf1.has_value(), "open() with Sequential hint succeeds");

    auto mf2 = MappedFile::open("CMakeLists.txt", AccessHint::Random);
    check(mf2.has_value(), "open() with Random hint succeeds");

    // Test advise() on an existing mapping — should not crash.
    if (mf1) {
        mf1->advise(AccessHint::Random);
        check(true, "advise(Random) does not crash");
    }
}

/// Test 4: Default-constructed MappedFile → path() returns empty string.
static void testDefaultConstructed() {
    std::cout << "[Test: Default-constructed MappedFile]\n";
    MappedFile mf;
    check(mf.path().empty(), "path() returns empty string");
    check(mf.size() == 0, "size() is 0");
    check(mf.ptr() == nullptr, "ptr() is nullptr");
    check(!static_cast<bool>(mf), "operator bool() returns false");
}

/// Test 5: Move semantics preserve path.
static void testMoveSemantics() {
    std::cout << "[Test: Move semantics]\n";
    auto mf = MappedFile::open("CMakeLists.txt");
    check(mf.has_value(), "open() succeeds");
    if (mf) {
        MappedFile moved(std::move(*mf));
        check(moved.path() == "CMakeLists.txt", "moved-to has correct path");
        check(moved.size() > 0, "moved-to has size > 0");
        check(mf->path().empty(), "moved-from has empty path");
        check(mf->ptr() == nullptr, "moved-from has null ptr");
    }
}

int main() {
    std::cout << "=== MappedFile Tests ===\n\n";

    testOpenRealFile();
    testOpenNonexistent();
    testAccessHints();
    testDefaultConstructed();
    testMoveSemantics();

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
    return g_failed > 0 ? 1 : 0;
}
