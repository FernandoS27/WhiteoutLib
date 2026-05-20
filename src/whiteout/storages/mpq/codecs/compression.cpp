// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../common/zlib.h"
#include "../../common/codecs/bzip2.h"
#include "../../common/codecs/lzma.h"
#include "adpcm.h"

#include <array>
#include "compression.h"
#include "huffman.h"
#include "pkware.h"
#include "sparse.h"

#include <cstring>

namespace whiteout::storages::mpq {

using storages::common::bzip2Compress;
using storages::common::bzip2Decompress;
using storages::common::lzmaDecompress;

// ============================================================================
// Codec registry — maps compression flags to decompress/compress functions
// ============================================================================

namespace {

using DecompressFn = std::vector<u8> (*)(std::span<const u8>, size_t);
using CompressFn = std::vector<u8> (*)(std::span<const u8>);

struct CodecEntry {
    CompressionFlag flag;
    const char* name;
    DecompressFn decompress;
    CompressFn compress;
};

// Thin wrappers to give zlib and ADPCM a uniform signature.
std::vector<u8> zlibDecompressAdapter(std::span<const u8> src, size_t) {
    return storages::common::zlibDecompress(src);
}
std::vector<u8> zlibCompressAdapter(std::span<const u8> src) {
    return storages::common::zlibCompress(src);
}
std::vector<u8> adpcmMonoDecompressAdapter(std::span<const u8> src, size_t sz) {
    return adpcmDecompress(src, sz, 1);
}
std::vector<u8> adpcmStereoDecompressAdapter(std::span<const u8> src, size_t sz) {
    return adpcmDecompress(src, sz, 2);
}
std::vector<u8> adpcmMonoCompressAdapter(std::span<const u8> src) {
    return adpcmCompress(src, 1);
}
std::vector<u8> adpcmStereoCompressAdapter(std::span<const u8> src) {
    return adpcmCompress(src, 2);
}
std::vector<u8> huffmanCompressAdapter(std::span<const u8> src) {
    return huffmanCompress(src);
}

static constexpr size_t kCodecCount = 8;
static const std::array<CodecEntry, kCodecCount> kCodecTable = {{
    {CompressionFlag::kBZip2, "BZip2", bzip2Decompress, bzip2Compress},
    {CompressionFlag::kPKware, "PKware DCL", pkwareExplode, pkwareImplode},
    {CompressionFlag::kZlib, "zlib", zlibDecompressAdapter, zlibCompressAdapter},
    {CompressionFlag::kHuffman, "Huffman", huffmanDecompress, huffmanCompressAdapter},
    {CompressionFlag::kAdpcmMono, "ADPCM mono", adpcmMonoDecompressAdapter,
     adpcmMonoCompressAdapter},
    {CompressionFlag::kAdpcmStereo, "ADPCM stereo", adpcmStereoDecompressAdapter,
     adpcmStereoCompressAdapter},
    {CompressionFlag::kSparse, "Sparse", sparseDecompress, sparseCompress},
    {CompressionFlag::kLZMA, "LZMA", lzmaDecompress, nullptr},
}};

const CodecEntry* findCodec(CompressionFlag flag) {
    for (size_t i = 0; i < kCodecCount; ++i) {
        if (kCodecTable[i].flag == flag)
            return &kCodecTable[i];
    }
    return nullptr;
}

/// Apply a single decompression stage to an intermediate buffer.
/// Returns true on success, replacing `buf` with the decompressed data.
bool decompressStage(CompressionFlag flag, std::vector<u8>& buf, size_t finalSize, std::string* error) {
    const auto* codec = findCodec(flag);
    if (!codec) {
        if (error)
            *error = "Unknown compression flag: " + std::to_string(static_cast<u8>(flag));
        return false;
    }
    if (!codec->decompress) {
        if (error)
            *error = std::string(codec->name) + " decompression not implemented";
        return false;
    }
    auto result = codec->decompress(std::span<const u8>(buf), finalSize);
    if (result.empty() && !buf.empty()) {
        if (error)
            *error = std::string(codec->name) + " decompression failed";
        return false;
    }
    buf = std::move(result);
    return true;
}

/// Canonical decompression order (innermost first).
static constexpr std::array<CompressionFlag, 7> kDecompressOrder = {{
    CompressionFlag::kBZip2,   CompressionFlag::kPKware,    CompressionFlag::kZlib,
    CompressionFlag::kHuffman, CompressionFlag::kAdpcmMono, CompressionFlag::kAdpcmStereo,
    CompressionFlag::kSparse,
}};

} // anonymous namespace

// ============================================================================
// Decompression
// ============================================================================

std::vector<u8> mpqDecompress(std::span<const u8> src, size_t uncompressedSize,
                              std::string* error) {
    if (src.empty())
        return {};

    CompressionFlag const compressionMask = static_cast<CompressionFlag>(src[0]);
    auto compressedData = src.subspan(1); // Skip the compression byte.

    // If no compression flags set, data is uncompressed (shouldn't happen in
    // practice, but handle gracefully).
    if (compressionMask == CompressionFlag::None) {
        return {compressedData.begin(), compressedData.end()};
    }

    // Start with the compressed payload.
    std::vector<u8> buf(compressedData.begin(), compressedData.end());

    // LZMA (0x12) is a special case: it's NOT a single-bit bitmask but equals
    // kBZip2 | kZlib. When LZMA is used, the compression byte is exactly 0x12.
    // Must check it as an exact match before the bitmask loop.
    if (compressionMask == CompressionFlag::kLZMA) {
        if (!decompressStage(CompressionFlag::kLZMA, buf, uncompressedSize, error))
            return {};
        return buf;
    }

    // Bitmask-based decompression for all other algorithms.
    for (CompressionFlag const flag : kDecompressOrder) {
        if (hasFlag(compressionMask, flag)) {
            if (!decompressStage(flag, buf, uncompressedSize, error))
                return {};
        }
    }

    return buf;
}

// ============================================================================
// Compression
// ============================================================================

std::vector<u8> mpqCompress(std::span<const u8> src, CompressionFlag compressionType, std::string* error) {
    if (src.empty())
        return {};

    const auto* codec = findCodec(compressionType);
    if (!codec || !codec->compress) {
        if (error)
            *error = codec ? (std::string(codec->name) + " compression not implemented")
                           : ("Unsupported compression type: " +
                              std::to_string(static_cast<u8>(compressionType)));
        return {};
    }

    auto result = codec->compress(src);
    if (result.empty()) {
        if (error)
            *error = std::string(codec->name) + " compression failed";
        return {};
    }

    // Prepend the compression type byte.
    std::vector<u8> compressed;
    compressed.reserve(1 + result.size());
    compressed.push_back(static_cast<u8>(compressionType));
    compressed.insert(compressed.end(), result.begin(), result.end());

    // If compressed is larger than original, store uncompressed.
    if (compressed.size() >= src.size())
        return {};

    return compressed;
}

} // namespace whiteout::storages::mpq
