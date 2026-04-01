// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "writer.h"
#include "blte.h"
#include "config.h"
#include "constants.h"
#include "encoding.h"
#include "index.h"
#include "roots/root.h"
#include "../common/byte_order.h"
#include "../common/hex.h"
#include "../common/md5.h"

#include <whiteout/common_types.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>

namespace whiteout::storages::casc {

using storages::common::writeBE32;
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
/// Layout: EKey hash (16 bytes) + encoded size (u32 BE) + decoded size (u32 BE)
///       + checksum bytes (6 bytes, zeroed for simplicity).
static std::array<u8, kArchiveEntryHeaderSize> makeArchiveEntryHeader(
    const std::array<u8, 16>& eKey, u32 encodedSize, u32 decodedSize) {
    std::array<u8, kArchiveEntryHeaderSize> hdr{};
    std::memcpy(hdr.data(), eKey.data(), 16);
    writeBE32(hdr.data() + 16, encodedSize);
    writeBE32(hdr.data() + 20, decodedSize);
    // Bytes 24–29: reserved/checksum — zero-filled.
    return hdr;
}

/// Generate `.build.info` content.
static std::string generateBuildInfo(const std::array<u8, 16>& buildKey,
                                     const std::string& product,
                                     const std::string& version) {
    std::string content;
    content += "Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|"
               "CDN Key!HEX:16|Version!STRING:0|Product!STRING:0\n";
    // Single build row.
    content += "master|1|" + hexEncode16(buildKey) + "|" + hexEncode16(buildKey) +
               "|" + version + "|" + product + "\n";
    return content;
}

/// Generate a build config file in key=value format.
static std::string generateBuildConfig(const std::array<u8, 16>& rootCKey,
                                       const std::array<u8, 16>& encodingCKey,
                                       const std::array<u8, 16>& encodingEKey,
                                       u64 encodingSize,
                                       const std::string& product,
                                       const std::string& version) {
    std::string content;
    content += "# Build Configuration\n";
    content += "root = " + hexEncode16(rootCKey) + "\n";
    content += "encoding = " + hexEncode16(encodingCKey) + " " + hexEncode16(encodingEKey) + "\n";
    content += "encoding-size = " + std::to_string(encodingSize) + "\n";
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
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return ofs.good();
}

static bool writeFileString(const std::string& path, const std::string& content) {
    return writeFileBytes(path, content.data(), content.size());
}

// ============================================================================
// TVFS Root Serialization
// ============================================================================

/// Build a minimal TVFS root manifest from entries.
/// This produces a flat TVFS blob (no prefix compression for simplicity).
static std::vector<u8> serializeTvfsRoot(const std::vector<WriteEntry>& entries) {
    // TVFS root format matching the real parser:
    //   Header (46 bytes): mixed LE/BE fields
    //   Path table: prefix-trie encoded (we use flat single-level)
    //   VFS table: span entries mapping paths → CFT regions
    //   CFT table: EKey (eKeySize bytes) per file entry

    constexpr u8 kFormatVersion = kTvfsFormatVersion;
    constexpr u8 kEKeySizeTvfs = 9;
    constexpr u8 kPatchKeySize = 0;
    constexpr u32 kFlags = 0; // no CKey in CFT for simplicity

    // --- Build CFT (Content-File Table) ---
    // Each entry: EKey (kEKeySize bytes). No file size or CKey.
    constexpr u32 kCftEntrySize = kEKeySizeTvfs;
    std::vector<u8> cft;
    cft.reserve(entries.size() * kCftEntrySize);
    for (auto& e : entries) {
        for (u32 i = 0; i < kEKeySizeTvfs; ++i)
            cft.push_back(e.eKey[i]);
    }

    // Determine CFT offset field size (for VFS table entries).
    u32 cftOffsSize = 1;
    if (cft.size() > 0xFFFF) cftOffsSize = 4;
    else if (cft.size() > 0xFF) cftOffsSize = 2;

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
    // For each file: encode path as flat trie entry.
    // Format for each entry:
    //   [len(u8)][name_bytes...]   — name fragment with length prefix
    //   0x00                       — path boundary marker (present before 0xFF)
    //   0xFF                       — node value marker
    //   nodeValue (u32 BE)         — VFS offset (bit 31 = 0 → file node)
    //
    // For paths with directories like "dir/file.txt":
    //   We create nested folder nodes. For simplicity, emit full paths as single
    //   top-level folder entries.
    //
    // Actually: the simplest valid structure is an anonymous root folder containing
    // all file nodes. Each file node has the full path as its name.

    // Build inner path table content (file nodes within root folder).
    std::vector<u8> innerPathTable;
    u32 vfsOffset = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) innerPathTable.push_back(kTvfsPathSeparator); // sibling separator

        auto& path = entries[i].path;
        // Write name as single fragment: [len][bytes...]
        // Paths can be >255 chars, but for simplicity limit to 255.
        u8 nameLen = static_cast<u8>(std::min<size_t>(path.size(), 255));
        innerPathTable.push_back(nameLen);
        for (u8 j = 0; j < nameLen; ++j)
            innerPathTable.push_back(static_cast<u8>(path[j]));

        // Path boundary + node value marker.
        innerPathTable.push_back(kTvfsPathSeparator);
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

    // --- Build header ---
    std::vector<u8> result;
    result.reserve(kTvfsHeaderSize + pathTableSize + vfsTableSize + cftTableSize);

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
    pushBE32(result, 0);                    // offset 38: estTableOffset (BE)
    pushBE32(result, 0);                    // offset 42: estTableSize (BE)

