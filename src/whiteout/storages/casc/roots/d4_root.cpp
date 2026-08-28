// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../common/byte_order.h"
#include "../../common/string_utils.h"
#include "d4_root.h"

#include <whiteout/interfaces.h>
#include <whiteout/sno/core_toc.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string_view>
#include <unordered_set>

namespace whiteout::storages::casc {

using storages::common::normalizeCascPath;

/// End-of-chain marker for the index chains.
static constexpr u32 kNoChain = 0xFFFFFFFFu;

/// Run @p fn over 0..@p count on @p pool, or on this thread without one.
/// @p fn must not itself put work on the pool — waiting on pool tasks from
/// inside a pool task deadlocks once the workers are all held here.
/// @param minParallel Below this many items the pool is not worth its
///        bookkeeping. The default suits per-entry work; a caller whose items
///        are each worth milliseconds should lower it.
template <typename Fn>
static void runOverEntries(size_t count, interfaces::WorkerPool* pool, Fn&& fn,
                           size_t minParallel = 2000) {
    if (pool == nullptr || count < minParallel) {
        for (size_t i = 0; i < count; ++i)
            fn(i);
        return;
    }
    // Four chunks per thread rather than one: the combined-meta containers
    // differ in size by two orders of magnitude, and one chunk each leaves the
    // pool waiting on whichever thread drew the biggest.
    size_t const threads = std::max<size_t>(pool->threadCount(), 1);
    size_t const chunk = std::max<size_t>(count / (threads * 4), 1);
    size_t const chunks = (count + chunk - 1) / chunk;

    utils::JobGroup jobGroup;
    jobGroup.add(chunks);
    for (size_t c = 0; c < chunks; ++c) {
        interfaces::WorkerTask task;
        task.fn = [&fn, &jobGroup, c, chunk, count]() {
            size_t const begin = c * chunk;
            size_t const end = std::min(begin + chunk, count);
            for (size_t i = begin; i < end; ++i)
                fn(i);
            jobGroup.done();
        };
        pool->submit(task);
    }
    jobGroup.wait();
}

// ============================================================================
// D4 folder structure constants
// ============================================================================

/// Known D4 root-level folder prefixes (before locale expansion).
/// D4 CASC has:  Base, Speech, Text  (and locale variants like enUS_Base, etc.)
[[maybe_unused]] static constexpr std::string_view kRootFolders[] = {"base", "speech", "text"};

/// Known D4 sub-folder names within each root folder.
static constexpr std::string_view kSubFolders[] = {"child", "meta", "payload", "paylow", "paymed"};

/// Sub-folders that carry payload data (used for shared-payload resolution).
static constexpr std::string_view kPayloadSubFolders[] = {"payload", "paylow", "paymed"};

/// True if @p tag — a 1-based index into kSubFolders, 0 meaning "no SNO path" —
/// names a payload folder.
static constexpr bool isPayloadTag(u8 tag) {
    if (tag == 0 || tag > std::size(kSubFolders))
        return false;
    for (auto pf : kPayloadSubFolders)
        if (kSubFolders[tag - 1] == pf)
            return true;
    return false;
}

/// Signature of D4 combined meta files (e.g. Texture-Global-Global.dat).
static constexpr u32 kCombinedMetaMagic = 0x44CF00F5;

// ============================================================================
// Helpers
// ============================================================================

namespace {

/// Try to parse an integer from a string_view.  Returns false on failure.
inline bool tryParseInt(std::string_view sv, i32& out) {
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc{} && ptr == sv.data() + sv.size();
}

/// Parse a D4 file stem like "12345" or "12345-2" into snoId and optional subId.
/// Returns false if the stem doesn't match the expected numeric pattern.
bool parseSnoStem(std::string_view stem, i32& snoId, i32& subId) {
    subId = -1;
    auto dashPos = stem.find('-');
    if (dashPos == std::string_view::npos) {
        return tryParseInt(stem, snoId);
    }
    if (!tryParseInt(stem.substr(0, dashPos), snoId))
        return false;
    return tryParseInt(stem.substr(dashPos + 1), subId);
}

/// Build the enriched path for a D4 SNO entry.
/// Format: <folder>:<subfolder>\<GroupDir>\<Name>[-<subId>].<ext>
std::string buildEnrichedPath(std::string_view folder, std::string_view subfolder,
                              const sno::TocEntry& toc, i32 subId,
                              const char* nameOverride = nullptr) {
    const char* ext = sno::snoGroupExtension(toc.group);
    const char* groupDir = sno::snoGroupDir(toc.group);

    std::string path;
    // Pre-calculate approximate capacity to avoid reallocs.
    path.reserve(folder.size() + 1 + subfolder.size() + 1 + (groupDir ? std::strlen(groupDir) : 4) +
                 1 + (nameOverride ? std::strlen(nameOverride) : toc.name.size()) + 8 +
                 (ext ? std::strlen(ext) + 1 : 4));

    path.append(folder);
    path.push_back(':'); // D4 uses colon between folder and subfolder
    path.append(subfolder);
    path.push_back('\\');
    path.append(groupDir ? groupDir : "Unknown");
    path.push_back('\\');
    path.append(nameOverride ? nameOverride : toc.name.c_str());

    if (subId >= 0) {
        path.push_back('-');
        char buf[16];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), subId);
        path.append(buf, static_cast<size_t>(ptr - buf));
    }

