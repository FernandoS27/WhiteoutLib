// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "special_files.h"

#include <cstring>
#include <sstream>

namespace whiteout::storages::mpq {

// ============================================================================
// (listfile)
// ============================================================================

std::vector<std::string> parseListfile(std::span<const u8> data) {
    std::vector<std::string> result;
    if (data.empty())
        return result;

    std::string const content(reinterpret_cast<const char*>(data.data()), data.size());
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        // Remove trailing CR.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Skip empty lines and comments.
        if (line.empty())
            continue;
        if (line[0] == ';' || line[0] == '#')
            continue;

        result.push_back(std::move(line));
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
