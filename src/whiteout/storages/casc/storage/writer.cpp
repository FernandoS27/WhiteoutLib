// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "writer.h"
#include "../codec/blte.h"
#include "../tables/config.h"
#include "constants.h"
#include "../tables/encoding.h"
#include "../tables/index.h"
#include "../roots/root.h"
#include "../../common/byte_order.h"
#include "../../common/hex.h"
#include "../../common/jenkins.h"
#include "../../common/md5.h"
#include "../../../common/unicode_path.h"

#include <whiteout/common_types.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>

namespace whiteout::storages::casc {

using storages::common::writeBE32;
using storages::common::writeLE32;
using storages::common::pushBE16;
using storages::common::pushBE32;
using storages::common::pushLE32;
using storages::common::hexEncode16;

// ---- Constants local to writer ----

/// TVFS full header size (including EST fields).
static constexpr u32 kTvfsHeaderSize = 46;

/// WoW locale flags — all locales enabled.
static constexpr u32 kWowLocaleAll = 0xFFFFFFFF;

/// Version written into the shmem file.
static constexpr u8 kShmemVersion = 7;

// ============================================================================
// Helpers
// ============================================================================

/// Build a 30-byte archive entry header.
/// Layout per CASC reference: EKey(16 reversed) + encodedSize(4 LE) + flags(2) + checksum(8).
/// Key bytes are reversed (big-endian key → little-endian storage order) per
/// cas::MakeFileHeader convention.
/// The checksum is MD5(header[0:22] + LE32(archiveOffset)), first 8 bytes.
static std::array<u8, kArchiveEntryHeaderSize> makeArchiveEntryHeader(
    const std::array<u8, 16>& eKey, u32 encodedSize, u32 archiveOffset) {
    std::array<u8, kArchiveEntryHeaderSize> hdr{};
    // Key bytes stored in reversed order per MakeFileHeader.
    for (int i = 0; i < 16; ++i)
        hdr[i] = eKey[15 - i];
    writeLE32(hdr.data() + 16, encodedSize);
    // Bytes 20–21: flags (channel byte + reserved) — zero for data channel.
    hdr[20] = 0;
    hdr[21] = 0;
    // Bytes 22–29: header checksum = first 8 bytes of MD5(header[0:22] + LE32(offset)).
    storages::common::MD5 hasher;
    hasher.update(hdr.data(), 22);
    u8 offsetLE[4];
    writeLE32(offsetLE, archiveOffset);
    hasher.update(offsetLE, 4);
    auto hash = hasher.finalize();
    std::memcpy(hdr.data() + 22, hash.data(), 8);
    return hdr;
}

/// Generate `.build.info` content.
static std::string generateBuildInfo(const std::array<u8, 16>& buildKey,
                                     const std::array<u8, 16>& cdnKey,
                                     const std::string& product,
                                     const std::string& version) {
    std::string content;
    content += "Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|"
               "CDN Key!HEX:16|Version!STRING:0|Product!STRING:0\n";
    // Single build row.
    content += "master|1|" + hexEncode16(buildKey) + "|" + hexEncode16(cdnKey) +
               "|" + version + "|" + product + "\n";
    return content;
}

/// Generate a build config file in key=value format.
static std::string generateBuildConfig(const std::array<u8, 16>& rootCKey,
                                       const std::array<u8, 16>& encodingCKey,
                                       const std::array<u8, 16>& encodingEKey,
                                       u64 encodingDecodedSize,
                                       u64 encodingEncodedSize,
                                       const std::array<u8, 16>& downloadCKey,
                                       const std::array<u8, 16>& downloadEKey,
                                       u64 downloadDecodedSize,
                                       u64 downloadEncodedSize,
                                       const std::string& product,
                                       const std::string& version) {
    std::string content;
    content += "# Build Configuration\n";
    content += "root = " + hexEncode16(rootCKey) + "\n";
    content += "encoding = " + hexEncode16(encodingCKey) + " " + hexEncode16(encodingEKey) + "\n";
    content += "encoding-size = " + std::to_string(encodingDecodedSize) + " " +
               std::to_string(encodingEncodedSize) + "\n";
    content += "download = " + hexEncode16(downloadCKey) + " " + hexEncode16(downloadEKey) + "\n";
    content += "download-size = " + std::to_string(downloadDecodedSize) + " " +
               std::to_string(downloadEncodedSize) + "\n";
    content += "build-name = " + version + "\n";
    content += "build-product = " + product + "\n";
    content += "build-uid = " + product + "\n";
    return content;
}

/// Generate a CDN config file (for local storages, minimal).
static std::string generateCdnConfig(const std::vector<std::array<u8, 16>>& archiveEKeys) {
    std::string content;
    content += "# CDN Configuration\n";
    content += "archives = ";
    for (size_t i = 0; i < archiveEKeys.size(); ++i) {
        if (i > 0) content += " ";
        content += hexEncode16(archiveEKeys[i]);
    }
    content += "\n";
    return content;
}

/// Write bytes to a file, creating directories as needed.
static bool writeFileBytes(const std::string& path, const void* data, size_t size) {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(path).parent_path());
    auto ofs = whiteout::common::open_ofstream(path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return ofs.good();
}

static bool writeFileString(const std::string& path, const std::string& content) {
    return writeFileBytes(path, content.data(), content.size());
}

// ============================================================================
// ESpec helper
// ============================================================================

/// Build an ESpec string describing the BLTE encoding recipe for a file.
/// The result matches the wowdev.wiki BLTE ESpec grammar:
///   single-frame compressed  → "z"
///   single-frame raw         → "n"
///   multi-frame compressed   → "b:{frameSize*=z}"
///   multi-frame raw          → "b:{frameSize*=n}"
static std::string buildESpec(bool compress, u64 fileSize, u32 frameSize) {
    if (fileSize == 0 || fileSize <= frameSize) {
        return compress ? "z" : "n";
    }
    // Multi-frame block encoding.
    if (compress)
        return "b:{" + std::to_string(frameSize) + "*=z}";
    else
        return "b:{" + std::to_string(frameSize) + "*=n}";
}

/// Compute variable-width offset field size for a given table size.
/// Mirrors getCftOffsSize() in the TVFS parser.
static u32 getOffsFieldSize(u32 tableSize) {
    if (tableSize <= 0xFF)     return 1;
    if (tableSize <= 0xFFFF)   return 2;
    if (tableSize <= 0xFFFFFF) return 3;
    return 4;
}

// ============================================================================
// TVFS Root Serialization
// ============================================================================

/// Build a TVFS root manifest from entries with INCLUDE_CKEY and WRITE_SUPPORT.
/// This produces a flat TVFS blob (no prefix compression for simplicity).
/// Includes CKey in CFT entries and an EST (Encoding Specifier Table) that
/// records the BLTE ESpec string for each file.
static std::vector<u8> serializeTvfsRoot(const std::vector<WriteEntry>& entries,
                                          u32 blteFrameSize) {
    // TVFS root format matching the real parser:
    //   Header (46 bytes): mixed LE/BE fields
    //   Path table: prefix-trie encoded (we use flat single-level)
    //   VFS table: span entries mapping paths → CFT regions
    //   CFT table: EKey(eKeySize) + CKey(eKeySize) per file entry
    //   EST table: ESpec string pool + per-entry offset indices

    constexpr u8 kFormatVersion = kTvfsFormatVersion;
    constexpr u8 kEKeySizeTvfs = 9;
    constexpr u8 kPatchKeySize = 9;
    // INCLUDE_CKEY (0x0001): store CKey alongside EKey in CFT entries.
    // WRITE_SUPPORT (0x0002): EST table present with ESpec encoding info.
    constexpr u32 kFlags = 0x0003;

    // --- Build CFT (Content-File Table) ---
    // Each entry: EKey(eKeySize) + CKey(eKeySize).
    constexpr u32 kCftEntrySize = kEKeySizeTvfs * 2;
    std::vector<u8> cft;
    cft.reserve(entries.size() * kCftEntrySize);
    for (auto& e : entries) {
        for (u32 i = 0; i < kEKeySizeTvfs; ++i)
            cft.push_back(e.eKey[i]);
        for (u32 i = 0; i < kEKeySizeTvfs; ++i)
            cft.push_back(e.cKey[i]);
    }

    // Determine CFT offset field size (for VFS table entries).
    u32 cftOffsSize = getOffsFieldSize(static_cast<u32>(cft.size()));

    // --- Build EST (Encoding Specifier Table) ---
    // Layout: [string pool: null-terminated ESpec strings] +
    //         [per-CFT-entry offset into string pool (estOffsSize bytes, BE)].
    //
    // The string pool contains unique ESpec strings. Each CFT entry has a
    // variable-width BE offset into this pool identifying its ESpec.
    // estOffsSize is computed from the total EST table size, matching
    // CascLib's GetOffsetFieldSize(EstTableSize) convention.

    // Collect unique ESpec strings and assign pool offsets.
    std::vector<std::string> uniqueESpecs;
    std::vector<u32> entryPoolOffsets;
    entryPoolOffsets.reserve(entries.size());
    {
        // Map ESpec string → byte offset in the string pool.
        struct SpecEntry { u32 poolOffset; };
        std::vector<std::pair<std::string, SpecEntry>> specMap;

        u32 poolPos = 0;
        for (auto& e : entries) {
            std::string spec = buildESpec(e.compress, e.fileSize, blteFrameSize);

            // Linear search is fine — typically 1-3 unique strings.
            u32 offset = UINT32_MAX;
            for (auto& [s, se] : specMap) {
                if (s == spec) { offset = se.poolOffset; break; }
            }
            if (offset == UINT32_MAX) {
                offset = poolPos;
                specMap.push_back({spec, {poolPos}});
                uniqueESpecs.push_back(spec);
                poolPos += static_cast<u32>(spec.size() + 1); // +1 for '\0'
            }
            entryPoolOffsets.push_back(offset);
        }
    }

    // Build string pool blob.
    std::vector<u8> estStringPool;
    for (auto& s : uniqueESpecs) {
        estStringPool.insert(estStringPool.end(), s.begin(), s.end());
        estStringPool.push_back(0); // null terminator
    }

    // Determine estOffsSize iteratively: estOffsSize must be consistent with
    // total EST table size (estTableSize = poolSize + numEntries * estOffsSize).
    u32 poolSize = static_cast<u32>(estStringPool.size());
    u32 numEntries = static_cast<u32>(entries.size());
    u32 estOffsSize = 1;
    for (int iter = 0; iter < 4; ++iter) {
        u32 total = poolSize + numEntries * estOffsSize;
        u32 needed = getOffsFieldSize(total);
        if (needed <= estOffsSize) break;
        estOffsSize = needed;
    }
    u32 estTableSize = poolSize + numEntries * estOffsSize;

    // Build complete EST blob: string pool + offset index.
    std::vector<u8> est;
    est.reserve(estTableSize);
    est.insert(est.end(), estStringPool.begin(), estStringPool.end());
    for (auto offset : entryPoolOffsets) {
        for (int b = static_cast<int>(estOffsSize) - 1; b >= 0; --b)
            est.push_back(static_cast<u8>((offset >> (b * 8)) & 0xFF));
    }

    // --- Build VFS table ---
    // Each file gets one VFS entry: spanCount(1) + FileOffset(4 BE) + SpanSize(4 BE) + CftOffset(var BE).
    u32 vfsEntrySize = 1 + 4 + 4 + cftOffsSize;
    std::vector<u8> vfs;
    vfs.reserve(entries.size() * vfsEntrySize);
    for (size_t i = 0; i < entries.size(); ++i) {
        u32 cftOff = static_cast<u32>(i * kCftEntrySize);
        vfs.push_back(1); // spanCount = 1
        pushBE32(vfs, 0); // fileOffset = 0 (not used for read)
        pushBE32(vfs, static_cast<u32>(entries[i].fileSize)); // spanSize
        // CftOffset (variable-length BE).
        for (int b = cftOffsSize - 1; b >= 0; --b)
            vfs.push_back(static_cast<u8>((cftOff >> (b * 8)) & 0xFF));
    }

    // --- Build path table ---
    // Simplest valid structure: anonymous root folder containing all file nodes,
    // each with the full path as its name.

    // Build inner path table content (file nodes within root folder).
    std::vector<u8> innerPathTable;
    u32 vfsOffset = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& path = entries[i].path;
        // Write name as single fragment: [len][bytes...]
        // Paths can be >255 chars, but for simplicity limit to 255.
        u8 nameLen = static_cast<u8>(std::min<size_t>(path.size(), 255));
        innerPathTable.push_back(nameLen);
        for (u8 j = 0; j < nameLen; ++j)
            innerPathTable.push_back(static_cast<u8>(path[j]));

        // Node value marker (no path separators — CascLib interprets 0x00
        // as '/' prefix/postfix on the resolved path, which would corrupt
        // the filename).
        innerPathTable.push_back(kTvfsNodeValueMarker);

        // File node value: VFS offset (bit 31 = 0).
        pushBE32(innerPathTable, vfsOffset);

        vfsOffset += vfsEntrySize;
    }

