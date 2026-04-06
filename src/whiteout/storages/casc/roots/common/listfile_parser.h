// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file listfile_parser.h
/// @brief Shared community-listfile parser for WoW roots.
///
/// Parses files in the standard community-listfile format:
///   FileDataId;path   (semicolon- or comma-separated, one per line)
///   Lines starting with '#' are comments. UTF-8 BOM is skipped.
#pragma once

#include <whiteout/common_types.h>

#include <charconv>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace whiteout::storages::casc {

/// Parse a community-listfile into a FileDataId → path map.
inline std::unordered_map<u32, std::string> parseListfile(std::span<const u8> data) {
    std::unordered_map<u32, std::string> result;
    if (data.empty()) return result;

    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());

    // Skip UTF-8 BOM if present.
    if (text.size() >= 3 && text[0] == '\xEF' && text[1] == '\xBB' && text[2] == '\xBF')
        text.remove_prefix(3);

    size_t pos = 0;
    while (pos < text.size()) {
        // Find end of line.
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();

        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;

        // Strip trailing \r.
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        // Skip empty lines and comment/header lines.
        if (line.empty() || line[0] == '#')
            continue;

        // Format: FileDataId;path  or  FileDataId,path
        size_t sep = line.find(';');
        if (sep == std::string_view::npos)
            sep = line.find(',');
        if (sep == std::string_view::npos || sep == 0 || sep + 1 >= line.size())
            continue;

        std::string_view idStr = line.substr(0, sep);
        std::string_view pathStr = line.substr(sep + 1);

        u32 fileDataId = 0;
        auto [ptr, ec] = std::from_chars(idStr.data(), idStr.data() + idStr.size(), fileDataId);
        if (ec != std::errc{} || ptr != idStr.data() + idStr.size())
            continue;

        // Store the path as-is (normalization happens at index build time).
        result.emplace(fileDataId, std::string(pathStr));
    }

    return result;
}

} // namespace whiteout::storages::casc