    if (ext && ext[0]) {
        path.push_back('.');
        path.append(ext);
    }

    return path;
}

/// Describes a parsed D4 TVFS entry that refers to a numeric SNO file.
struct ParsedSnoPath {
    std::string_view folder;    ///< e.g. "base", "enus_base"
    std::string_view subfolder; ///< e.g. "child", "payload"
    i32 snoId = 0;
    i32 subId = -1;
    bool valid = false;
};

/// Try to classify a normalized TVFS entry path as a D4 SNO path.
/// Expected format: <folder>:<subfolder>\<snoId>[-<subId>]
/// The colon separates the sub-container prefix from the rest.
ParsedSnoPath classifySnoPath(std::string_view path) {
    ParsedSnoPath result{};

    // Find the colon that separates folder from the rest.
    auto colonPos = path.find(':');
    if (colonPos == std::string_view::npos || colonPos == 0)
        return result;

    result.folder = path.substr(0, colonPos);
    auto rest = path.substr(colonPos + 1);

    // rest should be "subfolder\stem".
    auto bsPos = rest.find('\\');
    if (bsPos == std::string_view::npos || bsPos == 0)
        return result;

    auto subfolder = rest.substr(0, bsPos);
    auto stem = rest.substr(bsPos + 1);
    if (stem.empty())
        return result;

    // Validate subfolder is one of the known D4 sub-folders.
    bool knownSub = false;
    for (auto sf : kSubFolders) {
        if (subfolder == sf) {
            knownSub = true;
            break;
        }
    }
    if (!knownSub)
        return result;

    if (!parseSnoStem(stem, result.snoId, result.subId))
        return result;

    result.subfolder = subfolder;
    result.valid = true;
    return result;
}

/// SharedPayloads mapping: snoId → sharedSnoId.
struct SharedPayloads {
    std::unordered_map<i32, i32> mapping;

    bool parse(std::span<const u8> data) {
        if (data.size() < 8)
            return false;
        auto read32 = [](const u8* p) -> i32 {
            i32 v;
            std::memcpy(&v, p, 4);
            return v;
        };
        // i32 unk1 = read32(data.data());
        i32 const count = read32(data.data() + 4);
        if (count < 0 || static_cast<size_t>(count) * 8 + 8 > data.size())
            return false;

        mapping.reserve(static_cast<size_t>(count));
        const u8* ptr = data.data() + 8;
        for (i32 i = 0; i < count; ++i) {
            i32 const snoId = read32(ptr);
            i32 const sharedSnoId = read32(ptr + 4);
            mapping.emplace(snoId, sharedSnoId);
            ptr += 8;
        }
        return true;
    }
};

/// EncryptedSNOs mapping: snoId → (snoGroup, keyId).
struct EncryptedSnoEntry {
    i32 snoGroup;
    u64 keyId;
};

struct EncryptedSNOs {
    std::unordered_map<i32, EncryptedSnoEntry> entries;

