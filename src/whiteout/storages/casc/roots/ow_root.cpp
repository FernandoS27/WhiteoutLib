// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "ow_root.h"

#include "../../common/byte_order.h"

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

/// Minimum supported build version (OW 1.30+).
static constexpr u32 kMinBuildVersion = 52320;

/// Maximum sane build version.
static constexpr u32 kMaxBuildVersion = 12923648;

/// Version 25 CMF header size.
static constexpr size_t kCmfHeader25Size = 40;

/// Version 26+ CMF header size.
static constexpr size_t kCmfHeader26Size = 48;

/// HashData v24 record size (no Unknown byte): GUID(8) + Size(4) + CKey(16) = 28.
static constexpr size_t kHashData24Size = 28;

/// HashData v25+ record size: GUID(8) + Size(4) + Unknown(1) + CKey(16) = 29.
static constexpr size_t kHashData25Size = 29;

/// CMF entry size: index(4) + hashA(8) + hashB(8) = 20.
static constexpr size_t kCmfEntrySize = 20;

/// Parse a hex character to nibble value. Returns 0xFF on invalid input.
static u8 hexNibble(char c) {
    if (c >= '0' && c <= '9') return u8(c - '0');
    if (c >= 'a' && c <= 'f') return u8(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return u8(c - 'A' + 10);
    return 0xFF;
}

/// Parse a 32-character hex string into a 16-byte MD5 key.
static bool parseHexKey(std::string_view hex, std::array<u8, 16>& out) {
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        u8 hi = hexNibble(hex[size_t(i * 2)]);
        u8 lo = hexNibble(hex[size_t(i * 2 + 1)]);
        if (hi == 0xFF || lo == 0xFF) return false;
        out[size_t(i)] = u8((hi << 4) | lo);
    }
    return true;
}

/// Lowercase a string in-place.
static void toLowerInPlace(std::string& s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
}

