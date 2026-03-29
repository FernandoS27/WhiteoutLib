// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file special_files.h
/// @brief MPQ special file parsing and building: (listfile), (attributes).

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <string>
#include <vector>

namespace whiteout::storages::mpq {

// ============================================================================
// (listfile)
// ============================================================================

/// Parse a `(listfile)` file into a list of filenames.
/// Splits on CR/LF, skips empty lines and lines starting with `;` or `#`.
[[nodiscard]] std::vector<std::string> parseListfile(std::span<const u8> data);

/// Build a `(listfile)` from a list of filenames.
/// Produces CRLF-separated output.
[[nodiscard]] std::vector<u8> buildListfile(const std::vector<std::string>& filenames);

// ============================================================================
// (attributes)
// ============================================================================

/// Per-file attributes from `(attributes)`.
struct FileAttributes {
    std::vector<u32> crc32s;        ///< CRC32 per block entry.
    std::vector<u64> filetimes;     ///< FILETIME per block entry.
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