    bool parse(std::span<const u8> data) {
        if (data.size() < 8)
            return false;
        auto read32 = [](const u8* p) -> i32 {
            i32 v;
            std::memcpy(&v, p, 4);
            return v;
        };
        // i32 unkHash = read32(data.data());
        i32 const count = read32(data.data() + 4);
        if (count < 0 || static_cast<size_t>(count) * 16 + 8 > data.size())
            return false;

        entries.reserve(static_cast<size_t>(count));
        const u8* ptr = data.data() + 8;
        for (i32 i = 0; i < count; ++i) {
            i32 const snoGroup = read32(ptr);
            i32 const snoId = read32(ptr + 4);
            u64 keyId;
            std::memcpy(&keyId, ptr + 8, 8);
            entries.emplace(snoId, EncryptedSnoEntry{snoGroup, keyId});
            ptr += 16;
        }
        return true;
    }
};

/// Index entry from a combined meta file.
struct CombinedMetaEntry {
    i32 snoId;
    u32 size;
    size_t offset; ///< Byte offset of this entry's data within the container.
};

/// Parse the index of a combined meta file and compute each entry's data offset.
/// Entries are 8-byte aligned.  Texture groups (group 44) have an extra
/// 8-byte gap before each entry.
/// Returns an empty vector on failure or if the magic doesn't match.
std::vector<CombinedMetaEntry> parseCombinedMetaIndex(std::span<const u8> data, bool isTexture) {
    if (data.size() < 8)
        return {};

    u32 sig = 0;
    std::memcpy(&sig, data.data(), 4);
    if (sig != kCombinedMetaMagic)
        return {};

    u32 fileCount = 0;
    std::memcpy(&fileCount, data.data() + 4, 4);

    size_t const indexEnd = 8 + static_cast<size_t>(fileCount) * 8;
    if (indexEnd > data.size())
        return {};

    std::vector<CombinedMetaEntry> entries(fileCount);

    // First pass: read index pairs.
    for (u32 i = 0; i < fileCount; ++i) {
        size_t const off = 8 + static_cast<size_t>(i) * 8;
        std::memcpy(&entries[i].snoId, data.data() + off, 4);
        std::memcpy(&entries[i].size, data.data() + off + 4, 4);
    }

    // Second pass: walk the data section to compute byte offsets.
    constexpr size_t alignment = 8;
    size_t pos = indexEnd;
    for (u32 i = 0; i < fileCount; ++i) {
        pos = (pos + alignment - 1) & ~(alignment - 1);
        if (isTexture)
            pos += 8;

        entries[i].offset = pos;

        if (pos + entries[i].size > data.size()) {
            entries.resize(i); // truncated file
            break;
        }

        pos += entries[i].size;
    }

    return entries;
}

/// Map a locale tag (e.g. "enUS", "Global") to a LocaleMasks value.
/// Case-insensitive.  Returns LocaleMasks::All for "Global" or unknown tags.
u32 localeFromTag(std::string_view tag) {
    // Build a small static lookup table.
    struct Entry {
        const char* tag;
        u32 mask;
    };
    static constexpr Entry kTable[] = {
        {"enus", LocaleMasks::enUS}, {"kokr", LocaleMasks::koKR}, {"frfr", LocaleMasks::frFR},
        {"dede", LocaleMasks::deDE}, {"zhcn", LocaleMasks::zhCN}, {"eses", LocaleMasks::esES},
        {"zhtw", LocaleMasks::zhTW}, {"engb", LocaleMasks::enGB}, {"encn", LocaleMasks::enCN},
        {"entw", LocaleMasks::enTW}, {"esmx", LocaleMasks::esMX}, {"ruru", LocaleMasks::ruRU},
        {"ptbr", LocaleMasks::ptBR}, {"itit", LocaleMasks::itIT}, {"ptpt", LocaleMasks::ptPT},
        {"jajp", LocaleMasks::jaJP}, {"plpl", LocaleMasks::plPL}, {"thth", LocaleMasks::thTH},
        {"trtr", LocaleMasks::trTR},
    };

    if (tag.size() != 4)
        return LocaleMasks::All;

    char lower[4];
    for (int i = 0; i < 4; ++i)
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[i])));
    std::string_view const lv(lower, 4);

    for (auto& e : kTable)
        if (lv == e.tag)
            return e.mask;

    return LocaleMasks::All;
}

