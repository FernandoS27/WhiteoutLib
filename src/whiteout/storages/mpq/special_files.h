// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file special_files.h
/// @brief MPQ special file parsing and building: (listfile), (attributes).

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::storages::mpq {

// ============================================================================
// (attributes) Flags
// ============================================================================

enum class AttributeFlag : u32 {
    None = 0,
    kCrc32 = 0x00000001,    ///< CRC32 array is present.
    kFiletime = 0x00000002, ///< FILETIME array is present.
    kMd5 = 0x00000004,      ///< MD5 array is present.
    kPatchBit = 0x00000008, ///< A bit per block marks patch files (read for its size only).
};

inline AttributeFlag operator|(AttributeFlag a, AttributeFlag b) noexcept {
    return static_cast<AttributeFlag>(static_cast<u32>(a) | static_cast<u32>(b));
}
inline AttributeFlag operator&(AttributeFlag a, AttributeFlag b) noexcept {
    return static_cast<AttributeFlag>(static_cast<u32>(a) & static_cast<u32>(b));
}
inline AttributeFlag& operator|=(AttributeFlag& a, AttributeFlag b) noexcept {
    a = a | b;
    return a;
}
inline bool hasFlag(AttributeFlag flags, AttributeFlag flag) noexcept {
    return (flags & flag) != AttributeFlag::None;
}

// ============================================================================
// (listfile)
// ============================================================================

/// @p fn(line) for each line of a `(listfile)`, as StormLib reads it: a line
/// ends at CR or LF, and every other byte, spaces and a leading '#' or ';'
/// included, belongs to a name. Empty lines are skipped. A line points into
/// @p data.
template <class Fn>
void forEachListfileLine(std::span<const u8> data, Fn fn) {
    const char* const text = reinterpret_cast<const char*>(data.data());
    size_t start = 0;
    for (size_t i = 0; i <= data.size(); ++i) {
        if (i < data.size() && text[i] != '\r' && text[i] != '\n')
            continue;
        if (i > start)
            fn(std::string_view(text + start, i - start));
        start = i + 1;
    }
}

/// Parse a `(listfile)` file into a list of filenames (see forEachListfileLine).
[[nodiscard]] std::vector<std::string> parseListfile(std::span<const u8> data);

/// Build a `(listfile)` from a list of filenames.
/// Produces CRLF-separated output.
[[nodiscard]] std::vector<u8> buildListfile(const std::vector<std::string>& filenames);

// ============================================================================
// (attributes)
// ============================================================================

/// Per-file attributes from `(attributes)`.
struct FileAttributes {
    std::vector<u32> crc32s;              ///< CRC32 per block entry.
    std::vector<u64> filetimes;           ///< FILETIME per block entry.
    std::vector<std::array<u8, 16>> md5s; ///< MD5 per block entry.
};

/// Parse `(attributes)` file data.
/// @param data       Raw (attributes) file content.
/// @param blockCount Number of block table entries (determines array sizes).
[[nodiscard]] FileAttributes parseAttributes(std::span<const u8> data, u32 blockCount);

/// Build an `(attributes)` file from per-file data.
/// @param attrs   The attributes to serialize.
/// @param version Attributes version (typically 100).
[[nodiscard]] std::vector<u8> buildAttributes(const FileAttributes& attrs, u32 version = 100);

} // namespace whiteout::storages::mpq
