// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "special_files.h"

#include <cstring>

namespace whiteout::storages::mpq {

// ============================================================================
// (listfile)
// ============================================================================

std::vector<std::string> parseListfile(std::span<const u8> data) {
    // As StormLib reads it: a line ends at CR or LF, and every other byte,
    // spaces and a leading '#' or ';' included, belongs to a name.
    std::vector<std::string> result;
    size_t start = 0;
    for (size_t i = 0; i <= data.size(); ++i) {
        if (i < data.size() && data[i] != '\r' && data[i] != '\n')
            continue;
        if (i > start)
            result.emplace_back(reinterpret_cast<const char*>(data.data()) + start, i - start);
        start = i + 1;
    }
    return result;
}

std::vector<u8> buildListfile(const std::vector<std::string>& filenames) {
    std::string result;
    for (const auto& name : filenames) {
        result += name;
        result += "\r\n";
    }
    return {result.begin(), result.end()};
}

// ============================================================================
// (attributes)
// ============================================================================

FileAttributes parseAttributes(std::span<const u8> data, u32 blockCount) {
    FileAttributes attrs;
    if (data.size() < 8)
        return attrs;

    // Header: version (u32) + flags (u32).
    u32 version = 0;
    AttributeFlag flags = AttributeFlag::None;
    std::memcpy(&version, data.data(), 4);
    std::memcpy(&flags, data.data() + 4, 4);

    size_t offset = 8;

    // Some writers leave out the last entry, the (attributes) file's own:
    // when the size fits one entry less exactly, that is the count
    // (StormLib's CheckSizeOfAttributesFile).
    size_t const perEntry = (hasFlag(flags, AttributeFlag::kCrc32) ? 4 : 0) +
                            (hasFlag(flags, AttributeFlag::kFiletime) ? 8 : 0) +
                            (hasFlag(flags, AttributeFlag::kMd5) ? 16 : 0);
    if (blockCount > 0 && perEntry > 0) {
        size_t const shortArrays = 8 + size_t{blockCount - 1} * perEntry;
        size_t const patchBits = (size_t{blockCount} + 6) / 8;
        if (data.size() == shortArrays ||
            (hasFlag(flags, AttributeFlag::kPatchBit) && data.size() == shortArrays + patchBits))
            --blockCount;
    }

    // Flag kCrc32: CRC32 array.
    if (hasFlag(flags, AttributeFlag::kCrc32)) {
        size_t const needed = static_cast<size_t>(blockCount) * sizeof(u32);
        if (offset + needed <= data.size()) {
            attrs.crc32s.resize(blockCount);
            std::memcpy(attrs.crc32s.data(), data.data() + offset, needed);
        }
        offset += needed;
    }

    // Flag kFiletime: FILETIME array.
    if (hasFlag(flags, AttributeFlag::kFiletime)) {
        size_t const needed = static_cast<size_t>(blockCount) * sizeof(u64);
        if (offset + needed <= data.size()) {
            attrs.filetimes.resize(blockCount);
            std::memcpy(attrs.filetimes.data(), data.data() + offset, needed);
        }
        offset += needed;
    }

    // Flag kMd5: MD5 array.
    if (hasFlag(flags, AttributeFlag::kMd5)) {
        size_t const needed = static_cast<size_t>(blockCount) * 16;
        if (offset + needed <= data.size()) {
            attrs.md5s.resize(blockCount);
            for (u32 i = 0; i < blockCount; ++i) {
                std::memcpy(attrs.md5s[i].data(), data.data() + offset + i * 16, 16);
            }
        }
        offset += needed;
    }

    return attrs;
}

std::vector<u8> buildAttributes(const FileAttributes& attrs, u32 version) {
    AttributeFlag flags = AttributeFlag::None;
    if (!attrs.crc32s.empty())
        flags |= AttributeFlag::kCrc32;
    if (!attrs.filetimes.empty())
        flags |= AttributeFlag::kFiletime;
    if (!attrs.md5s.empty())
        flags |= AttributeFlag::kMd5;

    size_t totalSize = 8;
    if (hasFlag(flags, AttributeFlag::kCrc32))
        totalSize += attrs.crc32s.size() * sizeof(u32);
    if (hasFlag(flags, AttributeFlag::kFiletime))
        totalSize += attrs.filetimes.size() * sizeof(u64);
    if (hasFlag(flags, AttributeFlag::kMd5))
        totalSize += attrs.md5s.size() * 16;

    std::vector<u8> result(totalSize);
    size_t offset = 0;

    std::memcpy(result.data() + offset, &version, 4);
    offset += 4;
    u32 flagsU32 = static_cast<u32>(flags);
    std::memcpy(result.data() + offset, &flagsU32, 4);
    offset += 4;

    if (hasFlag(flags, AttributeFlag::kCrc32)) {
        size_t const len = attrs.crc32s.size() * sizeof(u32);
        std::memcpy(result.data() + offset, attrs.crc32s.data(), len);
        offset += len;
    }
    if (hasFlag(flags, AttributeFlag::kFiletime)) {
        size_t const len = attrs.filetimes.size() * sizeof(u64);
        std::memcpy(result.data() + offset, attrs.filetimes.data(), len);
        offset += len;
    }
    if (hasFlag(flags, AttributeFlag::kMd5)) {
        for (const auto& md5 : attrs.md5s) {
            std::memcpy(result.data() + offset, md5.data(), 16);
            offset += 16;
        }
    }

    return result;
}

} // namespace whiteout::storages::mpq