/// Extract the locale flags from a combined meta file name.
/// Pattern: {GroupName}-{Category}-{Language}[-{extra}].dat
/// The 3rd segment is the language tag.  "Global" → All locales.
u32 localeFromCombinedFileName(std::string_view path) {
    auto sep = path.find_last_of("\\/:");
    if (sep != std::string_view::npos)
        path = path.substr(sep + 1);

    // Strip ".dat".
    if (path.size() >= 5 && path.substr(path.size() - 4) == ".dat")
        path = path.substr(0, path.size() - 4);

    // Find 2nd and 3rd dash-separated segments.
    auto d1 = path.find('-');
    if (d1 == std::string_view::npos)
        return LocaleMasks::All;
    auto d2 = path.find('-', d1 + 1);
    if (d2 == std::string_view::npos)
        return LocaleMasks::All;

    auto d3 = path.find('-', d2 + 1);
    auto langTag =
        path.substr(d2 + 1, (d3 != std::string_view::npos) ? d3 - d2 - 1 : std::string_view::npos);

    // "Global" means all locales.
    if (langTag.size() == 6) {
        char buf[6];
        for (int i = 0; i < 6; ++i)
            buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(langTag[i])));
        if (std::string_view(buf, 6) == "global")
            return LocaleMasks::All;
    }

    return localeFromTag(langTag);
}

/// Try to resolve the SnoGroup from a combined meta file name.
/// Pattern: {GroupName}-{Category}-{Language}[-{extra}].dat
/// e.g. "Texture-Global-Global.dat", "StringList-Text-enUS.dat".
/// Returns SnoGroup::None if the name doesn't match.
sno::SnoGroup groupFromCombinedFileName(std::string_view path) {
    // Strip path prefixes (handles both separators and CASC "base:" prefix).
    auto sep = path.find_last_of("\\/:");
    if (sep != std::string_view::npos)
        path = path.substr(sep + 1);

    // Must end with ".dat".
    if (path.size() < 5 || path.substr(path.size() - 4) != ".dat")
        return sno::SnoGroup::None;
    path = path.substr(0, path.size() - 4);

    // Need at least 2 dashes: GroupName-Category-Language.
    auto dash1 = path.find('-');
    if (dash1 == std::string_view::npos)
        return sno::SnoGroup::None;
    if (path.find('-', dash1 + 1) == std::string_view::npos)
        return sno::SnoGroup::None;

    // Skip encrypted variants: last segment starts with "0x".
    auto lastDash = path.rfind('-');
    if (lastDash != std::string_view::npos) {
        auto suffix = path.substr(lastDash + 1);
        if (suffix.size() > 2 && suffix[0] == '0' && suffix[1] == 'x')
            return sno::SnoGroup::None;
    }

    auto groupStr = path.substr(0, dash1);
    for (int gid = -1; gid <= 180; ++gid) {
        auto g = static_cast<sno::SnoGroup>(gid);
        const char* gname = sno::snoGroupName(g);
        if (!gname)
            continue;
        std::string_view const gnameView(gname);
        if (gnameView.size() != groupStr.size())
            continue;
        // Case-insensitive compare (TVFS paths are lowercased).
        bool match = true;
        for (size_t i = 0; i < gnameView.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(gnameView[i])) !=
                std::tolower(static_cast<unsigned char>(groupStr[i]))) {
                match = false;
                break;
            }
        }
        if (match)
            return g;
    }
    return sno::SnoGroup::None;
}

} // anonymous namespace

// ============================================================================
// D4Root::create
// ============================================================================