/// Normalize a path: lowercase, forward slashes to backslashes, strip leading/trailing separators.
static std::string normalizePath(std::string_view path) {
    std::string result(path);
    for (auto& c : result) {
        if (c == '/') c = '\\';
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    // Strip leading separators.
    while (!result.empty() && result.front() == '\\')
        result.erase(result.begin());
    // Strip trailing separators.
    while (!result.empty() && result.back() == '\\')
        result.pop_back();
    return result;
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
static bool parseTextRoot(std::span<const u8> data,
                          std::vector<OwRootFileEntry>& outEntries) {
    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());

    // Split into lines.
    auto lines = splitView(text, '\n');
    if (lines.empty()) return false;

    // First line is the header: #COLUMN1|COLUMN2|...
    auto headerLine = lines[0];
    // Strip trailing \r if present.
    if (!headerLine.empty() && headerLine.back() == '\r')
        headerLine.remove_suffix(1);

    if (headerLine.empty() || headerLine[0] != '#') return false;

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

        if (col == "FILEID")      idxFileId = int(i);
        else if (col == "MD5")    idxMd5 = int(i);
        else if (col == "CHUNK_ID") idxChunkId = int(i);
        else if (col == "PRIORITY") idxPriority = int(i);
        else if (col == "MPRIORITY") idxMPriority = int(i);
        else if (col == "FILENAME") idxFileName = int(i);
        else if (col == "INSTALLPATH") idxInstallPath = int(i);
    }

    // MD5 and FILENAME are required.
    if (idxMd5 < 0 || idxFileName < 0) return false;

    // Parse data rows.
    for (size_t lineIdx = 1; lineIdx < lines.size(); ++lineIdx) {
        auto line = lines[lineIdx];
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty()) continue;

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

/// Parse CMF header. Supports v25 (pre-1.48) and v26+ (1.48+).
static bool parseCmfHeader(std::span<const u8> data, CmfHeader& out) {
    if (data.size() < kCmfHeader25Size) return false;

    // Read as v26 first (larger header).
    if (data.size() >= kCmfHeader26Size) {
        out.buildVersion = readLE32(data.data());

        // Check build version range.
        if (out.buildVersion >= kMinBuildVersion && out.buildVersion < kMaxBuildVersion) {
            // V26 header (1.48+):
            // buildVersion(4) + unk04(4) + unk08(4) + unk0C(4) + unk10(4) + unk14(4)
            // + unk18(4) + dataPatchRecordCount(4) + dataCount(4) + entryPatchRecordCount(4)
            // + entryCount(4) + magic(4) = 48 bytes
            out.dataCount = i32(readLE32(data.data() + 32));
            out.entryCount = i32(readLE32(data.data() + 40));
            out.magic = readLE32(data.data() + 44);

            // Check if encrypted.
            out.encrypted = ((out.magic >> 8) == kCmfEncryptedMagic);
            if (out.encrypted)
                out.version = u8(out.magic & 0xFF);
            else
                out.version = u8((out.magic >> 24) & 0xFF);

            // Validate: version should be >= 26 for this header layout.
            if (out.version >= 26) return true;
        }
    }

    // Try v25 header (pre-1.48):
    // buildVersion(4) + unk04(4) + unk08(4) + unk0C(4) + unk10(4) + unk14(4)
    // + dataCount(4) + unk1C(4) + entryCount(4) + magic(4) = 40 bytes
    out.buildVersion = readLE32(data.data());
    if (out.buildVersion < kMinBuildVersion || out.buildVersion >= kMaxBuildVersion)
        return false;

    out.dataCount = i32(readLE32(data.data() + 24));
    out.entryCount = i32(readLE32(data.data() + 32));
    out.magic = readLE32(data.data() + 36);

    out.encrypted = ((out.magic >> 8) == kCmfEncryptedMagic);
    if (out.encrypted)
        out.version = u8(out.magic & 0xFF);
    else
        out.version = u8((out.magic >> 24) & 0xFF);

    return true;
}

/// Parse CMF entries and hash data from a (decrypted) body.
/// @param body     Data after the header (entries + hash data).
/// @param header   Parsed CMF header.
/// @param outEntries Appended with root entries from the CMF.
/// @param cmfName  Name of the CMF file (used for path prefix).
static bool parseCmfBody(std::span<const u8> body, const CmfHeader& header,
                         std::vector<RootEntry>& outEntries,
                         const std::string& cmfName) {
    // Skip CMF entries (we don't need them for root lookup, but must account for their size).
    size_t entryBlockSize = size_t(header.entryCount) * kCmfEntrySize;
    if (entryBlockSize > body.size()) return false;

    auto hashBody = body.subspan(entryBlockSize);

    // Determine hash data record size based on version.
    bool useV25 = (header.version >= 25);
    size_t recordSize = useV25 ? kHashData25Size : kHashData24Size;
    size_t totalHashSize = size_t(header.dataCount) * recordSize;
    if (totalHashSize > hashBody.size()) return false;

    // Parse hash data entries.
    for (i32 i = 0; i < header.dataCount; ++i) {
        const u8* p = hashBody.data() + size_t(i) * recordSize;

        CmfHashData hd;
        hd.guid = readLE64(p);
        hd.size = readLE32(p + 8);
        if (useV25) {
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
        re.path = cmfName + "\\" + std::to_string(hd.guid);
        outEntries.push_back(std::move(re));
    }

    return true;
}

/// Parse a complete (unencrypted) CMF file and append entries.
static bool parseCmf(std::span<const u8> data, const std::string& cmfName,
                     std::vector<RootEntry>& outEntries) {
    CmfHeader header;
    if (!parseCmfHeader(data, header)) return false;

    // We do not support encrypted CMFs in this implementation.
    if (header.encrypted) return false;

    // Determine header size.
    size_t headerSize = (header.version >= 26) ? kCmfHeader26Size : kCmfHeader25Size;
    if (headerSize > data.size()) return false;

    auto body = data.subspan(headerSize);
    return parseCmfBody(body, header, outEntries, cmfName);
}

/// Check if a file has the OW text root format.
/// Returns true if data starts with '#' (header line).
static bool isOwTextRoot(std::span<const u8> data) {
    if (data.empty()) return false;
    return data[0] == '#';
}

} // anonymous namespace

// ============================================================================
// OwRoot public API
// ============================================================================

std::unique_ptr<OwRoot> OwRoot::parse(std::span<const u8> data,
                                      CKeyResolver resolver,
                                      interfaces::WorkerPool* /*pool*/) {
    if (!isOwTextRoot(data)) return nullptr;

    std::vector<OwRootFileEntry> manifestEntries;
    if (!parseTextRoot(data, manifestEntries))
        return nullptr;

    return fromManifestEntries(std::move(manifestEntries), std::move(resolver));
}

std::unique_ptr<OwRoot> OwRoot::fromManifestEntries(
    std::vector<OwRootFileEntry> manifestEntries,
    CKeyResolver resolver,
    interfaces::WorkerPool* /*pool*/) {

    auto root = std::make_unique<OwRoot>();
    root->m_manifestEntries = std::move(manifestEntries);

    // For each manifest entry, create a root entry for the manifest file itself.
    for (auto& mf : root->m_manifestEntries) {
        RootEntry re;
        re.cKey = mf.md5;
        re.path = normalizePath(mf.fileName);
        root->m_entries.push_back(std::move(re));
    }

    // If a resolver is provided, fetch and parse CMF files.
    if (resolver) {
        for (auto& mf : root->m_manifestEntries) {
            // Only process .cmf files.
            auto fileName = mf.fileName;
            toLowerInPlace(fileName);
            if (fileName.size() < 4 ||
                fileName.substr(fileName.size() - 4) != ".cmf")
                continue;

            auto cmfData = resolver(mf.md5);
            if (cmfData.empty()) continue;

            std::string cmfName = normalizePath(mf.fileName);
            // Strip .cmf extension for prefix.
            if (cmfName.size() > 4)
                cmfName = cmfName.substr(0, cmfName.size() - 4);

            parseCmf(cmfData, cmfName, root->m_entries);
        }
    }

    root->buildIndices();
    return root;
}

std::vector<const RootEntry*> OwRoot::findByPath(const std::string& path) const {
    auto normalizedPath = normalizePath(path);
    std::vector<const RootEntry*> results;
    auto range = m_byPath.equal_range(normalizedPath);
    for (auto it = range.first; it != range.second; ++it)
        results.push_back(&m_entries[it->second]);
    return results;
}

std::vector<const RootEntry*> OwRoot::findByFileDataId(u32 /*fileDataId*/) const {
    // Overwatch does not use FileDataIds.
    return {};
}

std::vector<const RootEntry*> OwRoot::findByGuid(u64 guid) const {
    std::vector<const RootEntry*> results;
    auto range = m_byGuid.equal_range(guid);
    for (auto it = range.first; it != range.second; ++it)
        results.push_back(&m_entries[it->second]);
    return results;
}

void OwRoot::buildIndices() {
    m_byGuid.reserve(m_entries.size());
    m_byPath.reserve(m_entries.size());

    for (size_t i = 0; i < m_entries.size(); ++i) {
        auto& e = m_entries[i];
        if (e.fileNameHash != 0)
            m_byGuid.emplace(e.fileNameHash, i);
        if (!e.path.empty())
            m_byPath.emplace(e.path, i);
    }
}

} // namespace whiteout::storages::casc