    // Wrap in anonymous root folder:
    //   0xFF + nodeValue(u32 BE, bit31=1, lower 31 = folderDataLen)
    //   where folderDataLen = 4 (for the nodeValue itself) + innerLen.
    u32 innerLen = static_cast<u32>(innerPathTable.size());
    u32 folderDataLen = 4 + innerLen; // includes the 4-byte nodeValue
    std::vector<u8> pathTable;
    pathTable.push_back(kTvfsNodeValueMarker);
    pushBE32(pathTable, kTvfsFolderNodeBit | folderDataLen);
    pathTable.insert(pathTable.end(), innerPathTable.begin(), innerPathTable.end());

    // --- Compute section offsets ---
    u32 pathTableOffset = kTvfsHeaderSize;
    u32 pathTableSize = static_cast<u32>(pathTable.size());
    u32 vfsTableOffset = pathTableOffset + pathTableSize;
    u32 vfsTableSize = static_cast<u32>(vfs.size());
    u32 cftTableOffset = vfsTableOffset + vfsTableSize;
    u32 cftTableSize = static_cast<u32>(cft.size());
    u32 estTableOffset = cftTableOffset + cftTableSize;

    // --- Build header ---
    std::vector<u8> result;
    result.reserve(kTvfsHeaderSize + pathTableSize + vfsTableSize + cftTableSize + estTableSize);