std::unique_ptr<D4Root> D4Root::create(std::unique_ptr<TvfsRoot> tvfs, const EKeyReader& reader,
                                       interfaces::WorkerPool* pool) {
    if (!tvfs)
        return nullptr;

    // --- Phase 1: Find CoreTOC.dat and the side-tables in the TVFS tree ---
    // D4 TVFS uses ':' as the sub-container separator: "base:coretoc.dat".
    // One walk rather than three lookups, so the caller need not pay for a
    // TVFS path index that nothing else here reads.
    static constexpr std::string_view kTocPath = "base:coretoc.dat";
    static constexpr std::string_view kSharedPayloadsPath =
        "base:coretocsharedpayloadsmapping.dat";
    static constexpr std::string_view kEncryptedSnosPath = "base:encryptedsnos.dat";

    std::array<u8, 16> tocEKey{}, sharedEKey{}, encryptedEKey{};
    tvfs->enumerate([&](const RootEntry& e) {
        if (e.path == kTocPath)
            tocEKey = e.eKey;
        else if (e.path == kSharedPayloadsPath)
            sharedEKey = e.eKey;
        else if (e.path == kEncryptedSnosPath)
            encryptedEKey = e.eKey;
        return true;
    });

    if (tocEKey == std::array<u8, 16>{})
        return nullptr;

    auto tocData = reader(tocEKey);
    if (tocData.empty())
        return nullptr;

    sno::CoreToc coreToc;
    if (!coreToc.parse(tocData))
        return nullptr;

    // --- Phase 2: Parse optional side-tables ---

    SharedPayloads sharedPayloads;
    if (sharedEKey != std::array<u8, 16>{}) {
        auto spData = reader(sharedEKey);
        if (!spData.empty())
            sharedPayloads.parse(spData);
    }

    EncryptedSNOs encryptedSNOs;
    if (encryptedEKey != std::array<u8, 16>{}) {
        auto encData = reader(encryptedEKey);
        if (!encData.empty())
            encryptedSNOs.parse(encData);
    }

    // Build encrypted name overrides.
    // We don't attempt to decrypt EncryptedNameDict files (would need key ring
    // integration), but we mark entries as _encrypted_<snoId> so they're
    // identifiable.
    std::unordered_map<i32, std::string> encryptedNames;
    for (auto& [snoId, enc] : encryptedSNOs.entries) {
        encryptedNames.emplace(snoId, "_encrypted_" + std::to_string(snoId));
    }

    // --- Phase 3: Build enriched entry table ---

    auto result = std::unique_ptr<D4Root>(new D4Root());
    result->m_tvfs = std::move(tvfs);

    // The TVFS entries become ours: every field except the path survives
    // enrichment unchanged, so copying them would be 1.6 M entries of pure
    // duplication with the originals kept alive for nothing.
    result->m_entries = result->m_tvfs->takeEntries();
    const size_t entryCount = result->m_entries.size();

    // Phase 4 needs each entry's classification and phase 3 is the only pass
    // that has it — the original path is gone once the path is rewritten.
    std::vector<u8> subFolderTag(entryCount, 0); // 0 = not an SNO path
    std::vector<i32> subIds(entryCount, -1);

    runOverEntries(
        entryCount, pool,
        [&](size_t i) {
            auto& e = result->m_entries[i];
            if (e.path.empty())
                return;

            auto parsed = classifySnoPath(e.path);
            if (!parsed.valid)
                return; // Not an SNO entry — keep the TVFS path.

            for (size_t sf = 0; sf < std::size(kSubFolders); ++sf) {
                if (parsed.subfolder == kSubFolders[sf]) {
                    subFolderTag[i] = static_cast<u8>(sf + 1);
                    break;
                }
            }
            subIds[i] = parsed.subId;

            // In D4, the snoId serves as the fileDataId.
            e.fileDataId = static_cast<u32>(parsed.snoId);

            const sno::TocEntry* toc = coreToc.findById(parsed.snoId);
            if (!toc || toc->name.empty())
                return; // Unknown SNO — keep the TVFS path.

            const char* nameOverride = nullptr;
            auto encIt = encryptedNames.find(parsed.snoId);
            if (encIt != encryptedNames.end())
                nameOverride = encIt->second.c_str();

            // parsed's views point into e.path; the replacement is fully built
            // before the assignment frees the buffer under them.
            e.path = buildEnrichedPath(parsed.folder, parsed.subfolder, *toc, parsed.subId,
                                       nameOverride);
        });

    // --- Phase 4: Create shared-payload aliases ---
    // For each shared payload mapping (snoId → sharedSnoId), find the shared
    // entry in payload folders and create an alias entry for the original snoId.
    if (!sharedPayloads.mapping.empty()) {
        // Build a quick snoId→entry index from enriched entries for payload folders.
        // Only entries that some mapping actually points at are worth indexing;
        // the payload folders hold most of the root.
        std::unordered_set<i32> shared;
        shared.reserve(sharedPayloads.mapping.size());
        for (auto& [snoId, sharedSnoId] : sharedPayloads.mapping)
            shared.insert(sharedSnoId);

        std::unordered_multimap<i32, size_t> payloadBySnoId;
        for (size_t i = 0; i < entryCount; ++i) {
            if (!isPayloadTag(subFolderTag[i]))
                continue;
            i32 const snoId = static_cast<i32>(result->m_entries[i].fileDataId);
            if (shared.count(snoId))
                payloadBySnoId.emplace(snoId, i);
        }

        for (auto& [snoId, sharedSnoId] : sharedPayloads.mapping) {
            const sno::TocEntry* toc = coreToc.findById(snoId);
            if (!toc || toc->name.empty())
                continue;

            auto range = payloadBySnoId.equal_range(sharedSnoId);
            for (auto it = range.first; it != range.second; ++it) {
                const auto& sharedEntry = result->m_entries[it->second];
                std::string_view const sharedPath(sharedEntry.path);
                auto const colon = sharedPath.find(':');
                if (colon == std::string_view::npos)
                    continue;

                RootEntry alias;
                alias.cKey = sharedEntry.cKey;
                alias.eKey = sharedEntry.eKey;
                alias.fileDataId = static_cast<u32>(snoId);
                alias.fileNameHash = sharedEntry.fileNameHash;
                alias.localeFlags = sharedEntry.localeFlags;
                alias.contentFlags = sharedEntry.contentFlags;
                alias.fileSize = sharedEntry.fileSize;
                alias.path = buildEnrichedPath(sharedPath.substr(0, colon),
                                               kSubFolders[subFolderTag[it->second] - 1], *toc,
                                               subIds[it->second]);
                result->m_entries.push_back(std::move(alias));
            }
        }
    }

    // --- Phase 5: Synthesize entries from combined meta files ---
    // Some D4 SNO groups (Texture, StringList, etc.) bundle all their meta
    // data into combined files (e.g. "Texture-Base-Global.dat") instead of
    // having individual per-asset files.  Parse these and create virtual
    // enriched entries so all SNOs appear in the file listing.
    //
    // Virtual entries set containerOffset/containerSize so that readFile()
    // extracts just the individual SNO from the decoded container, and
    // headerPrefix prepends the 16-byte synthetic SNO header (DEADBEEF).
    {
        // Collect combined meta file candidates first (before mutating m_entries).
        struct CombinedMetaCandidate {
            size_t sourceIndex; // Index in m_entries, for stable ordering.
            std::string folder; // e.g. "base"
            sno::SnoGroup group;
            std::array<u8, 16> eKey;
            std::array<u8, 16> cKey;
            u32 fileDataId;
            u32 fileNameHash;
            u32 localeFlags; // from filename language tag
            u32 contentFlags;
        };

        // A handful of the million-odd entries are combined meta files, so the
        // scan goes wide and the few hits take a lock.
        std::vector<CombinedMetaCandidate> candidates;
        std::mutex candidatesMutex;
        runOverEntries(entryCount, pool, [&](size_t i) {
            const auto& entry = result->m_entries[i];
            if (entry.path.empty())
                return;

            auto group = groupFromCombinedFileName(entry.path);
            if (group == sno::SnoGroup::None)
                return;

            auto colonPos = entry.path.find(':');
            CombinedMetaCandidate cand;
            cand.sourceIndex = i;
            cand.folder = (colonPos != std::string::npos) ? entry.path.substr(0, colonPos) : "base";
            cand.group = group;
            cand.eKey = entry.eKey;
            cand.cKey = entry.cKey;
            cand.fileDataId = entry.fileDataId;
            cand.fileNameHash = static_cast<u32>(entry.fileNameHash);
            cand.localeFlags = localeFromCombinedFileName(entry.path);
            cand.contentFlags = entry.contentFlags;

            std::lock_guard<std::mutex> const lk(candidatesMutex);
            candidates.push_back(std::move(cand));
        });

        // Entry order, not scan order — where a synthesized entry lands in
        // m_entries is part of what callers see.
        std::sort(candidates.begin(), candidates.end(),
                  [](const CombinedMetaCandidate& a, const CombinedMetaCandidate& b) {
                      return a.sourceIndex < b.sourceIndex;
                  });

        // Each container is a few megabytes of BLTE to decode and a few tens of
        // thousands of entries to build, and there are dozens of them. The
        // reader must stay off the pool for this: an outer task waiting on
        // frame tasks that no worker is free to run would deadlock the open.
        std::vector<std::vector<RootEntry>> synthesized(candidates.size());
        runOverEntries(candidates.size(), pool, [&](size_t c) {
            const auto& cand = candidates[c];
            if (cand.eKey == std::array<u8, 16>{})
                return;

            auto fileData = reader(cand.eKey);
            if (fileData.empty())
                return;

            const bool isTexture = (cand.group == sno::SnoGroup::Texture);
            auto cmEntries = parseCombinedMetaIndex(fileData, isTexture);
            if (cmEntries.empty())
                return;

            // Resolve the format hash for this group's SNO header.
            u32 fmtHash = 0;
            auto fhIt = coreToc.formatHashes().find(static_cast<i32>(cand.group));
            if (fhIt != coreToc.formatHashes().end())
                fmtHash = fhIt->second;

            // Build a 16-byte synthetic SNO header: DEADBEEF + formatHash + 0 + 0.
            std::array<u8, 16> hdrPrefix{};
            {
                u32 magic = sno::kSnoMagic;
                u32 zero = 0;
                std::memcpy(hdrPrefix.data() + 0, &magic, 4);
                std::memcpy(hdrPrefix.data() + 4, &fmtHash, 4);
                std::memcpy(hdrPrefix.data() + 8, &zero, 4);
                std::memcpy(hdrPrefix.data() + 12, &zero, 4);
            }

            auto& out = synthesized[c];
            out.reserve(cmEntries.size());
            for (auto& cm : cmEntries) {
                const sno::TocEntry* toc = coreToc.findById(cm.snoId);
                if (!toc || toc->name.empty())
                    continue;

                RootEntry virtEntry;
                virtEntry.cKey = cand.cKey;
                virtEntry.eKey = cand.eKey;
                virtEntry.fileDataId = static_cast<u32>(cm.snoId);
                virtEntry.fileNameHash = cand.fileNameHash;
                virtEntry.localeFlags = cand.localeFlags;
                virtEntry.contentFlags = cand.contentFlags;
                virtEntry.fileSize = 16 + cm.size; // header + entry data
                virtEntry.containerOffset = static_cast<u64>(cm.offset);
                virtEntry.containerSize = cm.size;
                virtEntry.headerSize = 16;
                virtEntry.headerPrefix = hdrPrefix;
                virtEntry.path = buildEnrichedPath(cand.folder, "meta", *toc, -1);
                out.push_back(std::move(virtEntry));
            }
        }, /*minParallel=*/2);

        size_t total = result->m_entries.size();
        for (auto& out : synthesized)
            total += out.size();
        result->m_entries.reserve(total);
        for (auto& out : synthesized)
            result->m_entries.insert(result->m_entries.end(), std::make_move_iterator(out.begin()),
                                     std::make_move_iterator(out.end()));
    }

    // --- Phase 6: Build lookup index ---
    result->buildIndex(pool);

    return result;
}

