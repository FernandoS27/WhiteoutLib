// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "compression.h"
#include "pkware.h"
#include "../common/zlib.h"

#include <cstring>

namespace whiteout::storages::mpq {

// ============================================================================
// Decompression
// ============================================================================

namespace {

/// Apply a single decompression stage to an intermediate buffer.
/// Returns true on success, replacing `buf` with the decompressed data.
bool decompressStage(u8 flag, std::vector<u8>& buf, size_t finalSize, std::string* error) {
    switch (flag) {
    case CompressionFlag::kZlib: {
        auto result = storages::common::zlibDecompress(std::span<const u8>(buf), finalSize);
        if (result.empty() && !buf.empty()) {
            if (error) *error = "zlib decompression failed";
            return false;
        }
        buf = std::move(result);
        return true;
    }
    case CompressionFlag::kPKware: {
        auto result = pkwareExplode(std::span<const u8>(buf), finalSize);
        if (result.empty() && !buf.empty()) {
            if (error) *error = "PKware DCL decompression failed";
            return false;
        }
        buf = std::move(result);
        return true;
    }
    case CompressionFlag::kBZip2:
        if (error) *error = "BZip2 decompression not implemented";
        return false;
    case CompressionFlag::kLZMA:
        if (error) *error = "LZMA decompression not implemented";
        return false;
    case CompressionFlag::kHuffman:
        // TODO: Implement MPQ-specific Huffman decompression.
        if (error) *error = "MPQ Huffman decompression not implemented";
        return false;
    case CompressionFlag::kSparse:
        // TODO: Implement sparse/RLE decompression.
        if (error) *error = "Sparse decompression not implemented";
        return false;
    case CompressionFlag::kAdpcmMono:
        if (error) *error = "ADPCM mono decompression not implemented";
        return false;
    case CompressionFlag::kAdpcmStereo:
        if (error) *error = "ADPCM stereo decompression not implemented";
        return false;
    default:
        if (error) *error = "Unknown compression flag: " + std::to_string(flag);
        return false;
    }
}

} // anonymous namespace

std::vector<u8> mpqDecompress(
    std::span<const u8> src,
    size_t uncompressedSize,
    std::string* error)
{
    if (src.empty()) return {};

    u8 compressionMask = src[0];
    auto compressedData = src.subspan(1); // Skip the compression byte.

    // If no compression flags set, data is uncompressed (shouldn't happen in
    // practice, but handle gracefully).
    if (compressionMask == 0) {
        return {compressedData.begin(), compressedData.end()};
    }

    // Start with the compressed payload.
    std::vector<u8> buf(compressedData.begin(), compressedData.end());

    // LZMA (0x12) is a special case: it's NOT a single-bit bitmask but equals
    // kBZip2 | kZlib. When LZMA is used, the compression byte is exactly 0x12.
    // Must check it as an exact match before the bitmask loop.
    if (compressionMask == CompressionFlag::kLZMA) {
        if (!decompressStage(CompressionFlag::kLZMA, buf, uncompressedSize, error)) {
            return {};
        }
        return buf;
    }

    // Bitmask-based decompression for all other algorithms.
    // Decompressed in canonical order (innermost first).
    static constexpr u8 kDecompressOrder[] = {
        CompressionFlag::kBZip2,
        CompressionFlag::kPKware,
        CompressionFlag::kZlib,
        CompressionFlag::kHuffman,
        CompressionFlag::kAdpcmMono,
        CompressionFlag::kAdpcmStereo,
        CompressionFlag::kSparse,
    };

    for (u8 flag : kDecompressOrder) {
        if (compressionMask & flag) {
            if (!decompressStage(flag, buf, uncompressedSize, error)) {
                return {};
            }
        }
    }

    return buf;
}

// ============================================================================
// Compression
// ============================================================================

std::vector<u8> mpqCompress(
    std::span<const u8> src,
    u8 compressionType,
    std::string* error)
{
    if (src.empty()) return {};

    std::vector<u8> compressed;

    switch (compressionType) {
    case CompressionFlag::kZlib: {
        auto result = storages::common::zlibCompress(src);
        if (result.empty()) {
            if (error) *error = "zlib compression failed";
            return {};
        }
        compressed.reserve(1 + result.size());
        compressed.push_back(CompressionFlag::kZlib);
        compressed.insert(compressed.end(), result.begin(), result.end());
        break;
    }
    case CompressionFlag::kPKware: {
        auto result = pkwareImplode(src);
        if (result.empty()) {
            if (error) *error = "PKware DCL compression failed";
            return {};
        }
        compressed.reserve(1 + result.size());
        compressed.push_back(CompressionFlag::kPKware);
        compressed.insert(compressed.end(), result.begin(), result.end());
        break;
    }
    default:
        if (error) *error = "Unsupported compression type for writing: " + std::to_string(compressionType);
        return {};
    }

    // If compressed is larger than original, store uncompressed.
    if (compressed.size() >= src.size()) {
        return {};
    }

    return compressed;
}

} // namespace whiteout::storages::mpq
