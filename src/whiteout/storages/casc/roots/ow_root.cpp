// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "ow_root.h"

#include "ow/ow_asset_types.h"
#include "ow/ow_manifest_crypto.h"

#include "../../common/byte_order.h"
#include "../../common/string_utils.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <optional>
#include <string_view>

namespace whiteout::storages::casc {

using storages::common::readLE32;
using storages::common::readLE64;

// ============================================================================
// Helpers
// ============================================================================

namespace {

/// Overwatch CMF magic constants.
static constexpr u32 kCmfEncryptedMagic = 0x636D66; // "cmf"

/// Oldest build we will read a CMF from; retail Overwatch 1.0 was build 24919.
static constexpr u32 kMinBuildVersion = 20000;

/// Maximum sane build version.
static constexpr u32 kMaxBuildVersion = 12923648;

/// Build versions at which the CMF header grew, per CascLib's overwatch.h.
static constexpr u32 kBuild122Ptr = 47161;
static constexpr u32 kBuild148Ptr = 68309;

/// Overwatch 1.35 added the Unknown byte to the HashData record.
static constexpr u32 kBuildHashData135 = 57230;

/// CMF header sizes, one per layout.
static constexpr size_t kCmfHeader100Size = 36;
static constexpr size_t kCmfHeader122Size = 40;
static constexpr size_t kCmfHeader148Size = 48;

/// Pre-1.35 HashData record: GUID(8) + Size(4) + CKey(16) = 28.
static constexpr size_t kHashDataOldSize = 28;

/// 1.35+ HashData record: GUID(8) + Size(4) + Unknown(1) + CKey(16) = 29.
static constexpr size_t kHashDataSize = 29;

/// CMF entry size: index(4) + hashA(8) + hashB(8) = 20.
static constexpr size_t kCmfEntrySize = 20;

/// Parse a hex character to nibble value. Returns 0xFF on invalid input.
static u8 hexNibble(char c) {
    if (c >= '0' && c <= '9')
        return u8(c - '0');
    if (c >= 'a' && c <= 'f')
        return u8(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return u8(c - 'A' + 10);
    return 0xFF;
}

/// Parse a 32-character hex string into a 16-byte MD5 key.
static bool parseHexKey(std::string_view hex, std::array<u8, 16>& out) {
    if (hex.size() != 32)
        return false;
    for (int i = 0; i < 16; ++i) {
        u8 const hi = hexNibble(hex[size_t(i * 2)]);
        u8 const lo = hexNibble(hex[size_t(i * 2 + 1)]);
        if (hi == 0xFF || lo == 0xFF)
            return false;
        out[size_t(i)] = u8((hi << 4) | lo);
    }
    return true;
}

/// Lowercase a string in-place.
static void toLowerInPlace(std::string& s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
}

// ============================================================================
// Text root file parser
// ============================================================================

/// Split a string_view by a delimiter, returning a vector of string_views.
static std::vector<std::string_view> splitView(std::string_view sv, char delim) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= sv.size()) {
        auto pos = sv.find(delim, start);
        if (pos == std::string_view::npos) {
            parts.push_back(sv.substr(start));
            break;
        }
        parts.push_back(sv.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

/// Parse the Overwatch text root file into manifest entries.
static bool parseTextRoot(std::span<const u8> data, std::vector<OwRootFileEntry>& outEntries) {
    std::string_view const text(reinterpret_cast<const char*>(data.data()), data.size());

    // Split into lines.
    auto lines = splitView(text, '\n');
    if (lines.empty())
        return false;

    // First line is the header: #COLUMN1|COLUMN2|...
    auto headerLine = lines[0];
    // Strip trailing \r if present.
    if (!headerLine.empty() && headerLine.back() == '\r')
        headerLine.remove_suffix(1);

    if (headerLine.empty() || headerLine[0] != '#')
        return false;

    // Parse column names.
    auto columns = splitView(headerLine.substr(1), '|');

    // Map column names to indices.
    int idxFileId = -1, idxMd5 = -1, idxChunkId = -1;
    int idxPriority = -1, idxMPriority = -1;
    int idxFileName = -1, idxInstallPath = -1;

    for (size_t i = 0; i < columns.size(); ++i) {
        auto col = columns[i];
        // Strip trailing \r from column names.
        if (!col.empty() && col.back() == '\r')
            col.remove_suffix(1);

        if (col == "FILEID")
            idxFileId = int(i);
        else if (col == "MD5")
            idxMd5 = int(i);
        else if (col == "CHUNK_ID")
            idxChunkId = int(i);
        else if (col == "PRIORITY")
            idxPriority = int(i);
        else if (col == "MPRIORITY")
            idxMPriority = int(i);
        else if (col == "FILENAME")
            idxFileName = int(i);
        else if (col == "INSTALLPATH")
            idxInstallPath = int(i);
    }

    // MD5 and FILENAME are required.
    if (idxMd5 < 0 || idxFileName < 0)
        return false;

    // Parse data rows.
    for (size_t lineIdx = 1; lineIdx < lines.size(); ++lineIdx) {
        auto line = lines[lineIdx];
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty())
            continue;

        auto fields = splitView(line, '|');

        OwRootFileEntry entry;

        if (idxFileId >= 0 && size_t(idxFileId) < fields.size())
            entry.fileId = std::string(fields[size_t(idxFileId)]);

        if (idxMd5 >= 0 && size_t(idxMd5) < fields.size()) {
            if (!parseHexKey(fields[size_t(idxMd5)], entry.md5))
                continue; // Skip entries with invalid MD5.
        }

        if (idxChunkId >= 0 && size_t(idxChunkId) < fields.size()) {
            auto sv = fields[size_t(idxChunkId)];
            u8 val = 0;
            std::from_chars(sv.data(), sv.data() + sv.size(), val);
            entry.chunkId = val;
        }

        if (idxPriority >= 0 && size_t(idxPriority) < fields.size()) {
            auto sv = fields[size_t(idxPriority)];
            u8 val = 0;
            std::from_chars(sv.data(), sv.data() + sv.size(), val);
            entry.priority = val;
        }

        if (idxMPriority >= 0 && size_t(idxMPriority) < fields.size()) {
            auto sv = fields[size_t(idxMPriority)];
            u8 val = 0;
            std::from_chars(sv.data(), sv.data() + sv.size(), val);
            entry.mPriority = val;
        }

        if (idxFileName >= 0 && size_t(idxFileName) < fields.size())
            entry.fileName = std::string(fields[size_t(idxFileName)]);

        if (idxInstallPath >= 0 && size_t(idxInstallPath) < fields.size())
            entry.installPath = std::string(fields[size_t(idxInstallPath)]);

        outEntries.push_back(std::move(entry));
    }

    return !outEntries.empty();
}

// ============================================================================
// CMF parser
// ============================================================================

/// Size of the CMF header for a given build. The three layouts differ only in
/// how many unknown fields precede the tail.
static size_t cmfHeaderSize(u32 buildVersion) {
    if (buildVersion > kBuild148Ptr)
        return kCmfHeader148Size;
    if (buildVersion > kBuild122Ptr)
        return kCmfHeader122Size;
    return kCmfHeader100Size;
}

/// Parse a CMF header. The layout is chosen by the build version in the first
/// field, the way CascLib and TACTLib do it — the magic cannot be the selector,
/// because where the magic lives is exactly what the layout decides.
static bool parseCmfHeader(std::span<const u8> data, CmfHeader& out) {
    if (data.size() < kCmfHeader100Size)
        return false;

    out.buildVersion = readLE32(data.data());
    if (out.buildVersion < kMinBuildVersion || out.buildVersion >= kMaxBuildVersion)
        return false;

    size_t const headerSize = cmfHeaderSize(out.buildVersion);
    if (data.size() < headerSize)
        return false;

    // All three layouts end the same way: dataCount, an unknown, entryCount,
    // then the magic. Only the run of unknowns ahead of them differs.
    const u8* const tail = data.data() + headerSize;
    out.dataCount = i32(readLE32(tail - 16));
    out.entryCount = i32(readLE32(tail - 8));
    out.magic = readLE32(tail - 4);

    out.encrypted = ((out.magic >> 8) == kCmfEncryptedMagic);
    out.version = out.encrypted ? u8(out.magic & 0xFF) : u8((out.magic >> 24) & 0xFF);
    return true;
}

static constexpr char kHexDigits[] = "0123456789abcdef";

/// Lowercase hex of a 64-bit GUID, most significant nibble first — the form
/// CascLib prints after byte-reversing the little-endian GUID.
static void appendGuidHex(u64 guid, std::string& out) {
    for (int shift = 60; shift >= 0; shift -= 4)
        out.push_back(kHexDigits[(guid >> shift) & 0xF]);
}

/// Extension for an asset, so a GUID reads as a filename. Types the client
/// names get that name; a type it knows but does not register keeps its id,
/// which is still what someone searching the tree would reach for.
static void appendAssetExtension(u64 guid, std::string& out) {
    out.push_back('.');

    auto const typeId = ow::assetTypeId(guid);
    if (typeId == 0) {
        // A GUID the client would reject outright. Its raw field can look just
        // like a valid type id, so mark it rather than let the two blur.
        out.push_back('x');
        auto const field = ow::assetTypeField(guid);
        for (int shift = 8; shift >= 0; shift -= 4)
            out.push_back(kHexDigits[(field >> shift) & 0xF]);
        return;
    }

    auto const name = ow::assetTypeName(typeId);
    if (!name.empty()) {
        out.append(name);
        return;
    }
    for (int shift = 8; shift >= 0; shift -= 4)
        out.push_back(kHexDigits[(typeId >> shift) & 0xF]);
}

/// True when @p entryPath names the same asset as @p query, allowing the query
/// to leave off the type extension that asset paths now carry.
static bool assetPathMatches(std::string_view entryPath, std::string_view query) {
    if (entryPath == query)
        return true;
    return entryPath.size() > query.size() && entryPath.compare(0, query.size(), query) == 0 &&
           entryPath[query.size()] == '.';
}

static bool startsWithNoCase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

/// The run of characters up to the next field separator.
static std::string_view assetToken(std::string_view s) {
    auto const end = s.find_first_of("_.");
    return end == std::string_view::npos ? s : s.substr(0, end);
}

/// Folder prefix for the assets of one CMF, e.g. "Win_SPWin_RCN_LesES_Speech_
/// EExt.cmf" becomes "ContentManifestFiles\Windows-RCN\esES\Speech\".
/// Matches CascLib's BuildAssetFileNameTemplate, except that the field tags are
/// compared case-insensitively: CascLib tests them against upper-case spellings
/// and current Overwatch ships the names lowercased, which would otherwise
/// collapse every field onto the trailing one.
static std::string buildAssetPathPrefix(std::string_view cmfFileName) {
    auto const slash = cmfFileName.find_last_of("\\/");
    std::string_view name =
        (slash == std::string_view::npos) ? cmfFileName : cmfFileName.substr(slash + 1);
    auto const dot = name.find('.');
    if (dot != std::string_view::npos)
        name = name.substr(0, dot);

    std::string platform, locale, asset;

    for (size_t i = 0; i < name.size();) {
        if (name[i] != '_') {
            ++i;
            continue;
        }
        std::string_view const rest = name.substr(i);

        if (startsWithNoCase(rest, "_spwin_")) {
            platform = "Windows";
            i += 6; // Leave the trailing '_' to open the next field.
        } else if (startsWithNoCase(rest, "_eext")) {
            i += 5;
        } else if (rest.size() >= 2 && (rest[1] == 'r' || rest[1] == 'R')) {
            auto const tok = assetToken(rest.substr(1));
            platform += '-';
            platform += tok;
            i += 1 + tok.size();
        } else if (rest.size() >= 2 && (rest[1] == 'l' || rest[1] == 'L')) {
            locale = assetToken(rest.substr(2));
            i += 2 + locale.size();
        } else {
            auto const tok = assetToken(rest.substr(1));
            asset = tok;
            i += 1 + tok.size();
        }
    }

    std::string out = "ContentManifestFiles\\";
    for (auto* field : {&platform, &locale, &asset}) {
        if (!field->empty()) {
            out += *field;
            out += '\\';
        }
    }
    return out;
}

/// Where a CMF's hash records live and how big they are, or nullopt when the
/// header promises more than the body holds.
struct CmfHashBlock {
    const u8* data = nullptr;
    size_t count = 0;
    size_t recordSize = 0;
};

static std::optional<CmfHashBlock> locateHashBlock(std::span<const u8> data,
                                                   const CmfHeader& header) {
    if (header.dataCount < 0 || header.entryCount < 0)
        return std::nullopt;

    size_t const headerSize = cmfHeaderSize(header.buildVersion);
    if (headerSize > data.size())
        return std::nullopt;
    auto body = data.subspan(headerSize);

    // The CMF entry block is not needed for root lookup, but its size has to be
    // stepped over to reach the hash records.
    size_t const entryBlockSize = size_t(header.entryCount) * kCmfEntrySize;
    if (entryBlockSize > body.size())
        return std::nullopt;
    auto hashBody = body.subspan(entryBlockSize);

    CmfHashBlock block;
    block.recordSize =
        (header.buildVersion >= kBuildHashData135) ? kHashDataSize : kHashDataOldSize;
    block.count = size_t(header.dataCount);
    if (block.count * block.recordSize > hashBody.size())
        return std::nullopt;
    block.data = hashBody.data();
    return block;
}

/// Turn a located hash block into root entries, written over @p out.
///
/// Asset paths are not stored — see OwRoot::assetPath. Everything a path is
/// built from is either the GUID in the entry or the manifest the range
/// belongs to.
static void parseHashBlock(const CmfHashBlock& block, RootEntry* out) {
    for (size_t i = 0; i < block.count; ++i) {
        const u8* p = block.data + i * block.recordSize;

        RootEntry& re = out[i];
        re.fileNameHash = readLE64(p);
        re.fileSize = readLE32(p + 8);
        std::memcpy(re.cKey.data(), p + (block.recordSize == kHashDataSize ? 13 : 12), 16);
    }
}

/// Decrypt an encrypted CMF in place and rewrite its magic to the plaintext
/// form, so everything downstream sees an ordinary manifest.
///
/// @param fileName Manifest name exactly as the root lists it; the IV is keyed
///                 on it, and Overwatch hashes the bare file name.
static bool decryptCmfInPlace(std::vector<u8>& data, CmfHeader& header, std::string_view fileName) {
    size_t const headerSize = cmfHeaderSize(header.buildVersion);
    if (data.size() <= headerSize)
        return false;

    auto const slash = fileName.find_last_of("\\/");
    auto const leaf = (slash == std::string_view::npos) ? fileName : fileName.substr(slash + 1);

    ow::CmfCryptoHeader const crypto{header.buildVersion, header.dataCount, header.entryCount,
                                     ow::cmfNonEncryptedMagic(header.magic)};
    if (!ow::decryptCmfBody(std::span<u8>(data).subspan(headerSize), crypto, leaf))
        return false;

    header.magic = crypto.nonEncryptedMagic;
    header.encrypted = false;
    storages::common::writeLE32(data.data() + headerSize - 4, header.magic);
    return true;
}

/// Recover the GUID from an asset path's trailing 16 hex digits.
static bool parseTrailingGuid(std::string_view path, u64& out) {
    auto const slash = path.find_last_of('\\');
    auto leaf = (slash == std::string_view::npos) ? path : path.substr(slash + 1);

    // Asset paths carry a type extension; callers that predate it, or that
    // built the path from a GUID by hand, will not have one.
    auto const dot = leaf.find('.');
    if (dot != std::string_view::npos)
        leaf = leaf.substr(0, dot);

    if (leaf.size() != 16)
        return false;

    u64 guid = 0;
    for (char const c : leaf) {
        u8 const nibble = hexNibble(c);
        if (nibble == 0xFF)
            return false;
        guid = (guid << 4) | nibble;
    }
    out = guid;
    return true;
}

/// Run @p fn over 0..@p count on @p pool, or on this thread without one.
template <typename Fn>
static void runOverIndices(size_t count, Fn&& fn, interfaces::WorkerPool* pool) {
    if (pool == nullptr || count < 2) {
        for (size_t i = 0; i < count; ++i)
            fn(i);
        return;
    }
    utils::JobGroup jobGroup;
    jobGroup.add(count);
    for (size_t i = 0; i < count; ++i) {
        interfaces::WorkerTask task;
        task.fn = [&fn, &jobGroup, i]() {
            fn(i);
            jobGroup.done();
        };
        pool->submit(task);
    }
    jobGroup.wait();
}

/// Past this many runs the merge tree is deeper than a sort is expensive, and
/// the input is not the layout the merge was written for anyway.
static constexpr size_t kMaxMergeRuns = 1024;

/// Merge adjacent sorted runs of @p data in place, pairing them off a round at
/// a time. std::merge takes from the first range on a tie, so runs merged in
/// order give exactly what a stable sort by GUID would have: the first hit for
/// a GUID is the one from the manifest the root lists first.
static void mergeGuidRuns(std::vector<std::pair<u64, u32>>& data, std::vector<size_t> bounds,
                          interfaces::WorkerPool* pool) {
    if (bounds.size() <= 2)
        return;

    using Pair = std::pair<u64, u32>;
    auto const less = [](const Pair& a, const Pair& b) { return a.first < b.first; };

    std::vector<Pair> scratch(data.size());
    bool inScratch = false;

    while (bounds.size() > 2) {
        Pair* const src = inScratch ? scratch.data() : data.data();
        Pair* const dst = inScratch ? data.data() : scratch.data();

        size_t const merges = (bounds.size() - 1) / 2;
        auto mergeOne = [&](size_t m) {
            size_t const a = bounds[2 * m], b = bounds[2 * m + 1], c = bounds[2 * m + 2];
            std::merge(src + a, src + b, src + b, src + c, dst + a, less);
        };

        runOverIndices(merges, mergeOne, pool);

        // An odd run count leaves the last run unpaired, and it still has to
        // move across to stay with the rest.
        if ((bounds.size() - 1) % 2 != 0) {
            size_t const a = bounds[bounds.size() - 2];
            std::copy(src + a, src + bounds.back(), dst + a);
        }
        inScratch = !inScratch;

        std::vector<size_t> next;
        next.reserve(bounds.size() / 2 + 2);
        for (size_t i = 0; i < bounds.size(); i += 2)
            next.push_back(bounds[i]);
        if (next.back() != bounds.back())
            next.push_back(bounds.back());
        bounds = std::move(next);
    }

    if (inScratch)
        data.swap(scratch);
}

/// Check if a file has the OW text root format.
/// Returns true if data starts with '#' (header line).
static bool isOwTextRoot(std::span<const u8> data) {
    if (data.empty())
        return false;
    return data[0] == '#';
}

} // anonymous namespace

// ============================================================================
// OwRoot public API
// ============================================================================

std::unique_ptr<OwRoot> OwRoot::parse(std::span<const u8> data, CKeyResolver resolver,
                                      interfaces::WorkerPool* pool) {
    if (!isOwTextRoot(data))
        return nullptr;

    std::vector<OwRootFileEntry> manifestEntries;
    if (!parseTextRoot(data, manifestEntries))
        return nullptr;

    return fromManifestEntries(std::move(manifestEntries), std::move(resolver), pool);
}

std::unique_ptr<OwRoot> OwRoot::fromManifestEntries(std::vector<OwRootFileEntry> manifestEntries,
                                                    CKeyResolver resolver,
                                                    interfaces::WorkerPool* pool) {

    auto root = std::make_unique<OwRoot>();
    root->m_manifestEntries = std::move(manifestEntries);

    // For each manifest entry, create a root entry for the manifest file itself.
    for (auto& mf : root->m_manifestEntries) {
        RootEntry re;
        re.cKey = mf.md5;
        re.path = storages::common::normalizeCascPath(mf.fileName);
        root->m_entries.push_back(std::move(re));
    }
    root->m_manifestRowCount = root->m_entries.size();

    // If a resolver is provided, fetch and parse CMF files. Fetching and
    // decrypting comes first so every manifest's record count is known before a
    // single entry is written: a current Overwatch install yields twenty-four
    // million of them, and giving each manifest a range up front is what lets
    // the record walk run on the pool and still land in manifest order.
    if (resolver) {
        std::vector<size_t> cmfRows;
        for (size_t i = 0; i < root->m_manifestEntries.size(); ++i) {
            auto fileName = root->m_manifestEntries[i].fileName;
            toLowerInPlace(fileName);
            if (fileName.size() >= 4 && fileName.compare(fileName.size() - 4, 4, ".cmf") == 0)
                cmfRows.push_back(i);
        }

        struct PendingCmf {
            std::vector<u8> data;
            CmfHashBlock block;
            std::string pathPrefix;
            bool ok = false;
        };
        std::vector<PendingCmf> pending(cmfRows.size());

        // Fetching a manifest is a BLTE decode of a few megabytes and
        // decrypting it is an AES pass over the result, both of which are the
        // same work for every manifest — so they go wide, the way the TVFS
        // sub-manifest prefetch does.
        auto fetchOne = [&](size_t i) {
            auto const& mf = root->m_manifestEntries[cmfRows[i]];
            auto& slot = pending[i];

            slot.data = resolver(mf.md5);
            if (slot.data.empty())
                return;

            CmfHeader header;
            if (!parseCmfHeader(slot.data, header) || header.dataCount < 0)
                return;
            if (header.encrypted && !decryptCmfInPlace(slot.data, header, mf.fileName))
                return;

            // A manifest no provider covered stays encrypted, and one whose
            // header promises more records than the body holds is malformed;
            // either way it is skipped rather than taking the whole root down.
            if (header.encrypted)
                return;
            auto block = locateHashBlock(slot.data, header);
            if (!block)
                return;

            slot.block = *block;
            // The prefix is derived from the manifest name as written, before
            // normalization folds its case — the field tags are what carry the
            // platform, locale and asset apart.
            slot.pathPrefix =
                storages::common::normalizeCascPath(buildAssetPathPrefix(mf.fileName)) + "\\";
            slot.ok = true;
        };

        runOverIndices(cmfRows.size(), fetchOne, pool);

        // Every manifest's record count is known now, so each gets a range of
        // its own and the record walk can go wide without giving up the order
        // the root lists them in.
        root->m_cmfPrefix.reserve(pending.size());
        root->m_cmfEntryStart.reserve(pending.size() + 1);
        std::vector<size_t> kept;
        kept.reserve(pending.size());
        size_t total = root->m_entries.size();
        for (size_t i = 0; i < pending.size(); ++i) {
            if (!pending[i].ok)
                continue;
            kept.push_back(i);
            root->m_cmfEntryStart.push_back(u32(total));
            root->m_cmfPrefix.push_back(std::move(pending[i].pathPrefix));
            total += pending[i].block.count;
        }
        root->m_cmfEntryStart.push_back(u32(total));
        root->m_entries.resize(total);

        RootEntry* const base = root->m_entries.data();
        runOverIndices(
            kept.size(),
            [&](size_t k) {
                parseHashBlock(pending[kept[k]].block, base + root->m_cmfEntryStart[k]);
            },
            pool);
    }

    root->buildIndices(pool);
    return root;
}

size_t OwRoot::cmfForEntry(size_t index) const {
    if (index < m_manifestRowCount || m_cmfPrefix.empty())
        return m_cmfPrefix.size();
    auto it = std::upper_bound(m_cmfEntryStart.begin(), m_cmfEntryStart.end(), u32(index));
    if (it == m_cmfEntryStart.begin() || it == m_cmfEntryStart.end())
        return m_cmfPrefix.size();
    return size_t(it - m_cmfEntryStart.begin()) - 1;
}

void OwRoot::buildAssetPath(size_t index, std::string& out) const {
    size_t const cmf = cmfForEntry(index);
    if (cmf >= m_cmfPrefix.size()) {
        out.assign(m_entries[index].path);
        return;
    }
    out.assign(m_cmfPrefix[cmf]);
    appendGuidHex(m_entries[index].fileNameHash, out);
    appendAssetExtension(m_entries[index].fileNameHash, out);
}

std::string OwRoot::assetPath(const RootEntry& entry) const {
    auto const* base = m_entries.data();
    if (&entry < base || &entry >= base + m_entries.size())
        return {};
    std::string out;
    buildAssetPath(size_t(&entry - base), out);
    return out;
}

void OwRoot::enumerate(std::function<bool(const RootEntry&)> callback) const {
    if (!callback)
        return;
    for (size_t i = 0; i < m_manifestRowCount; ++i) {
        if (!callback(m_entries[i]))
            return;
    }

    // Assets carry no path of their own, so each is handed over as a copy with
    // one filled in. The buffer is moved in and back out rather than assigned,
    // so the whole walk allocates once instead of twenty-four million times —
    // which is the cost not storing the paths was meant to avoid, not move.
    RootEntry scratch;
    std::string buf;
    for (size_t i = m_manifestRowCount; i < m_entries.size(); ++i) {
        scratch = m_entries[i]; // its path is empty, so this allocates nothing
        buildAssetPath(i, buf);
        scratch.path = std::move(buf);
        bool const keepGoing = callback(scratch);
        buf = std::move(scratch.path);
        if (!keepGoing)
            return;
    }
}

void OwRoot::enumerateUnder(const std::string& normalizedPrefix,
                            std::function<bool(const RootEntry&)> callback) const {
    if (!callback)
        return;
    enumerate([&](const RootEntry& e) {
        if (e.path.size() >= normalizedPrefix.size() &&
            e.path.compare(0, normalizedPrefix.size(), normalizedPrefix) == 0)
            return callback(e);
        return true;
    });
}

std::vector<const RootEntry*> OwRoot::findByPath(const std::string& path) const {
    return findByNormalizedPath(storages::common::normalizeCascPath(path));
}

std::vector<const RootEntry*> OwRoot::findByNormalizedPath(const std::string& path) const {
    auto results = m_byManifestPath.findAll(m_entries, path);
    if (!results.empty())
        return results;

    u64 guid = 0;
    if (!parseTrailingGuid(path, guid))
        return results;

    // The GUID narrows the candidates to a handful — one per manifest that
    // carries the asset — and the full path then picks the right one.
    std::string candidate;
    for (auto* e : findByGuid(guid)) {
        buildAssetPath(size_t(e - m_entries.data()), candidate);
        if (assetPathMatches(candidate, path))
            results.push_back(e);
    }
    return results;
}

std::vector<const RootEntry*> OwRoot::findByFileDataId(u32 /*fileDataId*/,
                                                       FileIdHint /*hint*/) const {
    // Overwatch does not use FileDataIds.
    return {};
}

std::vector<const RootEntry*> OwRoot::findByGuid(u64 guid) const {
    auto const cmp = [](const std::pair<u64, u32>& a, u64 key) { return a.first < key; };
    auto it = std::lower_bound(m_byGuid.begin(), m_byGuid.end(), guid, cmp);

    std::vector<const RootEntry*> results;
    for (; it != m_byGuid.end() && it->first == guid; ++it)
        results.push_back(&m_entries[it->second]);
    return results;
}

void OwRoot::buildIndices(interfaces::WorkerPool* pool) {
    m_byManifestPath.reserve(m_manifestRowCount);
    for (size_t i = 0; i < m_manifestRowCount; ++i) {
        if (!m_entries[i].path.empty())
            m_byManifestPath.emplace(m_entries[i].path, i);
    }

    m_byGuid.reserve(m_entries.size() - m_manifestRowCount);

    // Every manifest stores its hash records in GUID order, which makes the
    // index a merge of sorted runs rather than a sort of all twenty-four
    // million pairs. The runs are found rather than assumed — a manifest that
    // broke the order would otherwise produce a silently wrong index.
    std::vector<size_t> bounds{0};
    for (size_t i = m_manifestRowCount; i < m_entries.size(); ++i) {
        u64 const guid = m_entries[i].fileNameHash;
        if (guid == 0)
            continue;
        if (!m_byGuid.empty() && guid < m_byGuid.back().first)
            bounds.push_back(m_byGuid.size());
        m_byGuid.emplace_back(guid, u32(i));
    }
    bounds.push_back(m_byGuid.size());

    if (bounds.size() - 1 > kMaxMergeRuns) {
        // Not the layout we expect. Stable so that entries for one GUID stay in
        // manifest order — the first hit is the one from the manifest listed
        // first in the root, which is also what the merge below preserves.
        std::stable_sort(m_byGuid.begin(), m_byGuid.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        return;
    }
    mergeGuidRuns(m_byGuid, std::move(bounds), pool);
}

} // namespace whiteout::storages::casc