// ============================================================================
// Index building
// ============================================================================

void D4Root::buildIndex(interfaces::WorkerPool* pool) {
    size_t const n = m_entries.size();
    m_pathChain.assign(n, kNoChain);
    m_snoChain.assign(n, kNoChain);
    m_byPathMap.reserve(n);
    m_bySnoMap.reserve(n);

    std::vector<u64> hashes(n);
    runOverEntries(n, pool, [&](size_t i) {
        hashes[i] = m_entries[i].path.empty()
                        ? 0
                        : storages::common::normalizedCascPathHash64(m_entries[i].path);
    });

    // Filled back to front so each chain comes out in ascending entry order,
    // which is the order the root lists its entries in.
    for (size_t i = n; i-- > 0;) {
        if (hashes[i] != 0) {
            if (auto* head = m_byPathMap.find(hashes[i])) {
                m_pathChain[i] = *head;
                m_byPathMap.insertOrAssign(hashes[i], static_cast<u32>(i));
            } else {
                m_byPathMap.emplace(hashes[i], static_cast<u32>(i));
            }
        }
        u32 const snoId = m_entries[i].fileDataId;
        if (snoId != kInvalidFileDataId) {
            if (auto* head = m_bySnoMap.find(snoId)) {
                m_snoChain[i] = *head;
                m_bySnoMap.insertOrAssign(snoId, static_cast<u32>(i));
            } else {
                m_bySnoMap.emplace(snoId, static_cast<u32>(i));
            }
        }
    }
}

