// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../common/codecs/bzip2.h"
#include "../../common/codecs/lzma.h"
#include "../../common/inflate_fast.h"
#include "../../common/zlib.h"
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
std::vector<u8> zlibDecompressAdapter(std::span<const u8> src, size_t sz) {
    return storages::common::zlibDecompress(src, sz);
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

// MPQ's LZMA sector: a filter byte (0, none), the 5 property bytes, the
// 8-byte uncompressed size, then the stream.
constexpr size_t kLzmaPropsSize = 5;
constexpr size_t kLzmaHeaderSize = 1 + kLzmaPropsSize + 8;
std::vector<u8> mpqLzmaDecompress(std::span<const u8> src, size_t sz) {
    if (src.size() <= kLzmaHeaderSize || src[0] != 0)
        return {};
    return lzmaDecompress(src.subspan(1, kLzmaPropsSize), src.subspan(kLzmaHeaderSize), sz);
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
    {CompressionFlag::kLZMA, "LZMA", mpqLzmaDecompress, nullptr},
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
bool decompressStage(CompressionFlag flag, std::vector<u8>& buf, size_t finalSize,
                     std::string* error) {
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
    CompressionFlag::kBZip2,
    CompressionFlag::kPKware,
    CompressionFlag::kZlib,
    CompressionFlag::kHuffman,
    CompressionFlag::kAdpcmMono,
    CompressionFlag::kAdpcmStereo,
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
    CompressionFlag unclaimed = compressionMask;
    for (CompressionFlag const flag : kDecompressOrder) {
        if (hasFlag(compressionMask, flag)) {
            if (!decompressStage(flag, buf, uncompressedSize, error))
                return {};
            unclaimed &= ~flag;
        }
    }
    // A bit no codec owns means a codec we would skip, not data to pass on.
    if (unclaimed != CompressionFlag::None) {
        if (error)
            *error = "Unknown compression flag: " + std::to_string(static_cast<u8>(unclaimed));
        return {};
    }

    return buf;
}

namespace {

/// One stage of the pipeline into @p dst; the bytes written, 0 on failure.
size_t decompressStageInto(CompressionFlag flag, std::span<const u8> src, std::span<u8> dst) {
    switch (flag) {
    case CompressionFlag::kBZip2:
        return storages::common::bzip2DecompressInto(src, dst);
    case CompressionFlag::kPKware:
        return pkwareExplodeInto(src, dst);
    case CompressionFlag::kZlib:
        return storages::common::zlibInflateCheckedInto(dst, src).value_or(0);
    case CompressionFlag::kHuffman:
        return huffmanDecompressInto(src, dst);
    case CompressionFlag::kAdpcmMono:
        return adpcmDecompressInto(src, dst, 1);
    case CompressionFlag::kAdpcmStereo:
        return adpcmDecompressInto(src, dst, 2);
    case CompressionFlag::kSparse:
        return sparseDecompressInto(src, dst);
    default:
        return 0;
    }
}

} // anonymous namespace

bool mpqDecompressInto(std::span<const u8> src, std::span<u8> dst, DecodeScratch& scratch, std::string* error) {
    auto fail = [&](auto describe) {
        if (error)
            *error = describe();
        return false;
    };
    auto sizeMismatch = [&](size_t got) {
        return fail([&] {
            return "decompressed to " + std::to_string(got) + " bytes, expected " + std::to_string(dst.size());
        });
    };
    if (src.empty())
        return sizeMismatch(0);

    CompressionFlag const compressionMask = static_cast<CompressionFlag>(src[0]);
    std::span<const u8> input = src.subspan(1);

    // No codec at all: the payload is the data.
    if (compressionMask == CompressionFlag::None) {
        if (input.size() != dst.size())
            return sizeMismatch(input.size());
        std::memcpy(dst.data(), input.data(), dst.size());
        return true;
    }

    // LZMA is the exact value 0x12, not the two bits it shares with others.
    if (compressionMask == CompressionFlag::kLZMA) {
        if (input.size() <= kLzmaHeaderSize || input[0] != 0)
            return fail([] { return std::string("LZMA decompression failed"); });
        size_t const got = storages::common::lzmaDecompressInto(input.subspan(1, kLzmaPropsSize),
                                                                input.subspan(kLzmaHeaderSize), dst);
        if (got == 0)
            return fail([] { return std::string("LZMA decompression failed"); });
        return got == dst.size() ? true : sizeMismatch(got);
    }

    // A bit no codec owns means a codec we would skip, not data to pass on.
    CompressionFlag unclaimed = compressionMask;
    u32 stages = 0;
    for (CompressionFlag const flag : kDecompressOrder) {
        if (hasFlag(compressionMask, flag)) {
            unclaimed &= ~flag;
            ++stages;
        }
    }
    if (unclaimed != CompressionFlag::None)
        return fail([&] { return "Unknown compression flag: " + std::to_string(static_cast<u8>(unclaimed)); });

    // Each stage but the last writes into a scratch buffer of the final size.
    u32 stage = 0;
    for (CompressionFlag const flag : kDecompressOrder) {
        if (!hasFlag(compressionMask, flag))
            continue;
        std::span<u8> output = dst;
        if (++stage < stages) {
            std::vector<u8>& buffer = scratch.stage[stage & 1];
            if (buffer.size() < dst.size())
                buffer.resize(dst.size());
            output = std::span<u8>(buffer.data(), dst.size());
        }
        size_t const got = decompressStageInto(flag, input, output);
        if (got == 0 && !input.empty())
            return fail([&] { return std::string(findCodec(flag)->name) + " decompression failed"; });
        input = output.first(got);
    }
    return input.size() == dst.size() ? true : sizeMismatch(input.size());
}

// ============================================================================
// Compression
// ============================================================================

bool mpqCompressAppend(std::span<const u8> src, CompressionFlag compressionType, std::vector<u8>& out,
                       std::string* error) {
    if (src.empty())
        return false;

    // ADPCM with Huffman, the way Blizzard stores waves: ADPCM first, then
    // Huffman over what it gives.
    CompressionFlag const adpcm = compressionType & (CompressionFlag::kAdpcmMono | CompressionFlag::kAdpcmStereo);
    bool const paired = adpcm != CompressionFlag::None && hasFlag(compressionType, CompressionFlag::kHuffman);
    const auto* codec = findCodec(paired ? adpcm : compressionType);
    if (!codec || !codec->compress) {
        if (error)
            *error = codec ? (std::string(codec->name) + " compression not implemented")
                           : ("Unsupported compression type: " +
                              std::to_string(static_cast<u8>(compressionType)));
        return false;
    }

    // zlib writes straight behind the compression type byte.
    if (compressionType == CompressionFlag::kZlib) {
        size_t const start = out.size();
        out.push_back(static_cast<u8>(compressionType));
        if (!::whiteout::zlib_compress_append(out, src, error) || out.size() - start >= src.size()) {
            out.resize(start);
            return false;
        }
        return true;
    }

    auto result = codec->compress(src);
    if (paired && !result.empty())
        result = huffmanCompressAdapter(result);
    if (result.empty()) {
        if (error)
            *error = std::string(codec->name) + " compression failed";
        return false;
    }

    // If compressed is larger than original, store uncompressed.
    if (1 + result.size() >= src.size())
        return false;

    // The compression type byte, then the data.
    out.push_back(static_cast<u8>(compressionType));
    out.insert(out.end(), result.begin(), result.end());
    return true;
}

std::vector<u8> mpqCompress(std::span<const u8> src, CompressionFlag compressionType,
                            std::string* error) {
    std::vector<u8> compressed;
    if (!mpqCompressAppend(src, compressionType, compressed, error))
        return {};
    return compressed;
}

} // namespace whiteout::storages::mpq