    pushLE32(result, RootSignature::kTVFS); // offset 0: magic (LE)
    result.push_back(kFormatVersion);       // offset 4: formatVersion (u8)
    result.push_back(static_cast<u8>(kTvfsHeaderSize)); // offset 5: headerSize (u8)
    result.push_back(kEKeySizeTvfs);        // offset 6: eKeySize (u8)
    result.push_back(kPatchKeySize);        // offset 7: patchKeySize (u8)
    pushLE32(result, kFlags);               // offset 8: flags (LE)
    pushBE32(result, pathTableOffset);      // offset 12: pathTableOffset (BE)
    pushBE32(result, pathTableSize);        // offset 16: pathTableSize (BE)
    pushBE32(result, vfsTableOffset);       // offset 20: vfsTableOffset (BE)
    pushBE32(result, vfsTableSize);         // offset 24: vfsTableSize (BE)
    pushBE32(result, cftTableOffset);       // offset 28: cftTableOffset (BE)
    pushBE32(result, cftTableSize);         // offset 32: cftTableSize (BE)
    pushBE16(result, 1);                    // offset 36: maxDepth (BE)
    pushBE32(result, estTableOffset);       // offset 38: estTableOffset (BE)
    pushBE32(result, estTableSize);         // offset 42: estTableSize (BE)

