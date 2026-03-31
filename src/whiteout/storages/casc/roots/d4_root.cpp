// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "d4_root.h"
#include "tvfs_root.h"
#include "root.h"
#include "../../common/string_utils.h"

#include <whiteout/interfaces.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <memory>
#include <unordered_set>

namespace whiteout::storages::casc {

using storages::common::normalizeCascPath;

// ============================================================================
// Path parsing helpers
// ============================================================================

namespace {

/// Try to parse a D4 numeric SNO filename component.
/// Expected formats: "12345" or "12345-3".
/// Returns true if the entire filename is valid numeric (or numeric-dash-numeric).
static bool parseSnoFilename(const std::string& filename, i32& snoId, i32& subId) {
    if (filename.empty()) return false;

    auto dash = filename.find('-');
    const char* begin = filename.data();
    const char* end = filename.data() + filename.size();

    if (dash != std::string::npos) {
        // {snoId}-{subId}
        auto [p1, ec1] = std::from_chars(begin, begin + dash, snoId);
        if (ec1 != std::errc{} || p1 != begin + dash) return false;

        auto [p2, ec2] = std::from_chars(begin + dash + 1, end, subId);
        if (ec2 != std::errc{} || p2 != end) return false;
    } else {
        // {snoId}
        auto [p, ec] = std::from_chars(begin, end, snoId);
        if (ec != std::errc{} || p != end) return false;
        subId = -1;
    }

    return true;
}

/// Build an enriched path from a D4 TVFS numeric path.
/// Input:  "base:meta/12345" or "base:payload/12345-3"
/// Output: "base:meta/Texture/SomeName.tex" or "base:payload/Texture/SomeName-3.tex"
/// Returns empty string if enrichment is not possible.
static std::string enrichPath(const std::string& originalPath,
                              const sno::CoreToc& coreToc) {
    // Find last path separator.
    auto sep = originalPath.rfind('\\');
    if (sep == std::string::npos)
        sep = originalPath.rfind('/');
    if (sep == std::string::npos) return {};

    std::string prefix = originalPath.substr(0, sep);
    std::string filename = originalPath.substr(sep + 1);

    i32 snoId = 0, subId = -1;
    if (!parseSnoFilename(filename, snoId, subId)) return {};

    // Look up in CoreTOC.
    auto* entry = coreToc.findById(snoId);
    if (!entry) return {};

    const char* groupDir = sno::snoGroupDir(entry->group);
    const char* ext = sno::snoGroupExtension(entry->group);
    if (!groupDir || !ext) return {};

    // Build enriched path: {prefix}\{groupDir}\{name}.{ext}
    std::string enriched = prefix + "\\" + groupDir + "\\" + entry->name;
    if (subId >= 0)
        enriched += "-" + std::to_string(subId);
    enriched += ".";
    enriched += ext;

    return enriched;
}

/// Extract snoId from a D4 TVFS path.
/// Returns -1 if the path doesn't contain a numeric SNO filename.
static i32 extractSnoId(const std::string& path) {
    auto sep = path.rfind('\\');
    if (sep == std::string::npos)
        sep = path.rfind('/');
    if (sep == std::string::npos) return -1;

    std::string filename = path.substr(sep + 1);
    i32 snoId = 0, subId = -1;
    if (!parseSnoFilename(filename, snoId, subId)) return -1;
    return snoId;
}

/// Signature of D4 combined meta files (e.g. Texture-Global-Global.dat).
static constexpr u32 kCombinedMetaMagic = 0x44CF00F5;

/// Data alignment within combined meta files.
static constexpr size_t kCombinedAlignment = 8;

/// SNO magic for synthetic headers.
static constexpr u32 kSnoMagic = 0xDEADBEEF;

/// Resolve SnoGroup from a combined meta file name.
/// Combined meta files follow the naming pattern:
///   {GroupName}-{Category}-{Language}[-{extra}].dat
/// e.g. "Texture-Global-Global.dat", "StringList-Text-enUS.dat".
static sno::SnoGroup groupFromCombinedFileName(const std::string& path) {
    // Extract just the file name (strip path/CASC prefixes).
    std::string name = path;
    auto sep = name.find_last_of("\\/:");
    if (sep != std::string::npos)
        name = name.substr(sep + 1);

    // The group name is everything before the first dash.
    auto dash = name.find('-');
    if (dash == std::string::npos)
        return sno::SnoGroup::None;
    std::string groupStr = name.substr(0, dash);

    for (int gid = -1; gid <= 180; ++gid) {
        auto g = static_cast<sno::SnoGroup>(gid);
        const char* gname = sno::snoGroupName(g);
        if (gname && groupStr == gname)
            return g;
    }
    return sno::SnoGroup::None;
}

/// Returns true if a combined meta file name looks encrypted
/// (hex-hash suffix like "-0x1234abcd.dat").
static bool isEncryptedDatFile(const std::string& fileName) {
    auto dot = fileName.rfind('.');
    std::string base = (dot != std::string::npos) ? fileName.substr(0, dot) : fileName;
    auto lastDash = base.rfind('-');
    if (lastDash == std::string::npos) return false;
    std::string suffix = base.substr(lastDash + 1);
    return suffix.size() > 2 && suffix[0] == '0' && suffix[1] == 'x';
}

/// An entry from a combined meta file.
struct CombinedEntry {
    i32 snoId;
    std::shared_ptr<std::vector<u8>> fileData;
    size_t dataOffset;
    size_t dataSize;
    sno::SnoGroup group;
};

/// Parse a single combined meta file and return its entries.
static std::vector<CombinedEntry> parseCombinedMetaFile(
    std::shared_ptr<std::vector<u8>> data,
    sno::SnoGroup group) {
    std::vector<CombinedEntry> result;
    const auto& buf = *data;
    if (buf.size() < 8) return result;

    u32 sig = 0;
    std::memcpy(&sig, buf.data(), 4);
    if (sig != kCombinedMetaMagic) return result;

    u32 fileCount = 0;
    std::memcpy(&fileCount, buf.data() + 4, 4);

    size_t indexEnd = 8 + static_cast<size_t>(fileCount) * 8;
    if (indexEnd > buf.size()) return result;

    struct IndexEntry { i32 snoId; u32 size; };
    std::vector<IndexEntry> index(fileCount);
    for (u32 i = 0; i < fileCount; ++i) {
        size_t off = 8 + static_cast<size_t>(i) * 8;
        std::memcpy(&index[i].snoId, buf.data() + off, 4);
        std::memcpy(&index[i].size, buf.data() + off + 4, 4);
    }

    // Walk the data section. Entries are 8-byte aligned.
    // Textures have an extra 8-byte gap before each entry.
    const bool isTexture = (group == sno::SnoGroup::Texture);
    size_t pos = indexEnd;

    for (u32 i = 0; i < fileCount; ++i) {
        pos = (pos + kCombinedAlignment - 1) & ~(kCombinedAlignment - 1);
        if (isTexture) pos += 8;
        if (pos + index[i].size > buf.size()) break;

        // Verify snoId at the entry start.
        i32 check = 0;
        std::memcpy(&check, buf.data() + pos, 4);
        if (check != index[i].snoId) {
            pos += index[i].size;
            continue;
        }

        CombinedEntry ce;
        ce.snoId = index[i].snoId;
        ce.fileData = data;
        ce.dataOffset = pos;
        ce.dataSize = index[i].size;
        ce.group = group;
        result.push_back(std::move(ce));

        pos += index[i].size;
    }

    return result;
}

} // anonymous namespace

// ============================================================================
// D4Root
// ============================================================================

std::unique_ptr<D4Root> D4Root::create(std::unique_ptr<TvfsRoot> tvfsRoot,
                                       sno::CoreToc coreToc,
                                       FileResolver resolver,
                                       interfaces::WorkerPool* pool) {
    if (!tvfsRoot) return nullptr;

    auto root = std::make_unique<D4Root>();
    root->m_coreToc = std::move(coreToc);

    // Copy all entries from the TVFS root.
    root->m_entries.reserve(tvfsRoot->entryCount());
    tvfsRoot->enumerate([&](const RootEntry& re) -> bool {
        root->m_entries.push_back(re);
        return true;
    });

    // Enrich entries: resolve numeric paths and assign fileDataIds.
    for (auto& entry : root->m_entries) {
        if (entry.path.empty()) continue;

        // Try to extract snoId and set as fileDataId.
        i32 snoId = extractSnoId(entry.path);
        if (snoId >= 0) {
            entry.fileDataId = static_cast<u32>(snoId);

            // Try to enrich the path with human-readable name.
            std::string enriched = enrichPath(entry.path, root->m_coreToc);
            if (!enriched.empty())
                entry.path = std::move(enriched);
        }
    }

    // Load combined meta files to create synthetic entries for groups
    // that bundle their metas (Texture, StringList, etc.).
    if (resolver)
        root->loadCombinedMetas(resolver);

    if (root->m_entries.empty()) return nullptr;

    root->buildIndices(pool);
    return root;
}

std::vector<RootEntry> D4Root::findByPath(const std::string& path) const {
    auto key = normalizeCascPath(path);
    std::vector<RootEntry> results;
    auto range = m_byPath.equal_range(key);
    for (auto it = range.first; it != range.second; ++it)
        results.push_back(m_entries[it->second]);
    return results;
}

std::vector<RootEntry> D4Root::findByFileDataId(u32 fileDataId) const {
    std::vector<RootEntry> results;
    auto range = m_byFileDataId.equal_range(fileDataId);
    for (auto it = range.first; it != range.second; ++it)
        results.push_back(m_entries[it->second]);
    return results;
}

std::vector<RootEntry> D4Root::findByCKey(std::span<const u8, 16> cKey) const {
    std::vector<RootEntry> results;
    for (auto& e : m_entries) {
        if (std::memcmp(e.cKey.data(), cKey.data(), 16) == 0)
            results.push_back(e);
    }
    return results;
}

void D4Root::enumerate(std::function<bool(const RootEntry&)> callback) const {
    for (auto& e : m_entries) {
        if (!callback(e)) break;
    }
}

size_t D4Root::entryCount() const {
    return m_entries.size();
}

const std::vector<u8>* D4Root::findCombinedMeta(i32 snoId) const {
    auto it = m_combinedMetaCache.find(snoId);
    if (it != m_combinedMetaCache.end())
        return &it->second;
    return nullptr;
}

void D4Root::loadCombinedMetas(const FileResolver& resolver) {
    // Find combined meta .dat files among current entries.
    // These follow the naming pattern: {GroupName}-{Category}-{Language}.dat
    // e.g. "Texture-Global-Global.dat", "StringList-Text-enUS.dat"
    std::vector<size_t> combinedIndices;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const auto& path = m_entries[i].path;
        if (path.size() < 4) continue;
        if (path.compare(path.size() - 4, 4, ".dat") != 0) continue;

        // Extract file name portion.
        std::string fname = path;
        auto sep = fname.find_last_of("\\/:");
        if (sep != std::string::npos) fname = fname.substr(sep + 1);

        // Must have at least 2 dashes.
        int dashes = 0;
        for (char c : fname) if (c == '-') ++dashes;
        if (dashes < 2) continue;

        // Skip encrypted variants.
        if (isEncryptedDatFile(fname)) continue;

        // Must resolve to a known group.
        auto group = groupFromCombinedFileName(path);
        if (group == sno::SnoGroup::None) continue;

        combinedIndices.push_back(i);
    }