// ============================================================================
// RootManifest interface
// ============================================================================

std::vector<const RootEntry*> D4Root::findByPath(const std::string& path) const {
    auto normalized = normalizeCascPath(path);
    return findByNormalizedPath(normalized);
}

std::vector<const RootEntry*> D4Root::findByNormalizedPath(
    const std::string& normalizedPath) const {
    std::vector<const RootEntry*> results;
    auto* head = m_byPathMap.find(storages::common::cascPathHash64(normalizedPath));
    if (!head)
        return results;
    for (u32 idx = *head; idx != kNoChain; idx = m_pathChain[idx]) {
        if (storages::common::normalizedCascPathEquals(m_entries[idx].path, normalizedPath))
            results.push_back(&m_entries[idx]);
    }
    return results;
}

bool D4Root::hasPath(const std::string& normalizedPath) const {
    auto* head = m_byPathMap.find(storages::common::cascPathHash64(normalizedPath));
    if (!head)
        return false;
    for (u32 idx = *head; idx != kNoChain; idx = m_pathChain[idx]) {
        if (storages::common::normalizedCascPathEquals(m_entries[idx].path, normalizedPath))
            return true;
    }
    return false;
}

/// Map a FileIdHint to the expected D4 subfolder name.
/// Returns empty string_view for None (meaning: return "child" subfolder).
static std::string_view hintToSubfolder(FileIdHint hint) {
    switch (hint) {
    case FileIdHint::None:
        return "child";
    case FileIdHint::Meta:
        return "meta";
    case FileIdHint::Payload:
        return "payload";
    case FileIdHint::Paylow:
        return "paylow";
    case FileIdHint::Paymed:
        return "paymed";
    }
    return "child";
}

