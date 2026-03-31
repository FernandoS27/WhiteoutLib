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

} // namespace whiteout::storages::common
