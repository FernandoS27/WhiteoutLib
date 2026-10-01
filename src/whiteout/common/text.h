// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file text.h
 * @brief Locale-free primitives for the line-based text formats (OBJ, MTL,
 *        ASCII FBX). Internal header — not part of the public include path.
 *
 * A host that calls `setlocale(LC_NUMERIC, …)` turns `%g` and `strtod` into
 * decimal-comma readers and writers; nothing here depends on that.
 */

#include <string>
#include <string_view>

#include <whiteout/common_types.h>

namespace whiteout {
namespace text {

inline bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && IsSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

/// Pops the next whitespace-separated token off @p rest; empty at the end.
inline std::string_view NextToken(std::string_view& rest) {
    std::size_t i = 0;
    while (i < rest.size() && IsSpace(rest[i])) {
        ++i;
    }
    std::size_t j = i;
    while (j < rest.size() && !IsSpace(rest[j])) {
        ++j;
    }
    const std::string_view token = rest.substr(i, j - i);
    rest.remove_prefix(j);
    return token;
}

/// Calls @p fn once per line, without its terminator (`\n`, `\r\n` or a lone `\r`).
template <class Fn>
void ForEachLine(std::string_view text, Fn&& fn) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' || text[i] == '\r') {
            fn(text.substr(start, i - start));
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            start = i + 1;
        }
    }
    if (start < text.size()) {
        fn(text.substr(start));
    }
}

/// The whole token as a number; a leading `+` is accepted.
bool ParseF64(std::string_view token, f64& out);
bool ParseF32(std::string_view token, f32& out);
bool ParseI64(std::string_view token, i64& out);

/// @p value in nine significant digits — enough for any f32 to read back
/// bit-identically — always with a `.` radix. Non-finite values write `0`.
void AppendF32(std::string& out, f32 value);
/// Seventeen significant digits, for f64.
void AppendF64(std::string& out, f64 value);

} // namespace text
} // namespace whiteout
