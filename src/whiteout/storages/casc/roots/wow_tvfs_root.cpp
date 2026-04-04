// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "wow_tvfs_root.h"
#include "../../common/string_utils.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string_view>

namespace whiteout::storages::casc {

// ============================================================================
// Hex parsing helpers
// ============================================================================

namespace {

/// Parse a single hex digit (uppercase or lowercase).  Returns 0xFF on failure.
inline u8 hexDigit(char c) {
    if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<u8>(c - 'A' + 10);
    return 0xFF;
}

/// Parse @p len hex characters from @p s into a u32 (big-endian digit order).
/// Returns false if any character is not a valid hex digit.
inline bool parseHexU32(const char* s, size_t len, u32& out) {
    out = 0;
    for (size_t i = 0; i < len; ++i) {
        u8 d = hexDigit(s[i]);
        if (d == 0xFF) return false;
        out = (out << 4) | d;
    }
    return true;
}

/// Parse 32 hex characters into a 16-byte key.
/// Returns false if any character is not a valid hex digit.
inline bool parseHexKey(const char* s, std::array<u8, 16>& out) {
    for (int i = 0; i < 16; ++i) {
        u8 hi = hexDigit(s[i * 2]);
        u8 lo = hexDigit(s[i * 2 + 1]);
        if (hi == 0xFF || lo == 0xFF) return false;
        out[static_cast<size_t>(i)] = static_cast<u8>((hi << 4) | lo);
    }
    return true;
}

/// Expected path length: 8(locale) + 4(content) + 1('/') + 8(fdid) + 32(ckey) = 53.
static constexpr size_t kWowTvfsPathLen = 53;

/// Check whether a single path matches the WoW TVFS encoded format.
inline bool isWowTvfsPath(std::string_view path) {
    if (path.size() != kWowTvfsPathLen) return false;
    if (path[12] != '/') return false;
    // Quick validation: first 12 chars and positions 13-52 should be hex.
    for (size_t i = 0; i < 12; ++i)
        if (hexDigit(path[i]) == 0xFF) return false;
    for (size_t i = 13; i < 53; ++i)
        if (hexDigit(path[i]) == 0xFF) return false;
    return true;
}

/// Parse an encoded WoW TVFS path into its components.
/// Assumes isWowTvfsPath() already returned true.
struct WowTvfsPathInfo {
    u32 localeFlags;
    u32 contentFlags;
    u32 fileDataId;
    std::array<u8, 16> cKey;
};

inline bool parseWowTvfsPath(std::string_view path, WowTvfsPathInfo& info) {
    const char* s = path.data();
    if (!parseHexU32(s, 8, info.localeFlags)) return false;
    if (!parseHexU32(s + 8, 4, info.contentFlags)) return false;
    if (!parseHexU32(s + 13, 8, info.fileDataId)) return false;
    if (!parseHexKey(s + 21, info.cKey)) return false;
    return true;
}

} // anonymous namespace

// ============================================================================
// WowTvfsRoot — detection
// ============================================================================

bool WowTvfsRoot::looksLikeWowTvfs(const TvfsRoot& tvfs) {
    // Sample up to 16 entries.  If all of them match the 53-char hex pattern,
    // this is very likely a WoW TVFS root.
    size_t checked = 0;
    size_t matched = 0;
    tvfs.enumerate([&](const RootEntry& e) {
        if (checked >= 16) return false; // stop
        ++checked;
        if (isWowTvfsPath(e.path))
            ++matched;
        return true;
    });
    // Require at least 1 entry and all sampled entries must match.
    return checked > 0 && matched == checked;
}

// ============================================================================
// WowTvfsRoot — creation
// ============================================================================

std::unique_ptr<WowTvfsRoot> WowTvfsRoot::create(std::unique_ptr<TvfsRoot> tvfs,
                                                   interfaces::WorkerPool* pool,
                                                   std::span<const u8> listfile) {
    if (!tvfs) return nullptr;
    if (!looksLikeWowTvfs(*tvfs)) return nullptr;

    // Parse listfile if provided.
    std::unordered_map<u32, std::string> listfilePaths;
    if (!listfile.empty())
        listfilePaths = parseListfile(listfile);

    auto result = std::unique_ptr<WowTvfsRoot>(new WowTvfsRoot());
    result->m_tvfs = std::move(tvfs);

    // Collect all TVFS entries.
    std::vector<const RootEntry*> tvfsEntryPtrs;
    tvfsEntryPtrs.reserve(result->m_tvfs->entryCount());
    result->m_tvfs->enumerate([&](const RootEntry& e) {
        tvfsEntryPtrs.push_back(&e);
        return true;
    });

    const size_t entryCount = tvfsEntryPtrs.size();
    result->m_entries.resize(entryCount);

    // Parse each encoded TVFS path to extract locale/content flags, FileDataId, and CKey.
    auto enrichEntry = [&](size_t i) {
        auto& dst = result->m_entries[i];
        const auto& src = *tvfsEntryPtrs[i];

        // Copy EKey and file size from TVFS.
        dst.eKey = src.eKey;
        dst.fileSize = src.fileSize;

        if (!isWowTvfsPath(src.path)) {
            // Non-matching entry — keep original TVFS fields.
            dst.cKey = src.cKey;
            dst.localeFlags = src.localeFlags;
            dst.contentFlags = src.contentFlags;
            dst.fileDataId = src.fileDataId;
            dst.path = src.path;
            return;
        }

        WowTvfsPathInfo info{};
        if (!parseWowTvfsPath(src.path, info)) {
            dst.cKey = src.cKey;
            dst.localeFlags = src.localeFlags;
            dst.contentFlags = src.contentFlags;
            dst.fileDataId = src.fileDataId;
            dst.path = src.path;
            return;
        }

        dst.cKey = info.cKey;
        dst.localeFlags = info.localeFlags;
        dst.contentFlags = info.contentFlags;
        dst.fileDataId = info.fileDataId;

        // Enrich with human-readable path from listfile if available.
        if (!listfilePaths.empty()) {
            auto it = listfilePaths.find(info.fileDataId);
            if (it != listfilePaths.end())
                dst.path = it->second;
        }
    };

    // Parallel entry processing.
    if (pool && entryCount > 1000) {
        size_t numThreads = std::max<size_t>(pool->threadCount(), 1);
        size_t chunkSize = (entryCount + numThreads - 1) / numThreads;
        size_t chunks = (entryCount + chunkSize - 1) / chunkSize;

        utils::JobGroup jobGroup;
        jobGroup.add(chunks);
        for (size_t c = 0; c < chunks; ++c) {
            interfaces::WorkerTask task;
            task.fn = [&, c]() {
                size_t begin = c * chunkSize;
                size_t end = std::min(begin + chunkSize, entryCount);
                for (size_t i = begin; i < end; ++i)
                    enrichEntry(i);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    } else {
        for (size_t i = 0; i < entryCount; ++i)
            enrichEntry(i);
    }

    result->buildIndex(pool);
    return result;
}

// ============================================================================
// WowTvfsRoot — RootManifest interface
// ============================================================================

std::vector<const RootEntry*> WowTvfsRoot::findByPath(const std::string& path) const {
    if (m_byPath.empty())
        return {};
    auto key = storages::common::normalizeCascPath(path);
    return m_byPath.findAll(m_entries, key);
}

std::vector<const RootEntry*> WowTvfsRoot::findByFileDataId(u32 fileDataId, FileIdHint /*hint*/) const {
    return m_byFileDataId.findAll(m_entries, fileDataId);
}

bool WowTvfsRoot::hasFileDataId(u32 fileDataId, FileIdHint /*hint*/) const {
    return m_byFileDataId.contains(fileDataId);
}

const std::vector<RootEntry>& WowTvfsRoot::entries() const {
    return m_entries;
}

std::vector<RootEntry>& WowTvfsRoot::mutableEntries() {
    return m_entries;
}

// ============================================================================
// WowTvfsRoot — index building
// ============================================================================

void WowTvfsRoot::buildIndex(interfaces::WorkerPool* /*pool*/) {
    m_byFileDataId.reserve(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); ++i) {
        auto& e = m_entries[i];
        if (e.fileDataId != kInvalidFileDataId)
            m_byFileDataId.emplace(e.fileDataId, i);
        if (!e.path.empty()) {
            auto key = storages::common::normalizeCascPath(e.path);
            m_byPath.emplace(std::move(key), i);
        }
    }
}

// ============================================================================
// WowTvfsRoot — listfile parsing
// ============================================================================

std::unordered_map<u32, std::string> WowTvfsRoot::parseListfile(std::span<const u8> data) {
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