    // --- Append sections ---
    result.insert(result.end(), pathTable.begin(), pathTable.end());
    result.insert(result.end(), vfs.begin(), vfs.end());
    result.insert(result.end(), cft.begin(), cft.end());
    result.insert(result.end(), est.begin(), est.end());

    return result;
}

// ============================================================================
// D3 Root Serialization (two-level hierarchy)
// ============================================================================

/// Result of D3 root serialization.  The D3 root format uses a two-level
/// hierarchy: the root blob (kD3Root) references a subdirectory blob (kD3Dir)
/// that actually contains the file entries.  Both blobs must be stored in the
/// CASC archive and encoding table.
struct D3RootBlobs {
    std::vector<u8> rootData;    ///< Root blob (kD3Root + one "Base" named entry).
    std::vector<u8> subdirData;  ///< Subdirectory blob (kD3Dir + named entries for files).
    std::array<u8, 16> subdirCKey{}; ///< Content key of subdirData.
};

static D3RootBlobs serializeD3Root(const std::vector<WriteEntry>& entries) {
    D3RootBlobs result;

    // Partition entries into asset entries (have a fileIndex / fileDataId)
    // and named entries (path-only, no SNO fileIndex).
    // AssetIdx entries are not distinguishable from asset entries in WriteEntry
    // (subIndex is not preserved), so they are written as 0 assetIdx entries;
    // entries that originally were assetIdx are emitted as regular asset
    // entries here, which is acceptable for re-serialization.
    std::vector<size_t> assetIndices, namedIndices;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].fileDataId != 0xFFFFFFFF)
            assetIndices.push_back(i);
        else
            namedIndices.push_back(i);
    }


    // Step 1: Build the "Base" subdirectory blob (kD3Dir).
    // Format:
    //   [u32 LE: kD3Dir signature]
    //   [u32 LE: assetCount]
    //   For each asset entry: [CKey(16)] [fileIndex(4)]
    //   [u32 LE: assetIdxCount = 0]
    //   [u32 LE: namedCount]
    //   For each named entry: [CKey(16)] [null-terminated path string]
    auto& subdir = result.subdirData;
    pushLE32(subdir, RootSignature::kD3Dir);

    // Asset entries.
    pushLE32(subdir, static_cast<u32>(assetIndices.size()));
    for (auto idx : assetIndices) {
        subdir.insert(subdir.end(), entries[idx].cKey.begin(), entries[idx].cKey.end());
        pushLE32(subdir, entries[idx].fileDataId);
    }

    // AssetIdx entries (not preserved — subIndex lost during enumeration).
    pushLE32(subdir, 0);

    // Named entries.
    pushLE32(subdir, static_cast<u32>(namedIndices.size()));
    for (auto idx : namedIndices) {
        subdir.insert(subdir.end(), entries[idx].cKey.begin(), entries[idx].cKey.end());
        subdir.insert(subdir.end(), entries[idx].path.begin(), entries[idx].path.end());
        subdir.push_back(0); // null terminator
    }

    result.subdirCKey = storages::common::md5Hash(subdir);

    // Step 2: Build the root blob (kD3Root) referencing the subdirectory.
    // Use an empty name for the subdirectory so the parser doesn't prepend
    // a prefix to the file paths (prefix = "" when name is empty).
    //
    // CascLib rejects root blobs <= 32 bytes (MD5_STRING_SIZE) as a heuristic
    // to skip ROOT files that are plain MD5 hash strings.  A single named
    // entry with an empty name produces only 25 bytes, so we emit a second
    // dummy entry (all-zero CKey, empty name).  CascLib's D3 root handler
    // calls FindCKeyEntry_CKey for each named entry and silently skips
    // entries whose CKey is not found in the encoding table.
    //
    // Format:
    //   [u32 LE: kD3Root signature]
    //   [u32 LE: namedCount = 2]
    //   [CKey(16): subdirectory content key] [null-terminated string: ""]
    //   [CKey(16): 0x00...00 (dummy)]        [null-terminated string: ""]
    auto& root = result.rootData;
    pushLE32(root, RootSignature::kD3Root);
    pushLE32(root, 2); // two subdirectory entries (real + dummy).
    // Real subdirectory entry.
    root.insert(root.end(), result.subdirCKey.begin(), result.subdirCKey.end());
    root.push_back(0); // empty string + null terminator
    // Dummy entry — CKey not in encoding table, so CascLib skips it.
    root.insert(root.end(), 16, 0); // all-zero CKey.
    root.push_back(0); // empty string + null terminator

    return result;
}