    if (combinedIndices.empty()) return;

    // Collect existing snoIds so we don't create duplicates.
    std::unordered_set<i32> existingSnoIds;
    for (auto& entry : m_entries) {
        if (entry.fileDataId != kInvalidFileDataId)
            existingSnoIds.insert(static_cast<i32>(entry.fileDataId));
    }

    size_t totalCombinedEntries = 0;

    for (size_t idx : combinedIndices) {
        auto& re = m_entries[idx];
        auto group = groupFromCombinedFileName(re.path);
        if (group == sno::SnoGroup::None) continue;

        // Read the combined meta file from CASC.
        auto fileData = resolver(re);
        if (fileData.empty()) continue;

        auto shared = std::make_shared<std::vector<u8>>(std::move(fileData));
        auto entries = parseCombinedMetaFile(shared, group);
        if (entries.empty()) continue;

        const char* groupDir = sno::snoGroupDir(group);
        const char* ext = sno::snoGroupExtension(group);

        // Look up the format hash for this group from CoreTOC.
        u32 formatHash = 0;
        auto& fmtHashes = m_coreToc.formatHashes();
        auto fhIt = fmtHashes.find(static_cast<i32>(group));
        if (fhIt != fmtHashes.end())
            formatHash = fhIt->second;

        for (auto& ce : entries) {
            // Skip if an individual entry already exists for this snoId.
            if (existingSnoIds.count(ce.snoId)) continue;

            // Build a synthetic SNO blob: 16-byte header + entry data.
            std::vector<u8> syntheticData(16 + ce.dataSize);
            u32 magic = kSnoMagic;
            u32 zero = 0;
            std::memcpy(syntheticData.data() + 0, &magic, 4);
            std::memcpy(syntheticData.data() + 4, &formatHash, 4);
            std::memcpy(syntheticData.data() + 8, &zero, 4);
            std::memcpy(syntheticData.data() + 12, &zero, 4);
            std::memcpy(syntheticData.data() + 16,
                        ce.fileData->data() + ce.dataOffset, ce.dataSize);

            // Build an enriched path.
            std::string path = "base:meta\\";
            if (groupDir) {
                path += groupDir;
                path += "\\";
            }

            // Look up the name from CoreTOC.
            auto* tocEntry = m_coreToc.findById(ce.snoId);
            if (tocEntry && !tocEntry->name.empty()) {
                path += tocEntry->name;
            } else {
                path += std::to_string(ce.snoId);
            }
            if (ext) {
                path += ".";
                path += ext;
            }

            // Create a synthetic RootEntry.
            // The CKey is zero (no real archive backing) — the data comes from
            // the combined meta cache stored in this root.
            RootEntry newEntry;
            newEntry.path = std::move(path);
            newEntry.fileDataId = static_cast<u32>(ce.snoId);

            // Store the synthetic data in the combined meta cache.
            m_combinedMetaCache[ce.snoId] = std::move(syntheticData);

            existingSnoIds.insert(ce.snoId);
            m_entries.push_back(std::move(newEntry));
            ++totalCombinedEntries;
        }
    }
}

