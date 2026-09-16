// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file unicode.h
/// @brief Code point → UTF-8, and UTF-16 surrogate pairs → code point, for text
///        the library decodes itself (such as JSON `\u` escapes).
///
/// Internal header — not part of the public include path.
#pragma once

#include <string>

namespace whiteout::common::unicode {

/// Written in place of a code point that has no UTF-8 form.
inline constexpr char32_t kReplacementCharacter = 0xFFFD;
/// The last code point Unicode defines.
inline constexpr char32_t kMaxCodePoint = 0x10FFFF;
/// The first code point outside the Basic Multilingual Plane.
inline constexpr char32_t kFirstSupplementary = 0x10000;

// UTF-16 spells a supplementary code point as a high surrogate unit followed by
// a low one, each carrying 10 bits of it. The range they are drawn from holds
// no characters of its own.
inline constexpr char16_t kHighSurrogateFirst = 0xD800;
inline constexpr char16_t kHighSurrogateLast = 0xDBFF;
inline constexpr char16_t kLowSurrogateFirst = 0xDC00;
inline constexpr char16_t kLowSurrogateLast = 0xDFFF;
inline constexpr unsigned kSurrogatePayloadBits = 10;

constexpr bool isHighSurrogate(char16_t unit) {
    return unit >= kHighSurrogateFirst && unit <= kHighSurrogateLast;
}

constexpr bool isLowSurrogate(char16_t unit) {
    return unit >= kLowSurrogateFirst && unit <= kLowSurrogateLast;
}

/// True for a code point inside the surrogate range, which is not a character.
constexpr bool isSurrogate(char32_t codePoint) {
    return codePoint >= static_cast<char32_t>(kHighSurrogateFirst) &&
           codePoint <= static_cast<char32_t>(kLowSurrogateLast);
}

/// The code point a high surrogate followed by a low one encodes.
constexpr char32_t combineSurrogates(char16_t high, char16_t low) {
    return kFirstSupplementary +
           (static_cast<char32_t>(high - kHighSurrogateFirst) << kSurrogatePayloadBits) +
           static_cast<char32_t>(low - kLowSurrogateFirst);
}

/// Append @p codePoint to @p out as UTF-8. A surrogate, or a value past
/// kMaxCodePoint, has no UTF-8 form and is written as kReplacementCharacter.
inline void appendUtf8(std::string& out, char32_t codePoint) {
    // A sequence is a lead byte followed by up to three continuation bytes of
    // the form 10xxxxxx, each carrying the next 6 bits, low bits last. The lead
    // byte's tag says how many continuations follow and takes the rest.
    struct Length {
        unsigned payloadBits;  // the x's: how large a code point fits
        unsigned char leadTag; // the fixed high bits of the lead byte
    };
    static constexpr Length kLengths[] = {
        {7, 0b0000'0000},  // 0xxxxxxx
        {11, 0b1100'0000}, // 110xxxxx 10xxxxxx
        {16, 0b1110'0000}, // 1110xxxx 10xxxxxx 10xxxxxx
        {21, 0b1111'0000}, // 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    };
    constexpr unsigned kContinuationPayloadBits = 6;
    constexpr unsigned char kContinuationTag = 0b1000'0000;
    constexpr char32_t kContinuationPayloadMask = 0b0011'1111;

    if (isSurrogate(codePoint) || codePoint > kMaxCodePoint)
        codePoint = kReplacementCharacter;

    // The shortest length with room for every bit.
    unsigned continuations = 0;
    while (codePoint >> kLengths[continuations].payloadBits)
        ++continuations;

    out += static_cast<char>(kLengths[continuations].leadTag |
                             (codePoint >> (continuations * kContinuationPayloadBits)));
    while (continuations-- > 0) {
        char32_t const payload = codePoint >> (continuations * kContinuationPayloadBits);
        out += static_cast<char>(kContinuationTag | (payload & kContinuationPayloadMask));
    }
}

} // namespace whiteout::common::unicode