// ============================================================================
// WoW Root Serialization (minimal MFST header + flat entries)
// ============================================================================

static std::vector<u8> serializeWowRoot(const std::vector<WriteEntry>& entries) {
    // MFST root format (v1 header):
    //   magic(4) + totalFileCount(4) + namedFileCount(4)
    // Followed by one locale block:
    //   Block header: numRecords(4) + contentFlags(4) + localeFlags(4)
    //   Delta-encoded fileDataIds (i32[numRecords])
    //   CKeys (16 * numRecords bytes)
    //   NameHashes (8 * numRecords bytes) — if paths available

    std::vector<u8> result;

    // Count entries that have paths or preserved hashes (for name hashes).
    u32 namedCount = 0;
    for (auto& e : entries)
        if (!e.path.empty() || e.fileNameHash != 0) ++namedCount;

    pushLE32(result, RootSignature::kMFST);
    pushLE32(result, static_cast<u32>(entries.size())); // total file count.
    pushLE32(result, namedCount);                       // named file count.

    if (!entries.empty()) {
        const bool writingNameHashes = (namedCount > 0);

        // Block header.
        pushLE32(result, static_cast<u32>(entries.size())); // numRecords.
        pushLE32(result, writingNameHashes ? 0u : ContentFlags::NoNameHash); // contentFlags.
        pushLE32(result, kWowLocaleAll);                    // localeFlags.

        // Sort by fileDataId for delta encoding.
        std::vector<size_t> order(entries.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return entries[a].fileDataId < entries[b].fileDataId;
        });

        // Delta-encoded fileDataIds.
        // Parser decodes: fdi += delta; ids[i] = fdi; fdi++ (implicit +1).
        u32 prevId = 0;
        for (auto idx : order) {
            u32 id = entries[idx].fileDataId;
            pushLE32(result, id - prevId);
            prevId = id + 1; // account for implicit +1 in parser.
        }

        // CKeys.
        for (auto idx : order) {
            result.insert(result.end(), entries[idx].cKey.begin(), entries[idx].cKey.end());
        }

        // Name hashes (Jenkins).
        if (writingNameHashes) {
            for (auto idx : order) {
                u64 combined = 0;
                if (!entries[idx].path.empty()) {
                    auto h = storages::common::jenkinsHash(entries[idx].path);
                    combined = u64(h.pc) | (u64(h.pb) << 32);
                } else if (entries[idx].fileNameHash != 0) {
                    // Preserve original Jenkins hash from WoW root entries that
                    // have no path string (hash-only lookup).
                    combined = entries[idx].fileNameHash;
                }
                for (int b = 0; b < 8; ++b)
                    result.push_back(static_cast<u8>(combined >> (b * 8)));
            }
        }
    }

    return result;
}

// ============================================================================
// Overwatch Root Serialization (text CSV)
// ============================================================================

