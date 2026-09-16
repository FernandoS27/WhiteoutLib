// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file unicode_test.cpp
/// @brief Code point → UTF-8 and UTF-16 surrogate pairs (common/unicode.h).

#include "../src/whiteout/common/unicode.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

using namespace whiteout::common::unicode;

namespace {

std::string utf8(char32_t codePoint) {
    std::string out;
    appendUtf8(out, codePoint);
    return out;
}

std::string bytes(std::initializer_list<unsigned char> values) {
    return std::string(values.begin(), values.end());
}

std::string asString(std::u8string_view s) {
    return std::string(s.begin(), s.end());
}

/// A UTF-16 surrogate unit read as a code point, which is exactly what makes it
/// invalid as one.
constexpr char32_t asCodePoint(char16_t unit) {
    return static_cast<char32_t>(unit);
}

/// A code point as the standard library converts it — code that shares
/// nothing with the functions under test.
std::filesystem::path reference(char32_t codePoint) {
    return std::filesystem::path(std::u32string(1, codePoint));
}

} // namespace

TEST_CASE("UTF-8 grows a byte at each sequence-length boundary", "[unicode]") {
    // The last code point each length holds, and the first of the next.
    CHECK(utf8(0x00) == bytes({0x00}));
    CHECK(utf8(0x7F) == bytes({0x7F}));
    CHECK(utf8(0x80) == bytes({0xC2, 0x80}));
    CHECK(utf8(0x7FF) == bytes({0xDF, 0xBF}));
    CHECK(utf8(0x800) == bytes({0xE0, 0xA0, 0x80}));
    CHECK(utf8(0xFFFF) == bytes({0xEF, 0xBF, 0xBF}));
    CHECK(utf8(kFirstSupplementary) == bytes({0xF0, 0x90, 0x80, 0x80}));
    CHECK(utf8(kMaxCodePoint) == bytes({0xF4, 0x8F, 0xBF, 0xBF}));

    // Either side of the surrogate block, which is not encodable itself.
    CHECK(utf8(kHighSurrogateFirst - 1) == bytes({0xED, 0x9F, 0xBF}));
    CHECK(utf8(kLowSurrogateLast + 1) == bytes({0xEE, 0x80, 0x80}));
}

TEST_CASE("UTF-8 matches what the compiler writes for a u8 literal", "[unicode]") {
    CHECK(utf8(U'A') == asString(u8"A"));
    CHECK(utf8(U'\u00E9') == asString(u8"\u00E9"));         // é
    CHECK(utf8(U'\u5149') == asString(u8"\u5149"));         // 光
    CHECK(utf8(U'\U0001F600') == asString(u8"\U0001F600")); // 😀
}

TEST_CASE("UTF-8 matches the standard library for every code point", "[unicode]") {
    // NUL is left out only because a path cannot hold one.
    size_t checked = 0;
    char32_t firstMismatch = 0;
    for (char32_t codePoint = 1; codePoint <= kMaxCodePoint; ++codePoint) {
        if (isSurrogate(codePoint))
            continue;
        ++checked;
        if (!firstMismatch && utf8(codePoint) != asString(reference(codePoint).u8string()))
            firstMismatch = codePoint;
    }
    INFO("first mismatch: U+" << std::hex << static_cast<unsigned long>(firstMismatch));
    CHECK(firstMismatch == 0);
    // Every Unicode scalar value but NUL.
    CHECK(checked == kMaxCodePoint + 1 - (kLowSurrogateLast - kHighSurrogateFirst + 1) - 1);
}

TEST_CASE("A code point with no UTF-8 form is written as U+FFFD", "[unicode]") {
    std::string const replacement = bytes({0xEF, 0xBF, 0xBD});
    REQUIRE(utf8(kReplacementCharacter) == replacement);

    for (char32_t const invalid :
         {asCodePoint(kHighSurrogateFirst), asCodePoint(kHighSurrogateLast),
          asCodePoint(kLowSurrogateFirst), asCodePoint(kLowSurrogateLast),
          char32_t{kMaxCodePoint + 1}, char32_t{0xFFFFFFFF}}) {
        INFO("U+" << std::hex << static_cast<unsigned long>(invalid));
        CHECK(utf8(invalid) == replacement);
    }
}

TEST_CASE("UTF-8 is appended after what the string already holds", "[unicode]") {
    std::string path = "D:/";
    appendUtf8(path, U'\u5149');
    appendUtf8(path, U'/');
    CHECK(path == "D:/\xE5\x85\x89/");
}

TEST_CASE("Surrogates are told apart by range", "[unicode]") {
    STATIC_REQUIRE_FALSE(isHighSurrogate(kHighSurrogateFirst - 1));
    STATIC_REQUIRE(isHighSurrogate(kHighSurrogateFirst));
    STATIC_REQUIRE(isHighSurrogate(kHighSurrogateLast));
    STATIC_REQUIRE_FALSE(isHighSurrogate(kLowSurrogateFirst));

    STATIC_REQUIRE_FALSE(isLowSurrogate(kHighSurrogateLast));
    STATIC_REQUIRE(isLowSurrogate(kLowSurrogateFirst));
    STATIC_REQUIRE(isLowSurrogate(kLowSurrogateLast));
    STATIC_REQUIRE_FALSE(isLowSurrogate(kLowSurrogateLast + 1));

    STATIC_REQUIRE(isSurrogate(asCodePoint(kHighSurrogateFirst)));
    STATIC_REQUIRE(isSurrogate(asCodePoint(kLowSurrogateLast)));
    STATIC_REQUIRE_FALSE(isSurrogate(kHighSurrogateFirst - 1));
    STATIC_REQUIRE_FALSE(isSurrogate(kLowSurrogateLast + 1));
}

TEST_CASE("A surrogate pair combines into the code point it was split from", "[unicode]") {
    // The first and last pairs span the whole supplementary range.
    STATIC_REQUIRE(combineSurrogates(kHighSurrogateFirst, kLowSurrogateFirst) ==
                   kFirstSupplementary);
    STATIC_REQUIRE(combineSurrogates(kHighSurrogateLast, kLowSurrogateLast) == kMaxCodePoint);

    std::u16string_view const emoji = u"\U0001F600";
    REQUIRE(emoji.size() == 2);
    CHECK(combineSurrogates(emoji[0], emoji[1]) == U'\U0001F600');

    // Every supplementary code point, split by the standard library.
    char32_t firstMismatch = 0;
    for (char32_t codePoint = kFirstSupplementary; codePoint <= kMaxCodePoint; ++codePoint) {
        std::u16string const pair = reference(codePoint).u16string();
        bool const ok = pair.size() == 2 && isHighSurrogate(pair[0]) && isLowSurrogate(pair[1]) &&
                        combineSurrogates(pair[0], pair[1]) == codePoint;
        if (!ok) {
            firstMismatch = codePoint;
            break;
        }
    }
    INFO("first mismatch: U+" << std::hex << static_cast<unsigned long>(firstMismatch));
    CHECK(firstMismatch == 0);
}
