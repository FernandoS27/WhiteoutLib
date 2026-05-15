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
#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <charconv>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whiteout::storages::casc {

namespace detail {

/// Parse a run of whole lines into @p result. The view must start on a line
/// boundary (the BOM, if any, is the caller's responsibility to strip).
inline void parseListfileRange(std::string_view text,
                               std::unordered_map<u32, std::string>& result) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();

        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;

        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

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
}

/// Strip a leading UTF-8 BOM, if present.
inline std::string_view stripBom(std::string_view text) {
    if (text.size() >= 3 && text[0] == '\xEF' && text[1] == '\xBB' && text[2] == '\xBF')
        text.remove_prefix(3);
    return text;
}

} // namespace detail

/// Parse a community-listfile into a FileDataId → path map.
inline std::unordered_map<u32, std::string> parseListfile(std::span<const u8> data) {
    std::unordered_map<u32, std::string> result;
    if (data.empty()) return result;

    auto text = detail::stripBom(
        std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    detail::parseListfileRange(text, result);
    return result;
}

/// Parallel community-listfile parser. The input is split into line-aligned
/// chunks, parsed concurrently on @p pool, and the per-chunk maps are merged.
/// Falls back to the single-threaded parser for small inputs or when no pool
/// is given. Community listfiles are ~140 MB / millions of lines, so the
/// scan + per-path string allocation dominates and parallelizes well.
inline std::unordered_map<u32, std::string> parseListfile(
    std::span<const u8> data, interfaces::WorkerPool* pool) {

    constexpr size_t kMinParallelBytes = 8 * 1024 * 1024; // 8 MB
    if (!pool || data.size() < kMinParallelBytes)
        return parseListfile(data);

    auto text = detail::stripBom(
        std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));

    // Split into line-aligned chunks: each chunk ends just past a '\n'.
    const size_t nChunks = std::min<size_t>(
        std::max<size_t>(pool->threadCount(), 1), 64);
    std::vector<std::string_view> chunks;
    chunks.reserve(nChunks);
    {
        const size_t approx = text.size() / nChunks;
        size_t start = 0;
        for (size_t c = 0; c < nChunks && start < text.size(); ++c) {
            size_t end = (c + 1 == nChunks)
                ? text.size()
                : std::min(text.size(), start + approx);
            if (end < text.size()) {
                size_t nl = text.find('\n', end);
                end = (nl == std::string_view::npos) ? text.size() : nl + 1;
            }
            chunks.push_back(text.substr(start, end - start));
            start = end;
        }
    }

    if (chunks.size() <= 1)
        return parseListfile(data);

    // Parse each chunk into its own map in parallel.
    std::vector<std::unordered_map<u32, std::string>> partials(chunks.size());
    {
        utils::JobGroup jobGroup;
        jobGroup.add(chunks.size());
        for (size_t i = 0; i < chunks.size(); ++i) {
            interfaces::WorkerTask task;
            task.fn = [&, i]() {
                detail::parseListfileRange(chunks[i], partials[i]);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    }

    // Merge into the largest partial — node splicing, no string reallocation.
    size_t total = 0, largest = 0;
    for (size_t i = 0; i < partials.size(); ++i) {
        total += partials[i].size();
        if (partials[i].size() > partials[largest].size()) largest = i;
    }
    auto result = std::move(partials[largest]);
    result.reserve(total);
    for (size_t i = 0; i < partials.size(); ++i) {
        if (i != largest)
            result.merge(partials[i]);
    }
    return result;
}

} // namespace whiteout::storages::casc