/// Build an Overwatch-style text root manifest from entries.
/// CascLib parses this as pipe-delimited CSV with `#`-prefixed header.
/// Columns: FILEID|MD5|CHUNK_ID|PRIORITY|MPRIORITY|FILENAME|INSTALLPATH
/// CascLib uses the FILENAME and MD5 columns to insert entries into its tree.
static std::vector<u8> serializeOwRoot(const std::vector<WriteEntry>& entries) {
    static constexpr char kHexUpper[] = "0123456789ABCDEF";

    std::string csv;
    csv += "#FILEID|MD5|CHUNK_ID|PRIORITY|MPRIORITY|FILENAME|INSTALLPATH\n";

    for (auto& e : entries) {
        if (e.path.empty()) continue;

        // Encode CKey as uppercase hex (32 chars).
        std::string ckeyHex;
        ckeyHex.reserve(32);
        for (auto b : e.cKey) {
            ckeyHex.push_back(kHexUpper[b >> 4]);
            ckeyHex.push_back(kHexUpper[b & 0xF]);
        }

        // Extract basename from path.
        std::string basename = e.path;
        auto pos = basename.find_last_of("/\\");
        if (pos != std::string::npos)
            basename = basename.substr(pos + 1);

        // FILEID|MD5|CHUNK_ID|PRIORITY|MPRIORITY|FILENAME|INSTALLPATH
        csv += e.path;
        csv += '|';
        csv += ckeyHex;
        csv += "|0|0|255|";
        csv += e.path;
        csv += '|';
        csv += basename;
        csv += '\n';
    }

    return std::vector<u8>(csv.begin(), csv.end());
}

// ============================================================================
// writeStorage
// ============================================================================