void D4Root::buildIndices(interfaces::WorkerPool* pool) {
    size_t n = m_entries.size();

    // Pre-compute lowercase keys.
    // For each entry, we index both the enriched path (entry.path) and the
    // original numeric path (if it was enriched) to allow lookups by either.
    struct PathPair {
        std::string enriched;  // Always present.
        std::string original;  // Non-empty only if enrichment changed the path.
    };
    std::vector<PathPair> pathPairs(n);

    // Reconstruct original paths for enriched entries.
    auto computePaths = [&](size_t start, size_t end) {
        for (size_t i = start; i < end; ++i) {
            auto& entry = m_entries[i];
            if (entry.path.empty()) continue;

            pathPairs[i].enriched = normalizeCascPath(entry.path);

            // If fileDataId is set, this entry was enriched — also index the
            // original numeric path for backward compatibility.
            if (entry.fileDataId != kInvalidFileDataId) {
                // Reconstruct original: find the prefix up to the group dir.
                // The enriched path is "{prefix}\{groupDir}\{name}.{ext}".
                // The original was "{prefix}\{snoId}" or "{prefix}\{snoId}-{subId}".
                // We stored the snoId in fileDataId. Look for subId in filename.
                auto sep = entry.path.rfind('\\');
                if (sep == std::string::npos)
                    sep = entry.path.rfind('/');
                if (sep != std::string::npos) {
                    std::string filename = entry.path.substr(sep + 1);
                    // Find the prefix before the group dir.
                    // enriched = "base:meta\Texture\SomeName.tex"
                    //              ^prefix  ^group  ^filename
                    // We need the prefix before the group dir.
                    auto prefixSep = entry.path.rfind('\\', sep - 1);
                    if (prefixSep == std::string::npos)
                        prefixSep = entry.path.rfind('/', sep - 1);
                    if (prefixSep != std::string::npos) {
                        std::string prefix = entry.path.substr(0, prefixSep);

                        // Check if filename has a subId.
                        i32 subId = -1;
                        auto dash = filename.rfind('-');
                        if (dash != std::string::npos) {
                            // Could be part of the name. Only parse if what
                            // follows the dash (before the dot) is numeric.
                            auto dot = filename.rfind('.');
                            if (dot != std::string::npos && dash < dot) {
                                std::string subStr = filename.substr(dash + 1, dot - dash - 1);
                                const char* b = subStr.data();
                                const char* e = b + subStr.size();
                                i32 tmp = 0;
                                auto [p, ec] = std::from_chars(b, e, tmp);
                                if (ec == std::errc{} && p == e)
                                    subId = tmp;
                            }
                        }

                        std::string original = prefix + "\\" +
                            std::to_string(entry.fileDataId);
                        if (subId >= 0)
                            original += "-" + std::to_string(subId);

                        std::string lowerOrig = normalizeCascPath(original);
                        if (lowerOrig != pathPairs[i].enriched)
                            pathPairs[i].original = std::move(lowerOrig);
                    }
                }
            }
        }
    };

    if (pool && n > 1000) {
        size_t numThreads = std::max<size_t>(pool->threadCount(), 1);
        size_t chunkSize = (n + numThreads - 1) / numThreads;
        size_t chunks = (n + chunkSize - 1) / chunkSize;

        utils::JobGroup jobGroup;
        jobGroup.add(chunks);
        for (size_t c = 0; c < chunks; ++c) {
            interfaces::WorkerTask task;
            task.fn = [&, c]() {
                size_t start = c * chunkSize;
                size_t end = std::min(start + chunkSize, n);
                computePaths(start, end);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    } else {
        computePaths(0, n);
    }

    // Build the path and fileDataId indices.
    m_byPath.reserve(n * 2); // Enriched + original paths.
    m_byFileDataId.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!pathPairs[i].enriched.empty())
            m_byPath.emplace(std::move(pathPairs[i].enriched), i);
        if (!pathPairs[i].original.empty())
            m_byPath.emplace(std::move(pathPairs[i].original), i);
        if (m_entries[i].fileDataId != kInvalidFileDataId)
            m_byFileDataId.emplace(m_entries[i].fileDataId, i);
    }
}

} // namespace whiteout::storages::casc
