// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file string_utils.h
/// @brief Shared string utilities for CASC (and other storage) modules.

#pragma once

#include <algorithm>
#include <cctype>
#include <string>

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
        if (c == '/') c = '\\';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    // Strip leading separators without O(n²) erase(begin).
    size_t start = 0;
    while (start < r.size() && r[start] == '\\') ++start;
    // Strip trailing separators.
    size_t end = r.size();
    while (end > start && r[end - 1] == '\\') --end;
    if (start == 0 && end == r.size()) return r;
    return r.substr(start, end - start);
}

} // namespace whiteout::storages::common
