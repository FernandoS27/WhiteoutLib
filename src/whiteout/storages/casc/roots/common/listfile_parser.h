// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file listfile_parser.h
/// @brief Shared community-listfile parser for WoW roots.
///
/// Parses files in the standard community-listfile format:
///   FileDataId;path   (semicolon- or comma-separated, one per line)
///   Lines starting with '#' are comments. UTF-8 BOM is skipped.
#pragma once

#include "../../tables/flat_hash_map.h"

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <charconv>
#include <span>
#include <string_view>
#include <vector>

namespace whiteout::storages::casc {

namespace detail {

/// One accepted listfile line: its id and where its path sits in the input.
struct ListfileSlot {
    u32 fileDataId;
    u32 offset;
    u32 length;
};

/// Parse a run of whole lines, appending one slot per accepted line. The view
/// must start on a line boundary (the BOM, if any, is the caller's
/// responsibility to strip) and @p base must point at the whole input.
inline void parseListfileRange(const char* base, std::string_view text,
                               std::vector<ListfileSlot>& out) {
    // Lines average ~68 bytes; a generous guess up front beats regrowing a
    // multi-million-element vector inside a worker.
    out.reserve(out.size() + text.size() / 48 + 16);

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos)
            eol = text.size();

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

        // Stored as an offset, not a copy: paths keep their input spelling
        // (normalization happens at index build time) and the input outlives us.
        out.push_back(ListfileSlot{fileDataId, static_cast<u32>(pathStr.data() - base),
                                   static_cast<u32>(pathStr.size())});
    }
}

/// Strip a leading UTF-8 BOM, if present.
inline std::string_view stripBom(std::string_view text) {
    if (text.size() >= 3 && text[0] == '\xEF' && text[1] == '\xBB' && text[2] == '\xBF')
        text.remove_prefix(3);
    return text;
}

} // namespace detail

/// FileDataId to path index over a community listfile.
///
/// Every path stays where it already is — a view into the caller's buffer —
/// and the lookup table is indexed by FileDataId directly. The
/// `unordered_map<u32, std::string>` this replaces spent two allocations per
/// line, 4.3M of them for a 2.1M-line community listfile, which held the
/// parallel parse to ~3.5x however many threads it was given.
///
/// **The buffer passed to parseListfile must outlive the index.**
class ListfileIndex {
public:
    /// Path for @p fileDataId, or an empty view if the listfile has no such id.
    /// A line with an empty path is rejected at parse time, so a hit is never
    /// empty — which is also what makes a zero length usable as the empty slot.
    std::string_view find(u32 fileDataId) const {
        if (fileDataId < m_direct.size()) {
            const PathRef& ref = m_direct[fileDataId];
            return ref.length ? std::string_view(m_base + ref.offset, ref.length)
                              : std::string_view{};
        }
        if (m_sparse.size() == 0)
            return {};
        const PathRef* ref = m_sparse.find(mixId(fileDataId));
        return ref ? std::string_view(m_base + ref->offset, ref->length) : std::string_view{};
    }

    size_t size() const {
        return m_size;
    }

    bool empty() const {
        return m_size == 0;
    }

    /// Populate from per-chunk slot lists. Chunk order decides which of two
    /// lines sharing an id wins, so the parts must be in file order.
    void build(const char* base, std::span<const std::vector<detail::ListfileSlot>> parts) {
        m_base = base;
        size_t total = 0;
        u32 maxId = 0;
        for (auto& part : parts) {
            total += part.size();
            for (auto& slot : part)
                maxId = std::max(maxId, slot.fileDataId);
        }

        // FileDataIds are dense, so a plain array beats hashing them: one load
        // per lookup, no probing, and the community listfile's ids run to only
        // ~3x its line count. The bound keeps a listfile with a few absurd ids
        // from asking for gigabytes; those spill to the map below.
        size_t const directCap = total * 4 + 1024;
        size_t const directSize = std::min<size_t>(size_t(maxId) + 1, directCap);
        m_direct.assign(directSize, PathRef{});

        for (auto& part : parts) {
            for (auto& slot : part) {
                PathRef const ref{slot.offset, slot.length};
                if (slot.fileDataId < directSize) {
                    PathRef& dst = m_direct[slot.fileDataId];
                    if (dst.length)
                        continue; // first line with this id wins
                    dst = ref;
                } else {
                    u64 const key = mixId(slot.fileDataId);
                    if (m_sparse.find(key))
                        continue;
                    m_sparse.emplace(key, ref);
                }
                ++m_size;
            }
        }
    }

private:
    struct PathRef {
        u32 offset = 0;
        u32 length = 0; ///< 0 marks an unused slot.
    };

    /// FlatHashMap masks the key rather than hashing it, which a run of
    /// consecutive ids would turn into one long probe chain.
    static u64 mixId(u32 fileDataId) {
        u64 h = u64(fileDataId) + 0x9E3779B97F4A7C15ull;
        h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
        h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
        return h ^ (h >> 31);
    }

    const char* m_base = nullptr;
    std::vector<PathRef> m_direct;
    FlatHashMap<PathRef> m_sparse; ///< ids beyond the direct table's bound.
    size_t m_size = 0;
};

/// Parse a community-listfile into a FileDataId to path index.
/// @p data must stay alive for as long as the returned index is used.
inline ListfileIndex parseListfile(std::span<const u8> data) {
    ListfileIndex result;
    if (data.empty())
        return result;

    const char* base = reinterpret_cast<const char*>(data.data());
    auto text = detail::stripBom(std::string_view(base, data.size()));

    std::vector<std::vector<detail::ListfileSlot>> parts(1);
    detail::parseListfileRange(base, text, parts[0]);
    result.build(base, parts);
    return result;
}

/// Parallel community-listfile parser. The input is split into line-aligned
/// chunks and scanned concurrently on @p pool; the index build that follows is
/// serial. Falls back to the single-threaded parser for small inputs or when no
/// pool is given.
/// @p data must stay alive for as long as the returned index is used.
inline ListfileIndex parseListfile(std::span<const u8> data, interfaces::WorkerPool* pool) {

    constexpr size_t kMinParallelBytes = 8 * 1024 * 1024; // 8 MB
    if (!pool || data.size() < kMinParallelBytes)
        return parseListfile(data);

    const char* base = reinterpret_cast<const char*>(data.data());
    auto text = detail::stripBom(std::string_view(base, data.size()));

    // Split into line-aligned chunks: each chunk ends just past a newline.
    const size_t nChunks = std::min<size_t>(std::max<size_t>(pool->threadCount(), 1), 64);
    std::vector<std::string_view> chunks;
    chunks.reserve(nChunks);
    {
        const size_t approx = text.size() / nChunks;
        size_t start = 0;
        for (size_t c = 0; c < nChunks && start < text.size(); ++c) {
            size_t end = (c + 1 == nChunks) ? text.size() : std::min(text.size(), start + approx);
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

    std::vector<std::vector<detail::ListfileSlot>> parts(chunks.size());
    {
        utils::JobGroup jobGroup;
        jobGroup.add(chunks.size());
        for (size_t i = 0; i < chunks.size(); ++i) {
            interfaces::WorkerTask task;
            task.fn = [&, i]() {
                detail::parseListfileRange(base, chunks[i], parts[i]);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    }

    ListfileIndex result;
    result.build(base, parts);
    return result;
}

} // namespace whiteout::storages::casc