    // --- Append sections ---
    result.insert(result.end(), pathTable.begin(), pathTable.end());
    result.insert(result.end(), vfs.begin(), vfs.end());
    result.insert(result.end(), cft.begin(), cft.end());

    return result;
}

// ============================================================================
// D3 Root Serialization (minimal)
// ============================================================================

static std::vector<u8> serializeD3Root(const std::vector<WriteEntry>& entries) {
    // D3 root is a flat directory format. Single directory block containing all entries.
    // Format: magic (u32 LE = 0x8007D0C4) + u32 field1 + u32 field2 + u32 entryCount
    //       + entries (each: CKey(16) + snoId(u32) + fileIndex(u32)).
    // For simplicity, assign sequential snoIds.

    std::vector<u8> result;

    pushLE32(result, RootSignature::kD3Root);
    pushLE32(result, 0); // field1 (locale or flags).
    pushLE32(result, 0); // field2.
    pushLE32(result, static_cast<u32>(entries.size()));

    for (size_t i = 0; i < entries.size(); ++i) {
        // CKey (16 bytes).
        result.insert(result.end(), entries[i].cKey.begin(), entries[i].cKey.end());
        // SNO ID (u32 LE) — use index as placeholder.
        pushLE32(result, static_cast<u32>(i));
        // File index (u32 LE).
        pushLE32(result, static_cast<u32>(i));
    }

    return result;
}

// ============================================================================
// WoW Root Serialization (minimal MFST header + flat entries)
// ============================================================================

static std::vector<u8> serializeWowRoot(const std::vector<WriteEntry>& entries) {
    // MFST root format: magic (4 bytes) + totalFileCount (u32) + namedFileCount (u32)
    // Then locale blocks: each is localeFlags(u32) + contentFlags(u32) + entryCount(u32)
    //   + fileDataIds (delta-encoded u32[]) + CKeys (16 bytes * entryCount).

    std::vector<u8> result;

    pushLE32(result, RootSignature::kMFST);
    pushLE32(result, static_cast<u32>(entries.size())); // total file count.
    pushLE32(result, 0); // named file count (we use FileDataId only).

    if (!entries.empty()) {
        // Single locale block with all entries.
        pushLE32(result, kWowLocaleAll); // locale flags (ALL).
        pushLE32(result, 0);          // content flags.
        // No further header fields for MFST v1. Entries follow:
        // FileDataId deltas + CKeys.

        // Sort by fileDataId for delta encoding.
        std::vector<size_t> order(entries.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return entries[a].fileDataId < entries[b].fileDataId;
        });

        // Delta-encoded fileDataIds.
        u32 prevId = 0;
        for (auto idx : order) {
            u32 id = entries[idx].fileDataId;
            u32 delta = id - prevId;
            pushLE32(result, delta);
            prevId = id;
        }

        // CKeys.
        for (auto idx : order) {
            result.insert(result.end(), entries[idx].cKey.begin(), entries[idx].cKey.end());
        }
    }

    return result;
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

    std::vector<u8> rootRaw;
    switch (opts.rootFormat) {
        case RootFormat::Tvfs:    rootRaw = serializeTvfsRoot(entries); break;
        case RootFormat::Diablo3: rootRaw = serializeD3Root(entries);   break;
        case RootFormat::Wow:     rootRaw = serializeWowRoot(entries);  break;
        default:                  rootRaw = serializeTvfsRoot(entries); break;
    }

    auto rootCKey = storages::common::md5Hash(rootRaw);

    BlteEncodeOptions blteOpts;
    blteOpts.frameSize = opts.blteFrameSize;
    blteOpts.compress = true;
    auto rootBlte = blteEncode(rootRaw, blteOpts, pool);
    auto rootEKey = storages::common::md5Hash(rootBlte);

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
        encodingTable.insert(ee);
    }

    // Add root to encoding table so resolveCKey(rootCKey) works.
    {
        EncodingEntry rootEnc;
        rootEnc.cKey = rootCKey;
        rootEnc.eKey = rootEKey;
        rootEnc.fileSize = rootRaw.size();
        encodingTable.insert(rootEnc);
    }

    // Serialize encoding table → BLTE-encode → get encoding CKey/EKey.
    auto encodingRaw = encodingTable.serialize();
    auto encodingCKey = storages::common::md5Hash(encodingRaw);

    auto encodingBlte = blteEncode(encodingRaw, blteOpts, pool);
    auto encodingEKey = storages::common::md5Hash(encodingBlte);

    // -----------------------------------------------------------------------
    // Step 4: Write archive files (data.000, data.001, ...).
    // -----------------------------------------------------------------------

    struct ArchiveBlob {
        std::array<u8, 16> eKey;
        std::vector<u8> encodedData;
        u32 decodedSize;
    };

    // Gather all blobs to write: file entries + root + encoding.
    std::vector<ArchiveBlob> blobs;
    blobs.reserve(entries.size() + 2);

    for (auto& entry : entries) {
        ArchiveBlob ab;
        ab.eKey = entry.eKey;
        ab.encodedData = std::move(entry.encodedBlob);
        ab.decodedSize = static_cast<u32>(entry.fileSize);
        blobs.push_back(std::move(ab));
    }

    // Root blob.
    blobs.push_back({rootEKey, std::move(rootBlte), static_cast<u32>(rootRaw.size())});
    // Encoding blob.
    blobs.push_back({encodingEKey, std::move(encodingBlte), static_cast<u32>(encodingRaw.size())});

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
                                          blob.decodedSize);
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
                                              encodingRaw.size(), opts.product, opts.version);
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

    auto buildInfoStr = generateBuildInfo(buildKey, opts.product, opts.version);
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