/// Extract the subfolder segment from an enriched D4 path.
/// Enriched paths have the form: <folder>:<subfolder>\<rest>
static std::string_view extractSubfolder(std::string_view path) {
    auto colonPos = path.find(':');
    if (colonPos == std::string_view::npos)
        return {};
    auto rest = path.substr(colonPos + 1);
    auto bsPos = rest.find('\\');
    if (bsPos == std::string_view::npos)
        return rest;
    return rest.substr(0, bsPos);
}

std::vector<const RootEntry*> D4Root::findByFileDataId(u32 fileDataId, FileIdHint hint) const {
    // In D4, fileDataId == snoId.  Use the snoId index, then filter by
    // the subfolder implied by the hint.
    std::vector<const RootEntry*> all;
    if (fileDataId != kInvalidFileDataId) {
        if (auto* head = m_bySnoMap.find(fileDataId)) {
            for (u32 idx = *head; idx != kNoChain; idx = m_snoChain[idx])
                all.push_back(&m_entries[idx]);
        }
    }
    if (all.empty() || hint == FileIdHint::None)
        return all;

    auto targetSub = hintToSubfolder(hint);
    std::vector<const RootEntry*> filtered;
    for (auto* e : all) {
        if (extractSubfolder(e->path) == targetSub)
            filtered.push_back(e);
    }
    return filtered;
}

void D4Root::enumerateUnder(const std::string& normalizedPrefix,
                            std::function<bool(const RootEntry&)> callback) const {
    if (!callback)
        return;
    // Linear scan over our entries — could be optimized with a trie if needed,
    // but D4Root is a wrapper and the common case is full enumeration or
    // lookup by exact path.
    for (auto& e : m_entries) {
        if (e.path.size() >= normalizedPrefix.size() &&
            e.path.compare(0, normalizedPrefix.size(), normalizedPrefix) == 0) {
            if (!callback(e))
                break;
        }
    }
}

const std::vector<RootEntry>& D4Root::entries() const {
    return m_entries;
}

std::vector<RootEntry>& D4Root::mutableEntries() {
    return m_entries;
}

} // namespace whiteout::storages::casc