bool writeStorage(const std::string& outputDir,
                  std::vector<WriteEntry>& entries,
                  const WriterOptions& opts,
                  interfaces::WorkerPool* pool) {
    namespace fs = std::filesystem;

    // Create output directory structure.
    std::string dataDir = outputDir + "/Data";
    std::string dataSubdir = dataDir + "/data";
    std::string configDir = dataDir + "/config";
    fs::create_directories(dataSubdir);
    fs::create_directories(configDir);

    // -----------------------------------------------------------------------
    // Step 1: Compute CKeys and BLTE-encode new files.
    // -----------------------------------------------------------------------

    // Collect indices of entries that need encoding.
    std::vector<size_t> toEncode;
    toEncode.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].hasPreEncoded)
            toEncode.push_back(i);
    }

    auto encodeEntry = [&](size_t idx) {
        auto& entry = entries[idx];
        // Compute CKey = MD5(rawData).
        entry.cKey = storages::common::md5Hash(entry.rawData);
        entry.fileSize = entry.rawData.size();

        // BLTE-encode (single-entry, no inner parallelism to avoid over-subscription).
        BlteEncodeOptions entryOpts;
        entryOpts.frameSize = opts.blteFrameSize;
        entryOpts.compress = entry.compress;
        entry.encodedBlob = blteEncode(entry.rawData, entryOpts, nullptr);

        // Compute EKey = MD5(encodedBlob).
        entry.eKey = storages::common::md5Hash(entry.encodedBlob);
    };

    if (pool && toEncode.size() > 1) {
        utils::JobGroup jobGroup;
        jobGroup.add(toEncode.size());
        for (size_t idx : toEncode) {
            interfaces::WorkerTask task;
            task.fn = [&, idx]() {
                encodeEntry(idx);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    } else {
        for (size_t idx : toEncode)
            encodeEntry(idx);
    }

    // -----------------------------------------------------------------------
    // Step 2: Build root manifest (before encoding, since root entry goes into encoding table).
    // -----------------------------------------------------------------------

    // D3 root produces an extra subdirectory blob that must also go into the
    // encoding table and archives.
    std::vector<u8> d3SubdirBlte;
    std::array<u8, 16> d3SubdirEKey{};
    std::vector<u8> d3SubdirRaw;

    std::vector<u8> rootRaw;
    switch (opts.rootFormat) {
        case RootFormat::Tvfs:    rootRaw = serializeTvfsRoot(entries, opts.blteFrameSize); break;
        case RootFormat::Diablo3: {
            auto d3 = serializeD3Root(entries);
            rootRaw = std::move(d3.rootData);
            d3SubdirRaw = std::move(d3.subdirData);
            break;
        }
        case RootFormat::Wow:       rootRaw = serializeWowRoot(entries);  break;
        case RootFormat::Overwatch: rootRaw = serializeOwRoot(entries);  break;
        default:                    rootRaw = serializeTvfsRoot(entries, opts.blteFrameSize); break;
    }

    auto rootCKey = storages::common::md5Hash(rootRaw);

    BlteEncodeOptions blteOpts;
    blteOpts.frameSize = opts.blteFrameSize;
    blteOpts.compress = true;
    auto rootBlte = blteEncode(rootRaw, blteOpts, pool);
    auto rootEKey = storages::common::md5Hash(rootBlte);

    // BLTE-encode D3 subdirectory blob if present.
    if (!d3SubdirRaw.empty()) {
        auto d3SubdirCKey = storages::common::md5Hash(d3SubdirRaw);
        d3SubdirBlte = blteEncode(d3SubdirRaw, blteOpts, pool);
        d3SubdirEKey = storages::common::md5Hash(d3SubdirBlte);
    }

    // -----------------------------------------------------------------------
    // Step 3: Build encoding table (includes file entries + root entry).
    // The encoding table itself is NOT in the encoding table — it's found
    // via build config's encodingEKey directly in the index.
    // -----------------------------------------------------------------------

    EncodingTable encodingTable;
    for (auto& entry : entries) {
        EncodingEntry ee;
        ee.cKey = entry.cKey;
        ee.eKey = entry.eKey;
        ee.fileSize = entry.fileSize;
        ee.eSpec = buildESpec(entry.compress, entry.fileSize, opts.blteFrameSize);
        encodingTable.insert(ee);
    }

    // Add D3 subdirectory blob to encoding table (resolver must find it by CKey).
    if (!d3SubdirRaw.empty()) {
        EncodingEntry subdirEnc;
        subdirEnc.cKey = storages::common::md5Hash(d3SubdirRaw);
        subdirEnc.eKey = d3SubdirEKey;
        subdirEnc.fileSize = d3SubdirRaw.size();
        subdirEnc.eSpec = buildESpec(true, d3SubdirRaw.size(), opts.blteFrameSize);
        encodingTable.insert(subdirEnc);
    }

    // Add root to encoding table so resolveCKey(rootCKey) works.
    {
        EncodingEntry rootEnc;
        rootEnc.cKey = rootCKey;
        rootEnc.eKey = rootEKey;
        rootEnc.fileSize = rootRaw.size();
        rootEnc.eSpec = buildESpec(true, rootRaw.size(), opts.blteFrameSize);
        encodingTable.insert(rootEnc);
    }

    // Build a minimal DOWNLOAD manifest (required by CascLib).
    // FILE_DOWNLOAD_HEADER: 'DL' + version 1 + EKeyLen 16 + no checksum + 0 entries + 0 tags.
    std::vector<u8> downloadRaw(16, 0);
    downloadRaw[0] = 'D';
    downloadRaw[1] = 'L';
    downloadRaw[2] = 1;   // Version
    downloadRaw[3] = 16;  // EKeyLength
    // Bytes 4-15: zero (EntryHasChecksum=0, EntryCount=0, TagCount=0, padding)

    auto downloadCKey = storages::common::md5Hash(downloadRaw);
    auto downloadBlte = blteEncode(downloadRaw, blteOpts, pool);
    auto downloadEKey = storages::common::md5Hash(downloadBlte);
    u64 downloadEncodedSize = downloadBlte.size();

    // Add download manifest to encoding table.
    {
        EncodingEntry dlEnc;
        dlEnc.cKey = downloadCKey;
        dlEnc.eKey = downloadEKey;
        dlEnc.fileSize = downloadRaw.size();
        dlEnc.eSpec = buildESpec(true, downloadRaw.size(), opts.blteFrameSize);
        encodingTable.insert(dlEnc);
    }

    // Serialize encoding table → BLTE-encode → get encoding CKey/EKey.
    auto encodingRaw = encodingTable.serialize();
    auto encodingCKey = storages::common::md5Hash(encodingRaw);

    auto encodingBlte = blteEncode(encodingRaw, blteOpts, pool);
    auto encodingEKey = storages::common::md5Hash(encodingBlte);
    u64 encodingEncodedSize = encodingBlte.size();

    // -----------------------------------------------------------------------
    // Step 4: Write archive files (data.000, data.001, ...).
    // -----------------------------------------------------------------------

    struct ArchiveBlob {
        std::array<u8, 16> eKey;
        std::vector<u8> encodedData;
    };

    // Gather all blobs to write: file entries + root + encoding.
    std::vector<ArchiveBlob> blobs;
    blobs.reserve(entries.size() + 2);

    for (auto& entry : entries) {
        ArchiveBlob ab;
        ab.eKey = entry.eKey;
        ab.encodedData = std::move(entry.encodedBlob);
        blobs.push_back(std::move(ab));
    }

    // Root blob.
    blobs.push_back({rootEKey, std::move(rootBlte)});
    // D3 subdirectory blob (if present).
    if (!d3SubdirBlte.empty()) {
        blobs.push_back({d3SubdirEKey, std::move(d3SubdirBlte)});
    }
    // Download manifest blob.
    blobs.push_back({downloadEKey, std::move(downloadBlte)});
    // Encoding blob.
    blobs.push_back({encodingEKey, std::move(encodingBlte)});

    // Split into archives.
    IndexTable indexTable;
    std::vector<std::array<u8, 16>> archiveEKeys; // One hash per archive file.
    u32 archiveIndex = 0;
    u32 archiveOffset = 0;
    std::vector<u8> currentArchive;
    currentArchive.reserve(opts.archiveMaxSize);

    storages::common::MD5 archiveHasher;

    auto flushArchive = [&]() {
        if (currentArchive.empty()) return;

        char archiveName[32];
        std::snprintf(archiveName, sizeof(archiveName), "data.%03u", archiveIndex);
        std::string archivePath = dataSubdir + "/" + archiveName;
        writeFileBytes(archivePath, currentArchive.data(), currentArchive.size());

        archiveEKeys.push_back(archiveHasher.finalize());

        currentArchive.clear();
        archiveHasher = storages::common::MD5();
        archiveIndex++;
        archiveOffset = 0;
    };

    for (auto& blob : blobs) {
        u32 encodedSize = static_cast<u32>(blob.encodedData.size()) + kArchiveEntryHeaderSize;

        // Check if this blob would exceed the archive max size.
        if (!currentArchive.empty() &&
            (currentArchive.size() + encodedSize) > opts.archiveMaxSize) {
            flushArchive();
        }

        // Record index entry.
        IndexEntry ie;
        ie.eKey = blob.eKey;
        ie.archiveIndex = archiveIndex;
        ie.archiveOffset = archiveOffset;
        ie.encodedSize = encodedSize;
        indexTable.insert(ie);

        // Write archive entry header + data.
        auto hdr = makeArchiveEntryHeader(blob.eKey, static_cast<u32>(blob.encodedData.size()),
                                          archiveOffset);
        currentArchive.insert(currentArchive.end(), hdr.begin(), hdr.end());
        currentArchive.insert(currentArchive.end(), blob.encodedData.begin(), blob.encodedData.end());

        archiveHasher.update(std::span<const u8>(hdr.data(), hdr.size()));
        archiveHasher.update(std::span<const u8>(blob.encodedData.data(), blob.encodedData.size()));

        archiveOffset += encodedSize;
    }

    // Flush the last archive.
    flushArchive();

    // -----------------------------------------------------------------------
    // Step 5: Write index files.
    // -----------------------------------------------------------------------

    auto idxFiles = indexTable.serialize();
    for (auto& [name, data] : idxFiles) {
        std::string idxPath = dataSubdir + "/" + name;
        if (!writeFileBytes(idxPath, data.data(), data.size()))
            return false;
    }

    // -----------------------------------------------------------------------
    // Step 6: Write config files.
    // -----------------------------------------------------------------------

    // Build config.
    auto buildConfigStr = generateBuildConfig(rootCKey, encodingCKey, encodingEKey,
                                              encodingRaw.size(), encodingEncodedSize,
                                              downloadCKey, downloadEKey,
                                              downloadRaw.size(), downloadEncodedSize,
                                              opts.product, opts.version);
    std::vector<u8> buildConfigBytes(buildConfigStr.begin(), buildConfigStr.end());
    auto buildKey = storages::common::md5Hash(buildConfigBytes);

    // Config path: config/XX/YY/<hash>
    if (!writeFileString(configFilePath(dataDir, buildKey), buildConfigStr))
        return false;

    // CDN config (minimal).
    auto cdnConfigStr = generateCdnConfig(archiveEKeys);
    std::vector<u8> cdnConfigBytes(cdnConfigStr.begin(), cdnConfigStr.end());
    auto cdnKey = storages::common::md5Hash(cdnConfigBytes);

    if (!writeFileString(configFilePath(dataDir, cdnKey), cdnConfigStr))
        return false;

    // -----------------------------------------------------------------------
    // Step 7: Write .build.info.
    // -----------------------------------------------------------------------

    auto buildInfoStr = generateBuildInfo(buildKey, cdnKey, opts.product, opts.version);
    if (!writeFileString(outputDir + "/.build.info", buildInfoStr))
        return false;

    // -----------------------------------------------------------------------
    // Step 8: Write shmem file.
    // -----------------------------------------------------------------------

    // shmem: simple binary file with archive count.
    std::vector<u8> shmem(8, 0);
    shmem[0] = static_cast<u8>(archiveIndex & 0xFF);
    shmem[1] = static_cast<u8>((archiveIndex >> 8) & 0xFF);
    shmem[2] = static_cast<u8>((archiveIndex >> 16) & 0xFF);
    shmem[3] = static_cast<u8>((archiveIndex >> 24) & 0xFF);
    shmem[4] = kShmemVersion;
    writeFileBytes(dataDir + "/shmem", shmem.data(), shmem.size());

    return true;
}

} // namespace whiteout::storages::casc
