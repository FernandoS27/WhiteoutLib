// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "header.h"

#include "../../common/binary_reader.h"
#include "../../common/streams.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace whiteout::storages::mpq {

using whiteout::common::BinaryReader;
using whiteout::common::span_streambuf;

// ============================================================================
// Utilities
// ============================================================================

u32 nextPowerOf2(u32 v) {
    if (v == 0)
        return 1;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

// ============================================================================
// Header Parsing
// ============================================================================

namespace {

u32 minimumHeaderSizeForVersion(u16 formatVersion) {
    switch (formatVersion) {
    case 0:
        return 32;
    case 1:
        return 44;
    case 2:
        return 68;
    case 3:
        return 208;
    default:
        return 0;
    }
}

/// Parse the V1-V4 header fields from a BinaryReader positioned at the start
/// of the magic field.
std::optional<MpqHeader> parseHeaderFields(BinaryReader& reader, size_t availableBytes) {
    if (availableBytes < 32)
        return std::nullopt;

    MpqHeader h{};
    h.magic = reader.read<u32>();
    if (h.magic != kMpqMagic)
        return std::nullopt;

    h.headerSize = reader.read<u32>();
    h.archiveSize = reader.read<u32>();
    h.formatVersion = reader.read<u16>();
    h.sectorSizeShift = reader.read<u16>();
    h.hashTableOffset = reader.read<u32>();
    h.blockTableOffset = reader.read<u32>();
    h.hashTableEntries = reader.read<u32>();
    h.blockTableEntries = reader.read<u32>();

    u32 minimumHeaderSize = minimumHeaderSizeForVersion(h.formatVersion);
    if (minimumHeaderSize == 0 || h.headerSize < minimumHeaderSize ||
        h.headerSize > availableBytes) {
        return std::nullopt;
    }

    // V2 fields
    if (h.formatVersion >= 1 && availableBytes >= 44) {
        h.hiBlockTableOffset = reader.read<u64>();
        h.hashTableOffsetHi = reader.read<u16>();
        h.blockTableOffsetHi = reader.read<u16>();
    }

    // V3 fields
    if (h.formatVersion >= 2 && availableBytes >= 68) {
        h.archiveSize64 = reader.read<u64>();
        h.betTableOffset = reader.read<u64>();
        h.hetTableOffset = reader.read<u64>();
    }

    // V4 fields
    if (h.formatVersion >= 3 && availableBytes >= 208) {
        h.hashTableSize64 = reader.read<u64>();
        h.blockTableSize64 = reader.read<u64>();
        h.hiBlockTableSize64 = reader.read<u64>();
        h.hetTableSize64 = reader.read<u64>();
        h.betTableSize64 = reader.read<u64>();
        h.rawChunkSize = reader.read<u32>();

        auto readMd5 = [&](std::array<u8, 16>& dest) {
            for (auto& b : dest)
                b = reader.read<u8>();
        };
        readMd5(h.blockTableMd5);
        readMd5(h.hashTableMd5);
        readMd5(h.hiBlockTableMd5);
        readMd5(h.betTableMd5);
        readMd5(h.hetTableMd5);
        readMd5(h.mpqHeaderMd5);
    }

    return h;
}

} // anonymous namespace

std::optional<HeaderParseResult> findAndParseHeader(std::span<const u8> fileData) {
    if (fileData.size() < 32)
        return std::nullopt;

    // Scan at 0x200-byte boundaries for the MPQ signature.
    // Also check offset 0 (some archives have the header right at the start).
    const size_t maxOffset = fileData.size() - 32;

    for (size_t offset = 0; offset <= maxOffset; offset += kHeaderSearchAlignment) {
        u32 magic = 0;
        std::memcpy(&magic, fileData.data() + offset, sizeof(u32));

        if (magic == kMpqUserDataMagic && offset == 0) {
            // User data block at offset 0 — parse it and find the real header.
            if (fileData.size() < offset + 16)
                continue;

            // Clamp ud-reading span to 16 bytes (just the user-data header fields).
            span_streambuf udBuf(fileData.subspan(offset, 16));
            std::istream udStream(&udBuf);
            BinaryReader udReader(udStream);

            u32 udMagic = udReader.read<u32>();
            (void)udMagic;
            u32 userDataSize = udReader.read<u32>();
            u32 headerOff = udReader.read<u32>();
            u32 userDataHeaderSize = udReader.read<u32>();

            size_t realHeaderOffset = offset + headerOff;
            if (realHeaderOffset + 32 > fileData.size())
                continue;

            size_t avail = fileData.size() - realHeaderOffset;
            size_t readSize = std::min<size_t>(avail, 512);
            span_streambuf hdrBufSmall(fileData.subspan(realHeaderOffset, readSize));
            std::istream hdrStreamSmall(&hdrBufSmall);
            BinaryReader hdrReaderSmall(hdrStreamSmall);

            auto hdr = parseHeaderFields(hdrReaderSmall, avail);
            if (!hdr)
                continue;

            HeaderParseResult result;
            result.header = *hdr;
            result.archiveOffset = realHeaderOffset;

            UserData ud;
            ud.magic = kMpqUserDataMagic;
            ud.userDataSize = userDataSize;
            ud.headerOffset = headerOff;
            ud.userDataHeaderSize = userDataHeaderSize;

            size_t udDataStart = offset + 16; // After the 16-byte user data header.
            size_t udDataLen = std::min<size_t>(userDataSize, fileData.size() - udDataStart);
            ud.data.assign(fileData.data() + udDataStart,
                           fileData.data() + udDataStart + udDataLen);

            result.userData = std::move(ud);
            return result;
        }

        if (magic == kMpqMagic) {
            size_t avail = fileData.size() - offset;
            // Clamp the span passed to BinaryReader to cover only the header.
            // V4 is the largest header at 208 bytes; 512 gives comfortable headroom.
            // Passing the full (potentially multi-GB) span causes BinaryReader's
            // seekg-to-end to leave the istream in a bad state on some implementations.
            size_t readSize = std::min<size_t>(avail, 512);
            span_streambuf sbuf(fileData.subspan(offset, readSize));
            std::istream stream(&sbuf);
            BinaryReader reader(stream);

            auto hdr = parseHeaderFields(reader, avail);
            if (!hdr)
                continue;

            HeaderParseResult result;
            result.header = *hdr;
            result.archiveOffset = offset;
            return result;
        }
    }

    return std::nullopt;
}

// ============================================================================
// Header Construction
// ============================================================================

MpqHeader buildHeader(u16 formatVersion, u32 hashTableSize, u16 sectorSizeShift) {
    MpqHeader h{};
    h.magic = kMpqMagic;
    h.formatVersion = formatVersion;
    h.sectorSizeShift = sectorSizeShift;

    u32 htSize = nextPowerOf2(hashTableSize);
    h.hashTableEntries = htSize;
    h.blockTableEntries = 0; // No files yet.

    // Header size depends on version.
    switch (formatVersion) {
    case 0:
        h.headerSize = 32;
        break;
    case 1:
        h.headerSize = 44;
        break;
    case 2:
        h.headerSize = 68;
        break;
    case 3:
        h.headerSize = 208;
        break;
    default:
        h.headerSize = 32;
        break;
    }

    // Table offsets will be filled by the writer once file data is written.
    return h;
}

} // namespace whiteout::storages::mpq
