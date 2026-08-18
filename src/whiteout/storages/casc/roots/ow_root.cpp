// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "ow_root.h"

#include "../../common/byte_order.h"
#include "../../common/string_utils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
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

/// Lowercase hex of a 64-bit GUID, most significant nibble first — the form
/// CascLib prints after byte-reversing the little-endian GUID.
static std::string guidHex(u64 guid) {
    static const char hex[] = "0123456789abcdef";
    std::string s(16, '0');
    for (size_t i = 16; i-- > 0;) {
        s[i] = hex[guid & 0xF];
        guid >>= 4;
    }
    return s;
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

/// Parse CMF entries and hash data from a (decrypted) body.
/// @param body       Data after the header (entries + hash data).
/// @param header     Parsed CMF header.
/// @param outEntries Appended with root entries from the CMF.
/// @param pathPrefix Normalized asset folder, separator-terminated.
static bool parseCmfBody(std::span<const u8> body, const CmfHeader& header,
                         std::vector<RootEntry>& outEntries, const std::string& pathPrefix) {
    if (header.dataCount < 0 || header.entryCount < 0)
        return false;

    // Skip CMF entries (we don't need them for root lookup, but must account for their size).
    size_t const entryBlockSize = size_t(header.entryCount) * kCmfEntrySize;
    if (entryBlockSize > body.size())
        return false;

    auto hashBody = body.subspan(entryBlockSize);

    size_t const recordSize =
        (header.buildVersion >= kBuildHashData135) ? kHashDataSize : kHashDataOldSize;
    size_t const totalHashSize = size_t(header.dataCount) * recordSize;
    if (totalHashSize > hashBody.size())
        return false;

    // Parse hash data entries.
    for (i32 i = 0; i < header.dataCount; ++i) {
        const u8* p = hashBody.data() + size_t(i) * recordSize;

        CmfHashData hd;
        hd.guid = readLE64(p);
        hd.size = readLE32(p + 8);
        if (recordSize == kHashDataSize) {
            hd.unknown = p[12];
            std::memcpy(hd.contentKey.data(), p + 13, 16);
        } else {
            hd.unknown = 0;
            std::memcpy(hd.contentKey.data(), p + 12, 16);
        }

        RootEntry re;
        re.cKey = hd.contentKey;
        re.fileNameHash = hd.guid;
        re.fileSize = hd.size;
        re.path = pathPrefix + guidHex(hd.guid);
        outEntries.push_back(std::move(re));
    }

    return true;
}

/// Parse a complete (unencrypted) CMF file and append entries.
static bool parseCmf(std::span<const u8> data, const std::string& pathPrefix,
                     std::vector<RootEntry>& outEntries) {
    CmfHeader header;
    if (!parseCmfHeader(data, header))
        return false;

    // Encrypted CMFs need an AES key derived by a per-build generator; we have
    // none, so the manifest is skipped rather than taking the whole root down.
    if (header.encrypted)
        return false;

    size_t const headerSize = cmfHeaderSize(header.buildVersion);
    if (headerSize > data.size())
        return false;

    auto body = data.subspan(headerSize);
    return parseCmfBody(body, header, outEntries, pathPrefix);
}

/// Recover the GUID from an asset path's trailing 16 hex digits.
static bool parseTrailingGuid(std::string_view path, u64& out) {
    auto const slash = path.find_last_of('\\');
    auto const leaf = (slash == std::string_view::npos) ? path : path.substr(slash + 1);
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
                                      interfaces::WorkerPool* /*pool*/) {
    if (!isOwTextRoot(data))
        return nullptr;

    std::vector<OwRootFileEntry> manifestEntries;
    if (!parseTextRoot(data, manifestEntries))
        return nullptr;

    return fromManifestEntries(std::move(manifestEntries), std::move(resolver));
}

std::unique_ptr<OwRoot> OwRoot::fromManifestEntries(std::vector<OwRootFileEntry> manifestEntries,
                                                    CKeyResolver resolver,
                                                    interfaces::WorkerPool* /*pool*/) {

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

    // If a resolver is provided, fetch and parse CMF files. Headers are read
    // first so the entry vector is sized once: a current Overwatch install
    // yields twelve million entries, and letting the vector grow into that
    // costs more transient memory than every manifest put together.
    if (resolver) {
        struct PendingCmf {
            std::vector<u8> data;
            std::string pathPrefix;
        };
        std::vector<PendingCmf> pending;
        size_t assetCount = 0;

        for (auto& mf : root->m_manifestEntries) {
            // Only process .cmf files.
            auto fileName = mf.fileName;
            toLowerInPlace(fileName);
            if (fileName.size() < 4 || fileName.substr(fileName.size() - 4) != ".cmf")
                continue;

            auto cmfData = resolver(mf.md5);
            if (cmfData.empty())
                continue;

            CmfHeader header;
            if (!parseCmfHeader(cmfData, header) || header.encrypted || header.dataCount < 0)
                continue;
            assetCount += size_t(header.dataCount);

            // The prefix is derived from the manifest name as written, before
            // normalization folds its case — the field tags are what carry the
            // platform, locale and asset apart.
            pending.push_back(
                {std::move(cmfData),
                 storages::common::normalizeCascPath(buildAssetPathPrefix(mf.fileName)) + "\\"});
        }

        root->m_entries.reserve(root->m_entries.size() + assetCount);
        for (auto& cmf : pending)
            parseCmf(cmf.data, cmf.pathPrefix, root->m_entries);
    }

    root->buildIndices();
    return root;
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
    for (auto* e : findByGuid(guid)) {
        if (e->path == path)
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

void OwRoot::buildIndices() {
    m_byGuid.reserve(m_entries.size() - m_manifestRowCount);
    m_byManifestPath.reserve(m_manifestRowCount);

    for (size_t i = 0; i < m_entries.size(); ++i) {
        auto& e = m_entries[i];
        if (e.fileNameHash != 0)
            m_byGuid.emplace_back(e.fileNameHash, u32(i));
        if (i < m_manifestRowCount && !e.path.empty())
            m_byManifestPath.emplace(e.path, i);
    }

    // Stable so that entries for one GUID stay in manifest order — the first
    // hit is the one from the manifest listed first in the root.
    std::stable_sort(m_byGuid.begin(), m_byGuid.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
}

} // namespace whiteout::storages::casc
