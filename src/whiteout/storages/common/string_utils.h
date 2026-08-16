// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file string_utils.h
/// @brief Shared string utilities for CASC (and other storage) modules.

#pragma once

#include <whiteout/common_types.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace whiteout::storages::common {

/// Return a lowercased copy of @p s (ASCII-only, locale-independent).
inline std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return r;
}

/// Normalize a CASC path for lookup: lowercase, '/' → '\\', strip leading/trailing separators.
inline std::string normalizeCascPath(const std::string& s) {
    std::string r = s;
    for (auto& c : r) {
        if (c == '/')
            c = '\\';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    // Strip leading separators without O(n²) erase(begin).
    size_t start = 0;
    while (start < r.size() && r[start] == '\\')
        ++start;
    // Strip trailing separators.
    size_t end = r.size();
    while (end > start && r[end - 1] == '\\')
        --end;
    if (start == 0 && end == r.size())
        return r;
    return r.substr(start, end - start);
}

// ---------------------------------------------------------------------------
// Allocation-free counterparts
//
// Root index builds run these over millions of entries, where materializing a
// normalized std::string per entry dominates the cost.
// ---------------------------------------------------------------------------

/// Apply normalizeCascPath's per-character rule to a single character.
inline char normalizeCascChar(char c) {
    if (c == '/')
        return '\\';
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// The span of @p s that survives normalizeCascPath's leading/trailing trim.
inline std::string_view trimCascSeparators(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == '\\' || s[start] == '/'))
        ++start;
    size_t end = s.size();
    while (end > start && (s[end - 1] == '\\' || s[end - 1] == '/'))
        --end;
    return s.substr(start, end - start);
}

/// FNV-1a 64 with a MurmurHash3 finalizer, so low bits are well distributed for
/// power-of-2 masking. Never returns 0 — FlatHashMap reserves it as its empty
/// sentinel.
inline u64 cascPathHash64(std::string_view normalized) {
    u64 h = 14695981039346656037ULL;
    for (char c : normalized) {
        h ^= static_cast<u64>(static_cast<u8>(c));
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h == 0 ? 1 : h;
}

/// cascPathHash64 of @p raw's normalized form, without materializing it.
inline u64 normalizedCascPathHash64(std::string_view raw) {
    std::string_view const body = trimCascSeparators(raw);
    u64 h = 14695981039346656037ULL;
    for (char c : body) {
        h ^= static_cast<u64>(static_cast<u8>(normalizeCascChar(c)));
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h == 0 ? 1 : h;
}

/// True if @p raw normalizes to @p normalizedKey, without materializing it.
inline bool normalizedCascPathEquals(std::string_view raw, std::string_view normalizedKey) {
    std::string_view const body = trimCascSeparators(raw);
    if (body.size() != normalizedKey.size())
        return false;
    for (size_t i = 0; i < body.size(); ++i) {
        if (normalizeCascChar(body[i]) != normalizedKey[i])
            return false;
    }
    return true;
}

} // namespace whiteout::storages::common
