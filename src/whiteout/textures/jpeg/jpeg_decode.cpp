// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// JPEG decoder supporting baseline (SOF0) and progressive (SOF2) modes.
///
/// Returns raw component values without Y'CbCr-to-RGB conversion.  BLP files
/// store BGRA colour components directly in the JPEG data stream, so the raw
/// values must be preserved.
///
/// Supported:   Baseline sequential DCT (SOF0), progressive DCT (SOF2),
///              8-bit precision, 1-4 channels, restart markers (DRI/RST),
///              chroma subsampling.
/// Unsupported: Arithmetic coding, lossless, hierarchical.
///
/// Algorithms used:
///   - Huffman decoding: fast 9-bit look-up table with slow-path fallback
///     (identical approach to stb_image / libjpeg-turbo).
///   - Inverse DCT: Loeffler-Ligtenberg-Moschytz (LLM) separable 1-D
///     butterfly, applied row-then-column (IEEE 1992 fast IDCT).
///   - Upsampling: nearest-neighbour replication for subsampled components.

#include "jpeg_decode.h"

#include "huffman.h"
#include "jpeg_common.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace whiteout::textures::jpeg {

namespace {

// ============================================================================
// JPEG Sign Extension
// ============================================================================

/// Branchless JPEG sign extension (libjpeg-turbo style).
/// If the magnitude value is below 2^(category-1), the value is negative.
/// Uses an arithmetic mask to avoid the branch.
inline i32 extend_magnitude_to_signed(u32 magnitudeBits, i32 category) {
    i32 threshold = 1 << (category - 1);
    // mask = 0xFFFFFFFF when negative, 0x00000000 when positive.
    i32 mask = -static_cast<i32>(static_cast<i32>(magnitudeBits) < threshold);
    return static_cast<i32>(magnitudeBits) + (mask & (1 - (2 * threshold)));
}

// ============================================================================
// Inverse DCT — Loeffler-Ligtenberg-Moschytz (LLM) Butterfly
// ============================================================================

/// In-place 1-D IDCT butterfly on 8 contiguous floats (LLM algorithm).
/// Reads all 8 inputs into registers before writing, so input == output is safe.
inline void idct_1d_inplace(f32* data) {
    const f32 x0 = data[0], x1 = data[1], x2 = data[2], x3 = data[3];
    const f32 x4 = data[4], x5 = data[5], x6 = data[6], x7 = data[7];

    // Even-indexed butterflies (x0, x2, x4, x6).
    const f32 rot = (x2 + x6) * EVEN_ROTATION_K;
    const f32 even2 = rot - x6 * EVEN_ROTATION_A;
    const f32 even3 = rot + x2 * EVEN_ROTATION_B;
    const f32 e0 = x0 + x4;
    const f32 e1 = x0 - x4;
    const f32 se0 = e0 + even3;
    const f32 se3 = e0 - even3;
    const f32 se1 = e1 + even2;
    const f32 se2 = e1 - even2;

    // Odd-indexed butterflies (x1, x3, x5, x7).
    const f32 s73 = x7 + x3, s51 = x5 + x1;
    const f32 s71 = x7 + x1, s53 = x5 + x3;
    const f32 sf = (s73 + s51) * ODD_SCALE;
    const f32 t71 = sf + s71 * ODD_PAIR_71;
    const f32 t53 = sf + s53 * ODD_PAIR_53;
    const f32 t73 = s73 * ODD_PAIR_73;
    const f32 t51 = s51 * ODD_PAIR_51;
    const f32 o0 = x7 * ODD_COEFF_X7 + t71 + t73;
    const f32 o1 = x5 * ODD_COEFF_X5 + t53 + t51;
    const f32 o2 = x3 * ODD_COEFF_X3 + t53 + t73;
    const f32 o3 = x1 * ODD_COEFF_X1 + t71 + t51;

    // Final butterfly: combine even and odd parts.
    data[0] = se0 + o3;
    data[1] = se1 + o2;
    data[2] = se2 + o1;
    data[3] = se3 + o0;
    data[4] = se3 - o0;
    data[5] = se2 - o1;
    data[6] = se1 - o2;
    data[7] = se0 - o3;
}

/// Transpose an 8x8 float matrix in-place.
/// After transpose, columns become rows — enabling contiguous access for the
/// column IDCT pass.
inline void transpose_8x8_inplace(f32* block) {
    for (i32 i = 0; i < BLOCK_SIZE; ++i) {
        for (i32 j = i + 1; j < BLOCK_SIZE; ++j) {
            const i32 a = i * BLOCK_SIZE + j;
            const i32 b = j * BLOCK_SIZE + i;
            const f32 tmp = block[a];
            block[a] = block[b];
            block[b] = tmp;
        }
    }
}

/// Apply the 2-D IDCT to an 8x8 block of dequantised coefficients.
///
/// Strategy: row IDCT → 8x8 transpose → column IDCT (now contiguous) →
/// normalise + level-shift + clamp → write to strided output.
void inverse_dct_block(const std::array<i32, BLOCK_PIXELS>& dequantisedCoefficients,
                       u8* outputPixels, u32 outputRowStride) {
    alignas(32) f32 block[BLOCK_PIXELS];

    // Row pass: convert i32 → f32, skip all-zero-AC rows, run 1-D IDCT.
    for (i32 row = 0; row < BLOCK_SIZE; ++row) {
        const i32* src = dequantisedCoefficients.data() + row * BLOCK_SIZE;
        f32* dst = block + row * BLOCK_SIZE;

        // Fast all-AC-zero check using integer OR (avoids float comparisons).
        const i32 acOr = src[1] | src[2] | src[3] | src[4] | src[5] | src[6] | src[7];
        if (acOr == 0) {
            const f32 dc = static_cast<f32>(src[0]);
            dst[0] = dc;
            dst[1] = dc;
            dst[2] = dc;
            dst[3] = dc;
            dst[4] = dc;
            dst[5] = dc;
            dst[6] = dc;
            dst[7] = dc;
        } else {
            dst[0] = static_cast<f32>(src[0]);
            dst[1] = static_cast<f32>(src[1]);
            dst[2] = static_cast<f32>(src[2]);
            dst[3] = static_cast<f32>(src[3]);
            dst[4] = static_cast<f32>(src[4]);
            dst[5] = static_cast<f32>(src[5]);
            dst[6] = static_cast<f32>(src[6]);
            dst[7] = static_cast<f32>(src[7]);
            idct_1d_inplace(dst);
        }
    }

    // Transpose so columns become contiguous rows.
    transpose_8x8_inplace(block);

    // Column pass: 1-D IDCT on all 8 (transposed) columns.
    for (i32 col = 0; col < BLOCK_SIZE; ++col) {
        idct_1d_inplace(block + col * BLOCK_SIZE);
    }

    // Second transpose: restore row-major order so rows are contiguous.
    transpose_8x8_inplace(block);

    // Normalise + clamp + write: inner loop reads/writes contiguous memory.
    // The compiler can vectorize the 8-wide float→u8 conversion (mul, add,
    // cvt, pack) since both source and destination are contiguous.
    for (i32 row = 0; row < BLOCK_SIZE; ++row) {
        const f32* src = block + row * BLOCK_SIZE;
        u8* dst = outputPixels + row * outputRowStride;
        for (i32 col = 0; col < BLOCK_SIZE; ++col) {
            const i32 val =
                static_cast<i32>(src[col] * DCT_2D_NORMALISATION + DC_LEVEL_SHIFT_AND_ROUND);
            dst[col] = static_cast<u8>(std::clamp(val, 0, 255));
        }
    }
}

// ============================================================================
// Image Component Descriptor
// ============================================================================

/// Per-component state tracked during JPEG decoding.
struct ComponentDescriptor {
    u8 componentId = 0;        ///< JPEG component identifier (from SOF).
    u8 horizontalSampling = 1; ///< Horizontal sampling factor (1-4).
    u8 verticalSampling = 1;   ///< Vertical sampling factor (1-4).
    u8 quantTableIndex = 0;    ///< Index into the quantisation table array.
    u8 dcHuffmanIndex = 0;     ///< DC Huffman table selector (from SOS).
    u8 acHuffmanIndex = 0;     ///< AC Huffman table selector (from SOS).
    i32 dcPrediction = 0;      ///< Running DC prediction value.

    u32 sampleBufferStride = 0;   ///< Row pitch (in samples) of the decoded buffer.
    std::vector<u8> sampleBuffer; ///< Decoded samples before interleaving.

    /// Progressive mode: per-block coefficient storage (zig-zag order).
    /// Size = totalBlocksHorizontal * totalBlocksVertical, each entry is 64 coefficients.
    std::vector<std::array<i32, BLOCK_PIXELS>> coefficientBlocks;
    u32 blocksPerRow = 0; ///< Number of 8×8 blocks per row for this component.
    u32 blocksPerCol = 0; ///< Number of 8×8 blocks per column for this component.
};

// ============================================================================
// Decoder State Machine
// ============================================================================

struct JpegDecoder {
    BitstreamReader bitstream;

    std::array<std::array<i16, BLOCK_PIXELS>, MAX_TABLES>
        quantTables{}; ///< Quantisation tables (zig-zag order).
    std::array<bool, MAX_TABLES> quantTablePresent{};

    std::array<HuffmanTable, MAX_TABLES> dcHuffmanTables;
    std::array<HuffmanTable, MAX_TABLES> acHuffmanTables;

    std::array<ComponentDescriptor, MAX_COMPONENTS> components;
    u32 componentCount = 0;
    u32 imageWidth = 0;
    u32 imageHeight = 0;

    u32 maxHorizontalSampling = 1;
    u32 maxVerticalSampling = 1;
    u32 mcuPixelWidth = 0;  ///< MCU width in pixels  (maxHorizontalSampling * 8).
    u32 mcuPixelHeight = 0; ///< MCU height in pixels (maxVerticalSampling * 8).
    u32 mcuColumnsCount = 0;
    u32 mcuRowsCount = 0;
    u32 restartInterval = 0; ///< MCUs between restart markers (0 = disabled).

    // -- Progressive state --
    bool isProgressive = false;
    u8 scanSpectralStart = 0; ///< Ss from current scan's SOS.
    u8 scanSpectralEnd = 63;  ///< Se from current scan's SOS.
    u8 scanApproxHigh = 0;    ///< Ah (successive approximation high bit).
    u8 scanApproxLow = 0;     ///< Al (successive approximation low bit).
    u32 eobRun = 0;           ///< EOBRUN counter for progressive AC scans.

    /// Component indices included in the current scan (set by parseScanHeader).
    std::vector<u32> scanComponentIndices;

    std::string* errorOutput = nullptr;
    JpegContext* ctx = nullptr;

    // -- Helpers --

    bool reportError(const std::string& message) {
        if (errorOutput) {
            *errorOutput = message;
        }
        return false;
    }

    u16 readBigEndianU16(size_t offset) const {
        return static_cast<u16>((bitstream.data[offset] << 8) | bitstream.data[offset + 1]);
    }

    // -- Marker Segment Parsers --

    bool parseQuantizationTable(size_t dataOffset, size_t dataLength);
    bool parseFrameHeader(size_t dataOffset, size_t dataLength);
    bool parseHuffmanTable(size_t dataOffset, size_t dataLength);
    bool parseRestartInterval(size_t dataOffset, size_t dataLength);
    bool parseScanHeader(size_t dataOffset, size_t dataLength, size_t& scanDataStart);

    // -- Entropy Decoding --

    bool decodeDctBlock(std::array<i32, BLOCK_PIXELS>& coefficients, const HuffmanTable& dcTable,
                        const HuffmanTable& acTable, i32& dcPrediction,
                        const std::array<i16, BLOCK_PIXELS>& quantTable);
    bool decodeScanData();

    /// Decode entropy data to coefficient blocks WITHOUT running IDCT.
    /// Used when a worker pool is available so IDCT can run in parallel.
    /// Reuses the progressive coefficient storage in each ComponentDescriptor.
    bool decodeScanDataToCoefficients();

    /// Parallel baseline entropy decode using restart intervals.
    /// Fuses entropy decode + dequantise + IDCT per interval.
    /// When directOutput is non-null and no subsampling is active, writes the
    /// interleaved pixel data directly to the output image (skipping sample
    /// buffers and the separate assembly pass).
    bool decodeScanDataParallel(Image* directOutput = nullptr);

    // -- Progressive Entropy Decoding --

    bool decodeProgressiveScan();
    bool decodeProgressiveDcFirst(u32 compIdx, std::array<i32, BLOCK_PIXELS>& coeffs,
                                  i32& dcPrediction);
    bool decodeProgressiveDcRefine(std::array<i32, BLOCK_PIXELS>& coeffs);
    bool decodeProgressiveAcFirst(BitstreamReader& bs, std::array<i32, BLOCK_PIXELS>& coeffs,
                                  const HuffmanTable& acTable, u32& eobRunRef, u8 ss, u8 se, u8 al);
    bool decodeProgressiveAcRefine(BitstreamReader& bs, std::array<i32, BLOCK_PIXELS>& coeffs,
                                   const HuffmanTable& acTable, u32& eobRunRef, u8 ss, u8 se, u8 al);
    bool finalizeProgressiveImage();

    /// Combined dequantize + IDCT + interleave for the no-subsampling case.
    /// Writes directly to the output image, eliminating the intermediate
    /// sample buffer write + a separate assembly pass.
    bool finalizeAndAssembleImage(Image& outputImage);

    // -- Output Assembly --

    bool assembleInterleavedImage(Image& outputImage);

    // -- Top-Level Entry Point --

    bool decode(const u8* data, size_t size, Image& outputImage);
};

// ============================================================================
// Marker Segment Parsers
// ============================================================================

/// DQT — Define Quantization Table (ITU-T T.81, Section B.2.4.1)
bool JpegDecoder::parseQuantizationTable(size_t dataOffset, size_t dataLength) {
    size_t endOffset = dataOffset + dataLength;
    while (dataOffset < endOffset) {
        if (dataOffset >= bitstream.size) {
            return reportError("DQT: unexpected end of data");
        }
        u8 tableInfo = bitstream.data[dataOffset++];
        i32 elementPrecision = (tableInfo >> 4) & 0x0F; // 0 = 8-bit, 1 = 16-bit
        i32 tableIndex = tableInfo & 0x0F;
        if (tableIndex >= MAX_TABLES) {
            return reportError("DQT: table index " + std::to_string(tableIndex) +
                               " exceeds maximum");
        }

        if (elementPrecision == 0) {
            if (dataOffset + BLOCK_PIXELS > endOffset) {
                return reportError("DQT: 8-bit table truncated");
            }
            for (i32 coefficientIndex = 0; coefficientIndex < BLOCK_PIXELS; coefficientIndex++) {
                quantTables[tableIndex][coefficientIndex] =
                    static_cast<i16>(bitstream.data[dataOffset++]);
            }
        } else {
            if (dataOffset + BLOCK_PIXELS * 2 > endOffset) {
                return reportError("DQT: 16-bit table truncated");
            }
            for (i32 coefficientIndex = 0; coefficientIndex < BLOCK_PIXELS; coefficientIndex++) {
                quantTables[tableIndex][coefficientIndex] = static_cast<i16>(
                    (bitstream.data[dataOffset] << 8) | bitstream.data[dataOffset + 1]);
                dataOffset += 2;
            }
        }
        quantTablePresent[tableIndex] = true;
    }
    return true;
}

/// SOF0 — Baseline DCT Frame Header (ITU-T T.81, Section B.2.2)
bool JpegDecoder::parseFrameHeader(size_t dataOffset, size_t dataLength) {
    if (dataLength < 6) {
        return reportError("SOF0: segment too short");
    }
    u8 samplePrecision = bitstream.data[dataOffset];
    if (samplePrecision != 8) {
        return reportError("SOF0: only 8-bit sample precision is supported");
    }
    imageHeight = readBigEndianU16(dataOffset + 1);
    imageWidth = readBigEndianU16(dataOffset + 3);
    componentCount = bitstream.data[dataOffset + 5];
    if (componentCount == 0 || componentCount > MAX_COMPONENTS) {
        return reportError("SOF0: unsupported component count " + std::to_string(componentCount));
    }
    if (dataLength < 6 + componentCount * 3) {
        return reportError("SOF0: segment too short for component specifications");
    }
    if (imageWidth == 0 || imageHeight == 0) {
        return reportError("SOF0: image has zero dimensions");
    }

    maxHorizontalSampling = 1;
    maxVerticalSampling = 1;
    for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
        size_t specOffset = dataOffset + 6 + componentIndex * 3;
        components[componentIndex].componentId = bitstream.data[specOffset];
        u8 samplingFactors = bitstream.data[specOffset + 1];
        components[componentIndex].horizontalSampling = (samplingFactors >> 4) & 0x0F;
        components[componentIndex].verticalSampling = samplingFactors & 0x0F;
        components[componentIndex].quantTableIndex = bitstream.data[specOffset + 2];
        if (components[componentIndex].horizontalSampling == 0 ||
            components[componentIndex].verticalSampling == 0) {
            return reportError("SOF0: zero sampling factor for component " +
                               std::to_string(componentIndex));
        }
        if (components[componentIndex].quantTableIndex >= MAX_TABLES) {
            return reportError("SOF0: quantisation table index out of range");
        }
        maxHorizontalSampling = std::max(
            maxHorizontalSampling, static_cast<u32>(components[componentIndex].horizontalSampling));
        maxVerticalSampling = std::max(
            maxVerticalSampling, static_cast<u32>(components[componentIndex].verticalSampling));
    }

    mcuPixelWidth = maxHorizontalSampling * BLOCK_SIZE;
    mcuPixelHeight = maxVerticalSampling * BLOCK_SIZE;
    mcuColumnsCount = (imageWidth + mcuPixelWidth - 1) / mcuPixelWidth;
    mcuRowsCount = (imageHeight + mcuPixelHeight - 1) / mcuPixelHeight;

    // Allocate per-component sample buffers (MCU-aligned dimensions).
    for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
        u32 bufferWidth =
            mcuColumnsCount * components[componentIndex].horizontalSampling * BLOCK_SIZE;
        u32 bufferHeight = mcuRowsCount * components[componentIndex].verticalSampling * BLOCK_SIZE;
        components[componentIndex].sampleBufferStride = bufferWidth;
        components[componentIndex].sampleBuffer.resize(
            static_cast<size_t>(bufferWidth) * bufferHeight, 0);

        // For progressive mode, allocate per-block coefficient storage.
        u32 bpr = mcuColumnsCount * components[componentIndex].horizontalSampling;
        u32 bpc = mcuRowsCount * components[componentIndex].verticalSampling;
        components[componentIndex].blocksPerRow = bpr;
        components[componentIndex].blocksPerCol = bpc;
        if (isProgressive) {
            std::array<i32, BLOCK_PIXELS> zeroBlock{};
            components[componentIndex].coefficientBlocks.assign(static_cast<size_t>(bpr) * bpc,
                                                                zeroBlock);
        }
    }
    return true;
}

/// DHT — Define Huffman Table (ITU-T T.81, Section B.2.4.2)
bool JpegDecoder::parseHuffmanTable(size_t dataOffset, size_t dataLength) {
    size_t endOffset = dataOffset + dataLength;
    while (dataOffset < endOffset) {
        if (dataOffset >= bitstream.size) {
            return reportError("DHT: unexpected end of data");
        }
        u8 tableInfo = bitstream.data[dataOffset++];
        i32 tableClass = (tableInfo >> 4) & 0x0F; // 0 = DC, 1 = AC
        i32 tableIndex = tableInfo & 0x0F;
        if (tableClass > 1 || tableIndex >= MAX_TABLES) {
            return reportError("DHT: invalid table class/index");
        }

        if (dataOffset + 16 > endOffset) {
            return reportError("DHT: code length counts truncated");
        }
        std::array<u8, 16> codeLengthCounts{};
        std::memcpy(codeLengthCounts.data(), bitstream.data + dataOffset, 16);
        dataOffset += 16;

        i32 totalSymbols = 0;
        for (i32 lengthIndex = 0; lengthIndex < 16; lengthIndex++) {
            totalSymbols += codeLengthCounts[lengthIndex];
        }
        if (totalSymbols > 256 || dataOffset + totalSymbols > endOffset) {
            return reportError("DHT: symbol table truncated");
        }

        if (tableClass == 0) {
            dcHuffmanTables[tableIndex].build(codeLengthCounts, bitstream.data + dataOffset);
        } else {
            acHuffmanTables[tableIndex].build(codeLengthCounts, bitstream.data + dataOffset);
        }
        dataOffset += totalSymbols;
    }
    return true;
}

/// DRI — Define Restart Interval (ITU-T T.81, Section B.2.4.4)
bool JpegDecoder::parseRestartInterval(size_t dataOffset, size_t dataLength) {
    if (dataLength < 2) {
        return reportError("DRI: segment too short");
    }
    restartInterval = readBigEndianU16(dataOffset);
    return true;
}

/// SOS — Start of Scan header (ITU-T T.81, Section B.2.3)
bool JpegDecoder::parseScanHeader(size_t dataOffset, size_t dataLength, size_t& scanDataStart) {
    if (dataLength < 1) {
        return reportError("SOS: segment too short");
    }
    u8 scanComponentCount = bitstream.data[dataOffset];
    if (scanComponentCount == 0 || scanComponentCount > componentCount) {
        return reportError("SOS: invalid scan component count " +
                           std::to_string(scanComponentCount));
    }
    if (!isProgressive && scanComponentCount != componentCount) {
        return reportError("SOS: baseline scan component count does not match frame header");
    }
    if (dataLength < 1 + scanComponentCount * 2 + 3) {
        return reportError("SOS: segment too short for component selectors");
    }

    scanComponentIndices.clear();
    for (u32 scanIndex = 0; scanIndex < scanComponentCount; scanIndex++) {
        u8 selectorId = bitstream.data[dataOffset + 1 + scanIndex * 2];
        u8 tableSelectors = bitstream.data[dataOffset + 1 + scanIndex * 2 + 1];

        bool foundMatchingComponent = false;
        for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
            if (components[componentIndex].componentId == selectorId) {
                components[componentIndex].dcHuffmanIndex = (tableSelectors >> 4) & 0x0F;
                components[componentIndex].acHuffmanIndex = tableSelectors & 0x0F;
                if (components[componentIndex].dcHuffmanIndex >= MAX_TABLES ||
                    components[componentIndex].acHuffmanIndex >= MAX_TABLES) {
                    return reportError("SOS: Huffman table index out of range");
                }
                scanComponentIndices.push_back(componentIndex);
                foundMatchingComponent = true;
                break;
            }
        }
        if (!foundMatchingComponent) {
            return reportError("SOS: no matching component for selector id " +
                               std::to_string(selectorId));
        }
    }

    // Extract spectral selection and successive approximation.
    size_t ssOffset = dataOffset + 1 + scanComponentCount * 2;
    scanSpectralStart = bitstream.data[ssOffset];
    scanSpectralEnd = bitstream.data[ssOffset + 1];
    u8 ahAl = bitstream.data[ssOffset + 2];
    scanApproxHigh = (ahAl >> 4) & 0x0F;
    scanApproxLow = ahAl & 0x0F;

    if (isProgressive) {
        if (scanSpectralStart > 63 || scanSpectralEnd > 63 || scanSpectralStart > scanSpectralEnd) {
            return reportError("SOS: invalid spectral selection range");
        }
        // AC scans must be non-interleaved (single component).
        if (scanSpectralStart > 0 && scanComponentIndices.size() > 1) {
            return reportError("SOS: AC spectral selection requires single-component scan");
        }
    }

    scanDataStart = dataOffset + 1 + scanComponentCount * 2 + 3;
    return true;
}

// ============================================================================
// Entropy-Coded Block Decoding (ITU-T T.81, Section F.2.2)
// ============================================================================

/// Decode one 8x8 DCT block from the Huffman-coded bitstream, dequantise the
/// coefficients using the quantisation table, and store them in natural
/// (row-major) order via the zig-zag index table.
bool JpegDecoder::decodeDctBlock(std::array<i32, BLOCK_PIXELS>& coefficients,
                                 const HuffmanTable& dcTable, const HuffmanTable& acTable,
                                 i32& dcPrediction,
                                 const std::array<i16, BLOCK_PIXELS>& quantTable) {
    coefficients.fill(0);

    // DC coefficient: decode category, read magnitude bits, update prediction.
    i32 dcCategory = dcTable.decodeSymbol(bitstream);
    if (dcCategory < 0) {
        return reportError("Huffman DC decode error");
    }
    i32 dcDifference = 0;
    if (dcCategory > 0) {
        u32 magnitudeBits = bitstream.readBits(dcCategory);
        dcDifference = extend_magnitude_to_signed(magnitudeBits, dcCategory);
    }
    dcPrediction += dcDifference;
    coefficients[0] = dcPrediction * quantTable[0]; // Zig-zag position 0 == natural position 0.

    // AC coefficients: decode run-length/category pairs.
    i32 coefficientIndex = 1;
    while (coefficientIndex < BLOCK_PIXELS) {
        i32 runLengthCategory = acTable.decodeSymbol(bitstream);
        if (runLengthCategory < 0) {
            return reportError("Huffman AC decode error");
        }
        i32 zeroRunLength = (runLengthCategory >> 4) & 0x0F;
        i32 acCategory = runLengthCategory & 0x0F;

        if (acCategory == 0) {
            if (zeroRunLength == 0) {
                break; // EOB (End Of Block): remaining coefficients are zero.
            }
            if (zeroRunLength == 15) {
                coefficientIndex += 16; // ZRL: skip 16 zero coefficients.
                continue;
            }
            break; // Invalid encoding; treat as EOB.
        }

        coefficientIndex += zeroRunLength;
        if (coefficientIndex >= BLOCK_PIXELS) {
            return reportError("AC coefficient index out of range");
        }
        u32 magnitudeBits = bitstream.readBits(acCategory);
        i32 acValue = extend_magnitude_to_signed(magnitudeBits, acCategory);
        // Dequantise and place at the natural-order position using the zig-zag map.
        coefficients[ZIGZAG_ORDER[coefficientIndex]] = acValue * quantTable[coefficientIndex];
        coefficientIndex++;
    }
    return true;
}

// ============================================================================
// Scan Data Decoding
// ============================================================================

/// Decode all MCUs in the entropy-coded scan segment.
bool JpegDecoder::decodeScanData() {
    // Validate that all referenced Huffman and quantisation tables are present.
    for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
        if (!dcHuffmanTables[components[componentIndex].dcHuffmanIndex].isBuilt ||
            !acHuffmanTables[components[componentIndex].acHuffmanIndex].isBuilt) {
            return reportError("Missing Huffman table for component " +
                               std::to_string(componentIndex));
        }
        if (!quantTablePresent[components[componentIndex].quantTableIndex]) {
            return reportError("Missing quantisation table for component " +
                               std::to_string(componentIndex));
        }
    }

    u32 mcuSequenceIndex = 0;
    for (u32 mcuRow = 0; mcuRow < mcuRowsCount; mcuRow++) {
        for (u32 mcuColumn = 0; mcuColumn < mcuColumnsCount; mcuColumn++) {
            // Handle restart markers: reset DC predictions and align the bitstream.
            if (restartInterval > 0 && mcuSequenceIndex > 0 &&
                (mcuSequenceIndex % restartInterval) == 0) {
                for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
                    components[componentIndex].dcPrediction = 0;
                }
                bitstream.handleRestartMarker();
            }

            // Decode every 8x8 block within this MCU, for each component.
            for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
                auto& component = components[componentIndex];
                for (u32 blockRow = 0; blockRow < component.verticalSampling; blockRow++) {
                    for (u32 blockColumn = 0; blockColumn < component.horizontalSampling;
                         blockColumn++) {
                        // No zero-init needed: decodeDctBlock starts with fill(0).
                        std::array<i32, BLOCK_PIXELS> dctCoefficients;  // NOLINT(cppcoreguidelines-pro-type-member-init)
                        if (!decodeDctBlock(
                                dctCoefficients, dcHuffmanTables[component.dcHuffmanIndex],
                                acHuffmanTables[component.acHuffmanIndex], component.dcPrediction,
                                quantTables[component.quantTableIndex])) {
                            return false;
                        }

                        // Write the IDCT output into the component sample buffer.
                        u32 blockPixelX =
                            (mcuColumn * component.horizontalSampling + blockColumn) * BLOCK_SIZE;
                        u32 blockPixelY =
                            (mcuRow * component.verticalSampling + blockRow) * BLOCK_SIZE;
                        inverse_dct_block(dctCoefficients,
                                          component.sampleBuffer.data() +
                                              blockPixelY * component.sampleBufferStride +
                                              blockPixelX,
                                          component.sampleBufferStride);
                    }
                }
            }
            mcuSequenceIndex++;
        }
    }
    return true;
}

/// Decode baseline entropy data into coefficient blocks (no IDCT).
/// Allocates progressive-style coefficient storage so that finalizeProgressiveImage()
/// can run IDCT in parallel afterwards.
bool JpegDecoder::decodeScanDataToCoefficients() {
    // Validate tables (same as decodeScanData).
    for (u32 ci = 0; ci < componentCount; ci++) {
        if (!dcHuffmanTables[components[ci].dcHuffmanIndex].isBuilt ||
            !acHuffmanTables[components[ci].acHuffmanIndex].isBuilt) {
            return reportError("Missing Huffman table for component " + std::to_string(ci));
        }
        if (!quantTablePresent[components[ci].quantTableIndex]) {
            return reportError("Missing quantisation table for component " + std::to_string(ci));
        }
    }

    // Allocate coefficient blocks (same layout as progressive mode).
    for (u32 ci = 0; ci < componentCount; ci++) {
        auto& comp = components[ci];
        comp.blocksPerRow = mcuColumnsCount * comp.horizontalSampling;
        comp.blocksPerCol = mcuRowsCount * comp.verticalSampling;
        u32 totalBlocks = comp.blocksPerRow * comp.blocksPerCol;
        static constexpr std::array<i32, BLOCK_PIXELS> kZeroBlock{};
        comp.coefficientBlocks.assign(totalBlocks, kZeroBlock);
    }

    u32 mcuSequenceIndex = 0;
    for (u32 mcuRow = 0; mcuRow < mcuRowsCount; mcuRow++) {
        for (u32 mcuCol = 0; mcuCol < mcuColumnsCount; mcuCol++) {
            if (restartInterval > 0 && mcuSequenceIndex > 0 &&
                (mcuSequenceIndex % restartInterval) == 0) {
                for (u32 ci = 0; ci < componentCount; ci++) {
                    components[ci].dcPrediction = 0;
                }
                bitstream.handleRestartMarker();
            }

            for (u32 ci = 0; ci < componentCount; ci++) {
                auto& comp = components[ci];
                for (u32 blockRow = 0; blockRow < comp.verticalSampling; blockRow++) {
                    for (u32 blockCol = 0; blockCol < comp.horizontalSampling; blockCol++) {
                        u32 bx = mcuCol * comp.horizontalSampling + blockCol;
                        u32 by = mcuRow * comp.verticalSampling + blockRow;
                        auto& coeffs = comp.coefficientBlocks[by * comp.blocksPerRow + bx];

                        // Decode DC.
                        const auto& dcTable = dcHuffmanTables[comp.dcHuffmanIndex];
                        i32 dcCategory = dcTable.decodeSymbol(bitstream);
                        if (dcCategory < 0)
                            return reportError("Huffman DC decode error");
                        i32 dcDifference = 0;
                        if (dcCategory > 0) {
                            u32 magnitudeBits = bitstream.readBits(dcCategory);
                            dcDifference = extend_magnitude_to_signed(magnitudeBits, dcCategory);
                        }
                        comp.dcPrediction += dcDifference;
                        coeffs[0] = comp.dcPrediction;

                        // Decode AC.
                        const auto& acTable = acHuffmanTables[comp.acHuffmanIndex];
                        i32 coeffIdx = 1;
                        while (coeffIdx < BLOCK_PIXELS) {
                            i32 sym = acTable.decodeSymbol(bitstream);
                            if (sym < 0)
                                return reportError("Huffman AC decode error");
                            i32 run = (sym >> 4) & 0x0F;
                            i32 cat = sym & 0x0F;
                            if (cat == 0) {
                                if (run == 0)
                                    break; // EOB
                                if (run == 15) {
                                    coeffIdx += 16;
                                    continue;
                                } // ZRL
                                break;
                            }
                            coeffIdx += run;
                            if (coeffIdx >= BLOCK_PIXELS) {
                                return reportError("AC coefficient index out of range");
                            }
                            u32 mag = bitstream.readBits(cat);
                            i32 acVal = extend_magnitude_to_signed(mag, cat);
                            // Store in zig-zag order (coefficients are NOT dequantized
                            // here — finalizeProgressiveImage handles dequantization).
                            coeffs[coeffIdx] = acVal;
                            coeffIdx++;
                        }
                    }
                }
            }
            mcuSequenceIndex++;
        }
    }
    return true;
}

/// Parallel baseline entropy decode using restart intervals.
/// Pre-scans raw bytes for RST marker positions, then decodes each interval
/// independently in parallel — fusing entropy decode + dequantise + IDCT.
/// When directOutput is non-null, writes interleaved pixels directly to the
/// output image (only works for the no-subsampling case: all h/v sampling = 1).
bool JpegDecoder::decodeScanDataParallel(Image* directOutput) {
    // Validate tables.
    for (u32 ci = 0; ci < componentCount; ci++) {
        if (!dcHuffmanTables[components[ci].dcHuffmanIndex].isBuilt ||
            !acHuffmanTables[components[ci].acHuffmanIndex].isBuilt) {
            return reportError("Missing Huffman table for component " + std::to_string(ci));
        }
        if (!quantTablePresent[components[ci].quantTableIndex]) {
            return reportError("Missing quantisation table for component " + std::to_string(ci));
        }
    }

    // Pre-allocate the output image for direct-output mode.
    if (directOutput) {
        directOutput->width = imageWidth;
        directOutput->height = imageHeight;
        directOutput->components = componentCount;
        directOutput->pixels.resize(static_cast<size_t>(imageWidth) * imageHeight * componentCount);
    }

    // Pre-scan raw bytes to find restart marker byte positions.
    auto intervalStarts = std::make_shared<std::vector<size_t>>();
    intervalStarts->push_back(bitstream.bytePos);
    {
        size_t pos = bitstream.bytePos;
        while (pos + 1 < bitstream.size) {
            if (bitstream.data[pos] == 0xFF) {
                u8 next = bitstream.data[pos + 1];
                if (next == 0x00) {
                    pos += 2;
                    continue;
                }
                if (next >= MARKER_RST0 && next <= MARKER_RST0 + 7) {
                    pos += 2;
                    intervalStarts->push_back(pos);
                    continue;
                }
                break; // real marker — end of entropy data
            }
            ++pos;
        }
    }

    const u32 nIntervals = static_cast<u32>(intervalStarts->size());
    const u32 mcusPerInterval = restartInterval;
    const u32 totalMCUs = mcuColumnsCount * mcuRowsCount;
    const u32 mcuCols = mcuColumnsCount;
    const u32 compCnt = componentCount;

    // Capture per-component layout + table info for the parallel lambda.
    struct CompInfo {
        u8 dcHuffIdx;
        u8 acHuffIdx;
        u8 quantIdx;
        u8 hSampling;
        u8 vSampling;
        u32 sampleStride;
    };
    std::array<CompInfo, MAX_COMPONENTS> compInfo{};
    for (u32 ci = 0; ci < compCnt; ++ci) {
        compInfo[ci] = {components[ci].dcHuffmanIndex,   components[ci].acHuffmanIndex,
                        components[ci].quantTableIndex,  components[ci].horizontalSampling,
                        components[ci].verticalSampling, components[ci].sampleBufferStride};
    }

    if (directOutput) {
        // Fused path: decode + dequant + IDCT → temp blocks → interleave → output.
        // Skips sample buffers entirely for the no-subsampling case.
        u8* const outputData = directOutput->pixels.data();
        const u32 imgW = imageWidth;
        const u32 imgH = imageHeight;

        parallel_for_blocks(
            nIntervals, ctx,
            [this, intervalStarts, mcusPerInterval, totalMCUs, mcuCols, compCnt, compInfo,
             outputData, imgW, imgH](u32 intBegin, u32 intEnd) {
                for (u32 interval = intBegin; interval < intEnd; ++interval) {
                    BitstreamReader localBs;
                    localBs.init(bitstream.data, bitstream.size, (*intervalStarts)[interval]);

                    std::array<i32, MAX_COMPONENTS> dcPreds{};
                    u32 mcuStart = interval * mcusPerInterval;
                    u32 mcuEnd = std::min(mcuStart + mcusPerInterval, totalMCUs);

                    for (u32 mcuIdx = mcuStart; mcuIdx < mcuEnd; ++mcuIdx) {
                        u32 mcuRow = mcuIdx / mcuCols;
                        u32 mcuCol = mcuIdx % mcuCols;

                        // Decode all component blocks for this MCU into temp arrays.
                        std::array<std::array<u8, BLOCK_PIXELS>, MAX_COMPONENTS> blks;  // NOLINT(cppcoreguidelines-pro-type-member-init)
                        for (u32 ci = 0; ci < compCnt; ++ci) {
                            const auto& info = compInfo[ci];
                            const auto& dcTable = dcHuffmanTables[info.dcHuffIdx];
                            const auto& acTable = acHuffmanTables[info.acHuffIdx];
                            const auto& qt = quantTables[info.quantIdx];

                            std::array<i32, BLOCK_PIXELS> dequant{};

                            i32 dcCat = dcTable.decodeSymbol(localBs);
                            i32 dcDiff = 0;
                            if (dcCat > 0) {
                                u32 mag = localBs.readBits(dcCat);
                                dcDiff = extend_magnitude_to_signed(mag, dcCat);
                            }
                            dcPreds[ci] += dcDiff;
                            dequant[0] = dcPreds[ci] * qt[0];

                            i32 k = 1;
                            while (k < BLOCK_PIXELS) {
                                i32 sym = acTable.decodeSymbol(localBs);
                                i32 run = (sym >> 4) & 0x0F;
                                i32 cat = sym & 0x0F;
                                if (cat == 0) {
                                    if (run == 15) {
                                        k += 16;
                                        continue;
                                    }
                                    break;
                                }
                                k += run;
                                if (k >= BLOCK_PIXELS)
                                    break;
                                u32 mag = localBs.readBits(cat);
                                i32 val = extend_magnitude_to_signed(mag, cat);
                                dequant[ZIGZAG_ORDER[k]] = val * qt[k];
                                k++;
                            }

                            inverse_dct_block(dequant, blks[ci].data(), BLOCK_SIZE);
                        }

                        // Interleave directly to output image.
                        u32 px = mcuCol * BLOCK_SIZE;
                        u32 py = mcuRow * BLOCK_SIZE;
                        u32 rowEnd = std::min(py + static_cast<u32>(BLOCK_SIZE), imgH);
                        u32 cols = std::min(px + static_cast<u32>(BLOCK_SIZE), imgW) - px;

                        for (u32 y = py; y < rowEnd; ++y) {
                            u8* dst = outputData + (static_cast<size_t>(y) * imgW + px) * compCnt;
                            u32 bRow = y - py;
                            if (compCnt == 4) {
                                const u8* s0 = blks[0].data() + bRow * BLOCK_SIZE;
                                const u8* s1 = blks[1].data() + bRow * BLOCK_SIZE;
                                const u8* s2 = blks[2].data() + bRow * BLOCK_SIZE;
                                const u8* s3 = blks[3].data() + bRow * BLOCK_SIZE;
                                for (u32 x = 0; x < cols; ++x) {
                                    dst[x * 4] = s0[x];
                                    dst[x * 4 + 1] = s1[x];
                                    dst[x * 4 + 2] = s2[x];
                                    dst[x * 4 + 3] = s3[x];
                                }
                            } else if (compCnt == 3) {
                                const u8* s0 = blks[0].data() + bRow * BLOCK_SIZE;
                                const u8* s1 = blks[1].data() + bRow * BLOCK_SIZE;
                                const u8* s2 = blks[2].data() + bRow * BLOCK_SIZE;
                                for (u32 x = 0; x < cols; ++x) {
                                    dst[x * 3] = s0[x];
                                    dst[x * 3 + 1] = s1[x];
                                    dst[x * 3 + 2] = s2[x];
                                }
                            } else if (compCnt == 1) {
                                std::memcpy(dst, blks[0].data() + bRow * BLOCK_SIZE, cols);
                            } else {
                                for (u32 x = 0; x < cols; ++x) {
                                    for (u32 c = 0; c < compCnt; ++c) {
                                        dst[x * compCnt + c] = blks[c][bRow * BLOCK_SIZE + x];
                                    }
                                }
                            }
                        }
                    }
                }
            });
    } else {
        // Standard path: decode + dequant + IDCT → per-component sample buffers.
        parallel_for_blocks(
            nIntervals, ctx,
            [this, intervalStarts, mcusPerInterval, totalMCUs, mcuCols, compCnt,
             compInfo](u32 intBegin, u32 intEnd) {
                for (u32 interval = intBegin; interval < intEnd; ++interval) {
                    BitstreamReader localBs;
                    localBs.init(bitstream.data, bitstream.size, (*intervalStarts)[interval]);

                    std::array<i32, MAX_COMPONENTS> dcPreds{};
                    u32 mcuStart = interval * mcusPerInterval;
                    u32 mcuEnd = std::min(mcuStart + mcusPerInterval, totalMCUs);

                    for (u32 mcuIdx = mcuStart; mcuIdx < mcuEnd; ++mcuIdx) {
                        u32 mcuRow = mcuIdx / mcuCols;
                        u32 mcuCol = mcuIdx % mcuCols;

                        for (u32 ci = 0; ci < compCnt; ++ci) {
                            const auto& info = compInfo[ci];
                            const auto& dcTable = dcHuffmanTables[info.dcHuffIdx];
                            const auto& acTable = acHuffmanTables[info.acHuffIdx];
                            const auto& qt = quantTables[info.quantIdx];

                            for (u32 br = 0; br < info.vSampling; ++br) {
                                for (u32 bc = 0; bc < info.hSampling; ++bc) {
                                    std::array<i32, BLOCK_PIXELS> dequant{};

                                    i32 dcCat = dcTable.decodeSymbol(localBs);
                                    i32 dcDiff = 0;
                                    if (dcCat > 0) {
                                        u32 mag = localBs.readBits(dcCat);
                                        dcDiff = extend_magnitude_to_signed(mag, dcCat);
                                    }
                                    dcPreds[ci] += dcDiff;
                                    dequant[0] = dcPreds[ci] * qt[0];

                                    i32 k = 1;
                                    while (k < BLOCK_PIXELS) {
                                        i32 sym = acTable.decodeSymbol(localBs);
                                        i32 run = (sym >> 4) & 0x0F;
                                        i32 cat = sym & 0x0F;
                                        if (cat == 0) {
                                            if (run == 15) {
                                                k += 16;
                                                continue;
                                            }
                                            break;
                                        }
                                        k += run;
                                        if (k >= BLOCK_PIXELS)
                                            break;
                                        u32 mag = localBs.readBits(cat);
                                        i32 val = extend_magnitude_to_signed(mag, cat);
                                        dequant[ZIGZAG_ORDER[k]] = val * qt[k];
                                        k++;
                                    }

                                    u32 px = (mcuCol * info.hSampling + bc) * BLOCK_SIZE;
                                    u32 py = (mcuRow * info.vSampling + br) * BLOCK_SIZE;
                                    inverse_dct_block(dequant,
                                                      components[ci].sampleBuffer.data() +
                                                          py * info.sampleStride + px,
                                                      info.sampleStride);
                                }
                            }
                        }
                    }
                }
            });
    }

    return true;
}

// ============================================================================
// Progressive Scan Decoding (ITU-T T.81, Sections G.1–G.2)
// ============================================================================

/// Decode DC first-pass coefficient for one block in a progressive scan.
/// Ss=0, Se=0, Ah=0 — initial DC coefficient with point transform Al.
bool JpegDecoder::decodeProgressiveDcFirst(u32 compIdx, std::array<i32, BLOCK_PIXELS>& coeffs,
                                           i32& dcPrediction) {
    const auto& dcTable = dcHuffmanTables[components[compIdx].dcHuffmanIndex];
    i32 dcCategory = dcTable.decodeSymbol(bitstream);
    if (dcCategory < 0) {
        return reportError("Progressive DC first: Huffman decode error");
    }
    i32 dcDifference = 0;
    if (dcCategory > 0) {
        u32 magnitudeBits = bitstream.readBits(dcCategory);
        dcDifference = extend_magnitude_to_signed(magnitudeBits, dcCategory);
    }
    dcPrediction += dcDifference;
    coeffs[0] = dcPrediction << scanApproxLow;
    return true;
}

/// Decode DC refinement for one block in a progressive scan.
/// Ss=0, Se=0, Ah>0 — read one correction bit per block.
bool JpegDecoder::decodeProgressiveDcRefine(std::array<i32, BLOCK_PIXELS>& coeffs) {
    u32 bit = bitstream.readBits(1);
    coeffs[0] |= static_cast<i32>(bit) << scanApproxLow;
    return true;
}

/// Decode AC first-pass coefficients for one block in a progressive scan.
/// Ss>0, Ah=0 — initial AC coefficients in range [Ss, Se] with point transform Al.
bool JpegDecoder::decodeProgressiveAcFirst(BitstreamReader& bs,
                                           std::array<i32, BLOCK_PIXELS>& coeffs,
                                           const HuffmanTable& acTable, u32& eobRunRef, u8 ss, u8 se,
                                           u8 al) {
    if (eobRunRef > 0) {
        --eobRunRef;
        return true;
    }

    for (i32 k = ss; k <= se; ++k) {
        i32 symbol = acTable.decodeSymbol(bs);
        if (symbol < 0) {
            return reportError("Progressive AC first: Huffman decode error");
        }
        i32 runLength = (symbol >> 4) & 0x0F;
        i32 category = symbol & 0x0F;

        if (category == 0) {
            if (runLength == 15) {
                k += 15; // ZRL: skip 16 positions (loop increments once more).
                continue;
            }
            // EOBn: End of band for 2^runLength blocks.
            eobRunRef = (1u << runLength);
            if (runLength > 0) {
                eobRunRef += bs.readBits(runLength);
            }
            --eobRunRef; // Current block counts as one.
            return true;
        }

        k += runLength;
        if (k > se) {
            return reportError("Progressive AC first: coefficient index out of range");
        }
        u32 magnitudeBits = bs.readBits(category);
        i32 value = extend_magnitude_to_signed(magnitudeBits, category);
        coeffs[k] = value << al;
    }
    return true;
}

/// Apply one correction bit to a previously-nonzero coefficient (branchless).
/// If the read bit is 1, the coefficient magnitude is increased by correctionBit
/// (added for positive values, subtracted for negative).
inline void applyRefinementBit(BitstreamReader& bs, i32& coeff, i32 correctionBit) {
    const i32 bit = static_cast<i32>(bs.readBits(1));
    // sign = +1 for positive coeff, -1 for negative (coeff is always nonzero here).
    const i32 sign = (coeff >> 31) | 1;
    coeff += bit * sign * correctionBit;
}

/// Decode AC refinement for one block in a progressive scan.
/// Ss>0, Ah>0 — refine previously-coded AC coefficients in range [Ss, Se].
///
/// This is the most complex progressive pass. For each block:
///  - Previously-zero coefficients may become nonzero (category=1) or stay zero (run skip).
///  - Previously-nonzero coefficients receive one correction bit per pass.
///  - EOBRUN mechanism skips remaining zero positions.
bool JpegDecoder::decodeProgressiveAcRefine(BitstreamReader& bs,
                                            std::array<i32, BLOCK_PIXELS>& coeffs,
                                            const HuffmanTable& acTable, u32& eobRunRef, u8 ss, u8 se,
                                            u8 al) {
    i32 k = ss;
    i32 correctionBit = 1 << al;

    if (eobRunRef > 0) {
        // In an EOB run: just refine existing nonzero coefficients.
        for (; k <= se; ++k) {
            if (coeffs[k] != 0) {
                applyRefinementBit(bs, coeffs[k], correctionBit);
            }
        }
        --eobRunRef;
        return true;
    }

    for (; k <= se; ++k) {
        i32 symbol = acTable.decodeSymbol(bs);
        if (symbol < 0) {
            return reportError("Progressive AC refine: Huffman decode error");
        }
        i32 runLength = (symbol >> 4) & 0x0F;
        i32 category = symbol & 0x0F;

        i32 newValue = 0; // Will be set if a new nonzero coefficient is created.
        if (category == 0) {
            if (runLength < 15) {
                // EOBn.
                eobRunRef = (1u << runLength);
                if (runLength > 0) {
                    eobRunRef += bs.readBits(runLength);
                }
                // Refine remaining nonzero coefficients in this block, then done.
                for (; k <= se; ++k) {
                    if (coeffs[k] != 0) {
                        applyRefinementBit(bs, coeffs[k], correctionBit);
                    }
                }
                --eobRunRef;
                return true;
            }
            // runLength == 15: ZRL with no new nonzero — skip 16 zero positions.
        } else if (category == 1) {
            // New nonzero coefficient: read sign bit (branchless).
            // signBit=1 → +correctionBit, signBit=0 → -correctionBit.
            const i32 signBit = static_cast<i32>(bs.readBits(1));
            newValue = (signBit * 2 - 1) * correctionBit;
        } else {
            return reportError("Progressive AC refine: unexpected category " +
                               std::to_string(category));
        }

        // Skip `runLength` zero-valued positions, refining nonzero ones along the way.
        i32 zerosToSkip = runLength;
        for (; k <= se; ++k) {
            if (coeffs[k] != 0) {
                // Refine existing nonzero coefficient.
                applyRefinementBit(bs, coeffs[k], correctionBit);
            } else {
                if (zerosToSkip == 0) {
                    break; // Found the target zero position.
                }
                --zerosToSkip;
            }
        }

        // Place the new nonzero coefficient (if any) at position k.
        if (newValue != 0 && k <= se) {
            coeffs[k] = newValue;
        }
    }
    return true;
}

/// Decode one progressive scan (all MCUs). Dispatches to the appropriate
/// progressive decode function based on Ss, Se, Ah, Al values.
bool JpegDecoder::decodeProgressiveScan() {
    bool isDcScan = (scanSpectralStart == 0);
    bool isFirstPass = (scanApproxHigh == 0);

    // Validate Huffman and quant tables for scan components.
    for (u32 ci : scanComponentIndices) {
        if (isDcScan) {
            if (!dcHuffmanTables[components[ci].dcHuffmanIndex].isBuilt) {
                return reportError("Missing DC Huffman table for progressive scan");
            }
        }
        if (!isDcScan || scanSpectralEnd > 0) {
            if (!acHuffmanTables[components[ci].acHuffmanIndex].isBuilt) {
                return reportError("Missing AC Huffman table for progressive scan");
            }
        }
    }

    eobRun = 0;

    if (isDcScan) {
        // DC scan — may be interleaved (multiple components).
        u32 mcuSequenceIndex = 0;
        for (u32 mcuRow = 0; mcuRow < mcuRowsCount; ++mcuRow) {
            for (u32 mcuColumn = 0; mcuColumn < mcuColumnsCount; ++mcuColumn) {
                if (restartInterval > 0 && mcuSequenceIndex > 0 &&
                    (mcuSequenceIndex % restartInterval) == 0) {
                    for (u32 ci : scanComponentIndices) {
                        components[ci].dcPrediction = 0;
                    }
                    bitstream.handleRestartMarker();
                    eobRun = 0;
                }

                for (u32 ci : scanComponentIndices) {
                    auto& comp = components[ci];
                    for (u32 blockRow = 0; blockRow < comp.verticalSampling; ++blockRow) {
                        for (u32 blockCol = 0; blockCol < comp.horizontalSampling; ++blockCol) {
                            u32 bx = mcuColumn * comp.horizontalSampling + blockCol;
                            u32 by = mcuRow * comp.verticalSampling + blockRow;
                            auto& coeffs = comp.coefficientBlocks[by * comp.blocksPerRow + bx];

                            if (isFirstPass) {
                                if (!decodeProgressiveDcFirst(ci, coeffs, comp.dcPrediction))
                                    return false;
                            } else {
                                if (!decodeProgressiveDcRefine(coeffs))
                                    return false;
                            }
                        }
                    }
                }
                ++mcuSequenceIndex;
            }
        }
    } else {
        // AC scan — always single component.
        u32 ci = scanComponentIndices[0];
        auto& comp = components[ci];
        const auto& acTable = acHuffmanTables[comp.acHuffmanIndex];

        // For non-interleaved scans the MCU is a single block.
        u32 totalBlocksX = comp.blocksPerRow;
        u32 totalBlocksY = comp.blocksPerCol;
        u32 mcuSequenceIndex = 0;

        for (u32 by = 0; by < totalBlocksY; ++by) {
            for (u32 bx = 0; bx < totalBlocksX; ++bx) {
                if (restartInterval > 0 && mcuSequenceIndex > 0 &&
                    (mcuSequenceIndex % restartInterval) == 0) {
                    bitstream.handleRestartMarker();
                    eobRun = 0;
                }

                auto& coeffs = comp.coefficientBlocks[by * totalBlocksX + bx];

                if (isFirstPass) {
                    if (!decodeProgressiveAcFirst(bitstream, coeffs, acTable, eobRun,
                                                  scanSpectralStart, scanSpectralEnd,
                                                  scanApproxLow))
                        return false;
                } else {
                    if (!decodeProgressiveAcRefine(bitstream, coeffs, acTable, eobRun,
                                                   scanSpectralStart, scanSpectralEnd,
                                                   scanApproxLow))
                        return false;
                }
                ++mcuSequenceIndex;
            }
        }
    }
    return true;
}

/// After all progressive scans: dequantize coefficient buffers and run IDCT
/// to produce the final pixel data in each component's sample buffer.
bool JpegDecoder::finalizeProgressiveImage() {
    // Flatten all blocks across all components into a single index range
    // for parallel dispatch.  Each block's IDCT is independent.
    u32 totalBlocks = 0;
    std::array<u32, MAX_COMPONENTS> blockOffsets{};
    for (u32 ci = 0; ci < componentCount; ++ci) {
        blockOffsets[ci] = totalBlocks;
        totalBlocks += components[ci].blocksPerRow * components[ci].blocksPerCol;
    }

    // Capture a snapshot of per-component layout information for the lambda.
    struct CompInfo {
        u32 blocksPerRow;
        u32 blocksPerCol;
        u32 sampleBufferStride;
        u32 blockOffset;
        u8 quantTableIndex;
    };
    std::array<CompInfo, MAX_COMPONENTS> info{};
    for (u32 ci = 0; ci < componentCount; ++ci) {
        info[ci] = {components[ci].blocksPerRow, components[ci].blocksPerCol,
                    components[ci].sampleBufferStride, blockOffsets[ci],
                    components[ci].quantTableIndex};
    }

    // Precompute natural-to-zigzag index map so the inner loop can use a
    // contiguous-write gather pattern instead of a random-write scatter.
    // NATURAL_TO_ZIGZAG[naturalPos] = zigzagPos.
    std::array<u8, BLOCK_PIXELS> naturalToZigzag{};
    for (i32 z = 0; z < BLOCK_PIXELS; ++z) {
        naturalToZigzag[ZIGZAG_ORDER[z]] = static_cast<u8>(z);
    }

    // Precompute natural-order quantisation tables for each unique quant table
    // so the per-block loop avoids the scatter entirely.
    std::array<std::array<i16, BLOCK_PIXELS>, MAX_TABLES> naturalQuantTables{};
    for (u32 ci = 0; ci < componentCount; ++ci) {
        const u8 qi = info[ci].quantTableIndex;
        const auto& qt = quantTables[qi];
        auto& nqt = naturalQuantTables[qi];
        for (i32 z = 0; z < BLOCK_PIXELS; ++z) {
            nqt[ZIGZAG_ORDER[z]] = qt[z];
        }
    }

    const u32 compCnt = componentCount;

    parallel_for_blocks(
        totalBlocks, ctx,
        [this, compCnt, info, naturalToZigzag, naturalQuantTables](u32 flatBegin, u32 flatEnd) {
            for (u32 flat = flatBegin; flat < flatEnd; ++flat) {
                // Find which component and which block within it.
                u32 ci = 0;
                while (ci + 1 < compCnt && flat >= info[ci + 1].blockOffset) {
                    ++ci;
                }
                u32 localIdx = flat - info[ci].blockOffset;
                u32 bx = localIdx % info[ci].blocksPerRow;
                u32 by = localIdx / info[ci].blocksPerRow;

                auto& comp = components[ci];
                auto& coeffs = comp.coefficientBlocks[by * info[ci].blocksPerRow + bx];
                const auto& nqt = naturalQuantTables[info[ci].quantTableIndex];

                // Dequantize in natural (row-major) order: gather from zigzag,
                // multiply by natural-order quant table, write contiguously.
                // The contiguous write pattern is auto-vectorisation friendly.
                std::array<i32, BLOCK_PIXELS> dequantised;  // NOLINT(cppcoreguidelines-pro-type-member-init)
                for (i32 n = 0; n < BLOCK_PIXELS; ++n) {
                    dequantised[n] = coeffs[naturalToZigzag[n]] * nqt[n];
                }

                u32 pixelX = bx * BLOCK_SIZE;
                u32 pixelY = by * BLOCK_SIZE;
                inverse_dct_block(dequantised,
                                  comp.sampleBuffer.data() + pixelY * comp.sampleBufferStride +
                                      pixelX,
                                  comp.sampleBufferStride);
            }
        });

    return true;
}

/// Combined dequantize + IDCT + interleave for the no-subsampling case.
/// Processes blocks by MCU position: for each MCU, all component blocks are
/// dequantised and IDCT'd into stack-allocated temporaries, then interleaved
/// directly into the output image.  Eliminates the intermediate sample buffers
/// and the separate assembly pass.
bool JpegDecoder::finalizeAndAssembleImage(Image& outputImage) {
    outputImage.width = imageWidth;
    outputImage.height = imageHeight;
    outputImage.components = componentCount;
    outputImage.pixels.resize(static_cast<size_t>(imageWidth) * imageHeight * componentCount);

    // Precompute natural-to-zigzag index map.
    std::array<u8, BLOCK_PIXELS> naturalToZigzag{};
    for (i32 z = 0; z < BLOCK_PIXELS; ++z) {
        naturalToZigzag[ZIGZAG_ORDER[z]] = static_cast<u8>(z);
    }

    // Precompute natural-order quantisation tables.
    std::array<std::array<i16, BLOCK_PIXELS>, MAX_TABLES> naturalQuantTables{};
    for (u32 ci = 0; ci < componentCount; ++ci) {
        const u8 qi = components[ci].quantTableIndex;
        const auto& qt = quantTables[qi];
        auto& nqt = naturalQuantTables[qi];
        for (i32 z = 0; z < BLOCK_PIXELS; ++z) {
            nqt[ZIGZAG_ORDER[z]] = qt[z];
        }
    }

    const u32 compCnt = componentCount;
    const u32 imgW = imageWidth;
    const u32 imgH = imageHeight;
    const u32 mcuCols = mcuColumnsCount;
    const u32 totalMCUs = mcuColumnsCount * mcuRowsCount;

    struct CompLayout {
        u32 blocksPerRow;
        u8 quantTableIndex;
    };
    std::array<CompLayout, MAX_COMPONENTS> layout{};
    for (u32 ci = 0; ci < compCnt; ++ci) {
        layout[ci] = {components[ci].blocksPerRow, components[ci].quantTableIndex};
    }

    parallel_for_blocks(
        totalMCUs, ctx,
        [this, compCnt, imgW, imgH, mcuCols, layout, naturalToZigzag, naturalQuantTables,
         &outputImage](u32 mcuBegin, u32 mcuEnd) {
            u8* const pixelBase = outputImage.pixels.data();

            for (u32 mcuIdx = mcuBegin; mcuIdx < mcuEnd; ++mcuIdx) {
                const u32 mcuCol = mcuIdx % mcuCols;
                const u32 mcuRow = mcuIdx / mcuCols;
                const u32 px = mcuCol * BLOCK_SIZE;
                const u32 py = mcuRow * BLOCK_SIZE;

                // Dequantize + IDCT each component block into temp arrays.
                std::array<std::array<u8, BLOCK_PIXELS>, MAX_COMPONENTS> blks;  // NOLINT(cppcoreguidelines-pro-type-member-init)
                for (u32 ci = 0; ci < compCnt; ++ci) {
                    auto& coeffs =
                        components[ci].coefficientBlocks[mcuRow * layout[ci].blocksPerRow + mcuCol];
                    const auto& nqt = naturalQuantTables[layout[ci].quantTableIndex];

                    std::array<i32, BLOCK_PIXELS> dequantised;  // NOLINT(cppcoreguidelines-pro-type-member-init)
                    for (i32 n = 0; n < BLOCK_PIXELS; ++n) {
                        dequantised[n] = coeffs[naturalToZigzag[n]] * nqt[n];
                    }
                    inverse_dct_block(dequantised, blks[ci].data(), BLOCK_SIZE);
                }

                // Interleave all components directly into the output image.
                const u32 rowEnd = std::min(py + static_cast<u32>(BLOCK_SIZE), imgH);
                const u32 cols = std::min(px + static_cast<u32>(BLOCK_SIZE), imgW) - px;

                for (u32 y = py; y < rowEnd; ++y) {
                    u8* dst = pixelBase + (static_cast<size_t>(y) * imgW + px) * compCnt;
                    const u32 bRow = y - py;
                    if (compCnt == 4) {
                        const u8* s0 = blks[0].data() + bRow * BLOCK_SIZE;
                        const u8* s1 = blks[1].data() + bRow * BLOCK_SIZE;
                        const u8* s2 = blks[2].data() + bRow * BLOCK_SIZE;
                        const u8* s3 = blks[3].data() + bRow * BLOCK_SIZE;
                        for (u32 x = 0; x < cols; ++x) {
                            dst[x * 4] = s0[x];
                            dst[x * 4 + 1] = s1[x];
                            dst[x * 4 + 2] = s2[x];
                            dst[x * 4 + 3] = s3[x];
                        }
                    } else if (compCnt == 3) {
                        const u8* s0 = blks[0].data() + bRow * BLOCK_SIZE;
                        const u8* s1 = blks[1].data() + bRow * BLOCK_SIZE;
                        const u8* s2 = blks[2].data() + bRow * BLOCK_SIZE;
                        for (u32 x = 0; x < cols; ++x) {
                            dst[x * 3] = s0[x];
                            dst[x * 3 + 1] = s1[x];
                            dst[x * 3 + 2] = s2[x];
                        }
                    } else if (compCnt == 1) {
                        std::memcpy(dst, blks[0].data() + bRow * BLOCK_SIZE, cols);
                    } else {
                        for (u32 x = 0; x < cols; ++x) {
                            for (u32 c = 0; c < compCnt; ++c) {
                                dst[x * compCnt + c] = blks[c][bRow * BLOCK_SIZE + x];
                            }
                        }
                    }
                }
            }
        });

    return true;
}

// ============================================================================
// Output Assembly (Interleave + Nearest-Neighbour Upsample)
// ============================================================================

/// Combine the per-component sample buffers into a single interleaved image.
/// Components with smaller sampling factors than the maximum are upsampled
/// using nearest-neighbour replication.
bool JpegDecoder::assembleInterleavedImage(Image& outputImage) {
    outputImage.width = imageWidth;
    outputImage.height = imageHeight;
    outputImage.components = componentCount;
    outputImage.pixels.resize(static_cast<size_t>(imageWidth) * imageHeight * componentCount);

    const u32 imgW = imageWidth;
    const u32 compCnt = componentCount;
    const u32 maxHS = maxHorizontalSampling;
    const u32 maxVS = maxVerticalSampling;

    // Capture per-component layout info by value for the lambda.
    struct CompLayout {
        u32 horizontalSampling;
        u32 verticalSampling;
        u32 sampleBufferStride;
        const u8* sampleData;
    };
    std::array<CompLayout, MAX_COMPONENTS> layouts{};
    for (u32 ci = 0; ci < componentCount; ++ci) {
        layouts[ci] = {components[ci].horizontalSampling, components[ci].verticalSampling,
                       components[ci].sampleBufferStride, components[ci].sampleBuffer.data()};
    }

    // Detect the common no-subsampling case (all components match max sampling).
    bool noSubsampling = true;
    for (u32 ci = 0; ci < compCnt; ++ci) {
        if (layouts[ci].horizontalSampling != maxHS || layouts[ci].verticalSampling != maxVS) {
            noSubsampling = false;
            break;
        }
    }

    parallel_for_blocks(
        imageHeight, ctx,
        [&outputImage, imgW, compCnt, maxHS, maxVS, layouts, noSubsampling](u32 rowBegin,
                                                                            u32 rowEnd) {
            u8* const pixelBase = outputImage.pixels.data();

            if (noSubsampling) {
                // Fast path: sampleX == pixelX, sampleY == pixelY for all components.
                // No divisions needed; specialise inner loop by component count
                // so the compiler can unroll and use wider stores.
                for (u32 pixelY = rowBegin; pixelY < rowEnd; ++pixelY) {
                    u8* dest = pixelBase + static_cast<size_t>(pixelY) * imgW * compCnt;
                    std::array<const u8*, MAX_COMPONENTS> srcRow{};
                    for (u32 ci = 0; ci < compCnt; ++ci) {
                        srcRow[ci] =
                            layouts[ci].sampleData + pixelY * layouts[ci].sampleBufferStride;
                    }

                    if (compCnt == 1) {
                        // Grayscale: straight memcpy.
                        std::memcpy(dest, srcRow[0], imgW);
                    } else if (compCnt == 4) {
                        // 4-component (BLP BGRA): explicit interleave enables
                        // the compiler to emit 32-bit packed writes.
                        const u8* s0 = srcRow[0];
                        const u8* s1 = srcRow[1];
                        const u8* s2 = srcRow[2];
                        const u8* s3 = srcRow[3];
                        for (u32 x = 0; x < imgW; ++x) {
                            dest[x * 4] = s0[x];
                            dest[x * 4 + 1] = s1[x];
                            dest[x * 4 + 2] = s2[x];
                            dest[x * 4 + 3] = s3[x];
                        }
                    } else if (compCnt == 3) {
                        const u8* s0 = srcRow[0];
                        const u8* s1 = srcRow[1];
                        const u8* s2 = srcRow[2];
                        for (u32 x = 0; x < imgW; ++x) {
                            dest[x * 3] = s0[x];
                            dest[x * 3 + 1] = s1[x];
                            dest[x * 3 + 2] = s2[x];
                        }
                    } else {
                        for (u32 pixelX = 0; pixelX < imgW; ++pixelX) {
                            for (u32 ci = 0; ci < compCnt; ++ci) {
                                dest[pixelX * compCnt + ci] = srcRow[ci][pixelX];
                            }
                        }
                    }
                }
            } else {
                // General path with upsampling.  Hoist sampleY + row pointer
                // computation out of the inner pixel loop.
                for (u32 pixelY = rowBegin; pixelY < rowEnd; ++pixelY) {
                    u8* dest = pixelBase + static_cast<size_t>(pixelY) * imgW * compCnt;
                    std::array<const u8*, MAX_COMPONENTS> srcRow{};
                    for (u32 ci = 0; ci < compCnt; ++ci) {
                        u32 sampleY = pixelY * layouts[ci].verticalSampling / maxVS;
                        srcRow[ci] =
                            layouts[ci].sampleData + sampleY * layouts[ci].sampleBufferStride;
                    }
                    for (u32 pixelX = 0; pixelX < imgW; ++pixelX) {
                        for (u32 ci = 0; ci < compCnt; ++ci) {
                            u32 sampleX = pixelX * layouts[ci].horizontalSampling / maxHS;
                            dest[pixelX * compCnt + ci] = srcRow[ci][sampleX];
                        }
                    }
                }
            }
        });

    return true;
}

// ============================================================================
// Top-Level: Parse Markers + Decode (ITU-T T.81 Figure E.6)
// ============================================================================

bool JpegDecoder::decode(const u8* data, size_t size, Image& outputImage) {
    bitstream.init(data, size, 0);

    // Verify SOI (Start of Image) marker.
    if (size < 2 || data[0] != 0xFF || data[1] != 0xD8) {
        return reportError("Not a JPEG file (no SOI marker)");
    }
    size_t parsePosition = 2;
    bool foundFrameHeader = false;

    // Helper lambda: skip past the entropy-coded segment after an SOS to find
    // the next marker.  JPEG entropy data uses byte-stuffing (0xFF 0x00) for
    // literal 0xFF values; any 0xFF followed by a non-zero, non-stuffing byte
    // is a marker.
    auto skipEntropyData = [&]() {
        while (parsePosition + 1 < size) {
            if (data[parsePosition] == 0xFF) {
                u8 next = data[parsePosition + 1];
                if (next == 0x00) {
                    parsePosition += 2; // Byte-stuffed 0xFF value.
                    continue;
                }
                if (next >= MARKER_RST0 && next <= MARKER_RST0 + 7) {
                    parsePosition += 2; // Restart marker — skip.
                    continue;
                }
                // Found a real marker; leave parsePosition pointing at the 0xFF.
                return;
            }
            ++parsePosition;
        }
    };

    // Main marker parsing + scan decoding loop.
    // For baseline: parses markers up to the single SOS, decodes, and exits.
    // For progressive: loops over multiple SOS segments, decoding each scan.
    bool done = false;
    bool parallelBaselineDone = false;
    bool seenFirstSos = false;
    bool dhtAfterSos = false;

    // Progressive scan collection for parallel AC decode.
    struct ProgressiveScanInfo {
        std::vector<u32> componentIndices;
        u8 ss = 0;
        u8 se = 0;
        u8 ah = 0;
        u8 al = 0;
        size_t entropyStart = 0;
        // Huffman table indices for the scan's components.
        std::array<u8, MAX_COMPONENTS> acHuffIdx{};
    };
    std::vector<ProgressiveScanInfo> collectedScans;
    bool collectProgressiveScans = false;
    while (parsePosition + 1 < size && !done) {
        if (data[parsePosition] != 0xFF) {
            parsePosition++; // Skip padding bytes between markers.
            continue;
        }
        // Skip fill bytes (consecutive 0xFF).
        while (parsePosition + 1 < size && data[parsePosition + 1] == 0xFF) {
            parsePosition++;
        }
        if (parsePosition + 1 >= size) {
            break;
        }
        u8 markerCode = data[parsePosition + 1];
        parsePosition += 2;

        // Markers with no payload: stuffed byte, TEM, RST0-RST7.
        if (markerCode == 0x00 || markerCode == 0x01 ||
            (markerCode >= MARKER_RST0 && markerCode <= MARKER_RST0 + 7)) {
            continue;
        }
        if (markerCode == MARKER_EOI) {
            break;
        }

        // All other markers have a 2-byte big-endian length field.
        if (parsePosition + 2 > size) {
            return reportError("Truncated marker segment");
        }
        u16 segmentLength = readBigEndianU16(parsePosition);
        if (segmentLength < 2 || parsePosition + segmentLength > size) {
            return reportError("Invalid marker segment length");
        }
        size_t segmentDataOffset = parsePosition + 2;
        size_t segmentDataLength = segmentLength - 2;

        switch (markerCode) {
        case MARKER_DQT:
            if (!parseQuantizationTable(segmentDataOffset, segmentDataLength)) {
                return false;
            }
            break;
        case MARKER_SOF0:
            if (!parseFrameHeader(segmentDataOffset, segmentDataLength)) {
                return false;
            }
            foundFrameHeader = true;
            break;
        case MARKER_SOF2:
            isProgressive = true;
            if (!parseFrameHeader(segmentDataOffset, segmentDataLength)) {
                return false;
            }
            foundFrameHeader = true;
            break;
        case MARKER_DHT:
            // If DHT appears between SOS markers and we've collected AC scans
            // for parallel dispatch, flush them now (before overwriting tables).
            if (seenFirstSos && !collectedScans.empty()) {
                for (auto& scan : collectedScans) {
                    scanComponentIndices = scan.componentIndices;
                    scanSpectralStart = scan.ss;
                    scanSpectralEnd = scan.se;
                    scanApproxHigh = scan.ah;
                    scanApproxLow = scan.al;
                    for (u32 ci : scanComponentIndices) {
                        components[ci].acHuffmanIndex = scan.acHuffIdx[ci];
                    }
                    bitstream.init(data, size, scan.entropyStart);
                    eobRun = 0;
                    if (!decodeProgressiveScan()) {
                        return false;
                    }
                }
                collectedScans.clear();
                collectProgressiveScans = false;
            }
            if (!parseHuffmanTable(segmentDataOffset, segmentDataLength)) {
                return false;
            }
            if (seenFirstSos)
                dhtAfterSos = true;
            break;
        case MARKER_DRI:
            if (!parseRestartInterval(segmentDataOffset, segmentDataLength)) {
                return false;
            }
            break;
        case MARKER_SOS: {
            if (!foundFrameHeader) {
                return reportError("SOS marker encountered before frame header");
            }
            size_t scanDataPosition = 0;
            if (!parseScanHeader(segmentDataOffset, segmentDataLength, scanDataPosition)) {
                return false;
            }

            if (!seenFirstSos) {
                seenFirstSos = true;
                // Decide whether to collect progressive scans for parallel decode.
                collectProgressiveScans =
                    isProgressive && ctx && ctx->pool && ctx->pool->threadCount() > 1;
            }

            if (isProgressive) {
                bool isDcScan = (scanSpectralStart == 0);

                if (isDcScan) {
                    // DC scans: always serial (inter-block prediction dependency).
                    bitstream.init(data, size, scanDataPosition);
                    for (u32 ci : scanComponentIndices) {
                        components[ci].dcPrediction = 0;
                    }
                    if (!decodeProgressiveScan()) {
                        return false;
                    }
                } else if (collectProgressiveScans && !dhtAfterSos) {
                    // AC scan: collect for later parallel dispatch.
                    ProgressiveScanInfo info;
                    info.componentIndices = scanComponentIndices;
                    info.ss = scanSpectralStart;
                    info.se = scanSpectralEnd;
                    info.ah = scanApproxHigh;
                    info.al = scanApproxLow;
                    info.entropyStart = scanDataPosition;
                    for (u32 ci : scanComponentIndices) {
                        info.acHuffIdx[ci] = components[ci].acHuffmanIndex;
                    }
                    collectedScans.push_back(std::move(info));
                } else {
                    // AC scan: serial fallback.
                    bitstream.init(data, size, scanDataPosition);
                    eobRun = 0;
                    if (!decodeProgressiveScan()) {
                        return false;
                    }
                }
            } else {
                // Baseline: choose parallel or serial path.
                bool canParallelDecode =
                    ctx && ctx->pool && ctx->pool->threadCount() > 1 && restartInterval > 0;
                if (canParallelDecode) {
                    bitstream.init(data, size, scanDataPosition);
                    // Check no-subsampling for fused direct output.
                    bool noSub = true;
                    for (u32 ci = 0; ci < componentCount; ++ci) {
                        if (components[ci].horizontalSampling != 1 ||
                            components[ci].verticalSampling != 1) {
                            noSub = false;
                            break;
                        }
                    }
                    if (!decodeScanDataParallel(noSub ? &outputImage : nullptr)) {
                        return false;
                    }
                    parallelBaselineDone = true;
                } else {
                    bitstream.init(data, size, scanDataPosition);
                    bool useParallelIdct = ctx && ctx->pool && ctx->pool->threadCount() > 1;
                    if (useParallelIdct) {
                        if (!decodeScanDataToCoefficients()) {
                            return false;
                        }
                    } else {
                        if (!decodeScanData()) {
                            return false;
                        }
                    }
                }
                done = true; // Baseline: single scan only.
            }

            // Skip past the entropy-coded segment to find the next marker.
            // The bitstream reader has consumed some bytes, but the marker
            // scanner works on the raw byte stream, so we need to fast-forward.
            parsePosition = scanDataPosition;
            skipEntropyData();
            continue; // Don't add segmentLength — we've already repositioned.
        }
        default:
            // Reject unsupported SOF variants (SOF1, SOF3-SOF15 except DHT/SOF2).
            if (markerCode >= 0xC1 && markerCode <= 0xCF && markerCode != MARKER_DHT &&
                markerCode != MARKER_SOF2) {
                return reportError("Unsupported JPEG frame type 0x" + std::to_string(markerCode));
            }
            // APPn, COM, etc. — skip silently.
            break;
        }

        parsePosition += segmentLength;
    }

    if (!foundFrameHeader) {
        return reportError("No frame header found in JPEG data");
    }

    // Detect no-subsampling: all components have 1×1 sampling factors.
    bool noSubsampling = true;
    for (u32 ci = 0; ci < componentCount; ++ci) {
        if (components[ci].horizontalSampling != 1 || components[ci].verticalSampling != 1) {
            noSubsampling = false;
            break;
        }
    }

    if (parallelBaselineDone && noSubsampling) {
        // Fused baseline parallel: decodeScanDataParallel wrote directly to
        // outputImage.  Nothing left to do.
        return true;
    } else if (parallelBaselineDone) {
        // Parallel baseline with subsampling: IDCT done, need assembly.
        return assembleInterleavedImage(outputImage);
    } else if (!collectedScans.empty()) {
        // Parallel progressive: dispatch collected AC scans in waves, then finalize.
        //
        // Group scans into waves where scans within a wave target different
        // (component, spectral_range) pairs — safe to decode in parallel.
        // When a scan conflicts with one already in the current wave,
        // flush the wave and start a new one.
        struct ScanKey {
            u32 ci;
            u8 ss;
            u8 se;
        };

        std::vector<std::vector<size_t>> waves;
        std::vector<ScanKey> currentWaveKeys;
        std::vector<size_t> currentWave;

        auto hasConflict = [&](u32 ci, u8 ss, u8 se) {
            for (auto& k : currentWaveKeys) {
                if (k.ci == ci && k.ss == ss && k.se == se)
                    return true;
            }
            return false;
        };

        for (size_t i = 0; i < collectedScans.size(); ++i) {
            auto& scan = collectedScans[i];
            u32 ci = scan.componentIndices[0]; // AC scans are single-component.
            if (hasConflict(ci, scan.ss, scan.se)) {
                waves.push_back(std::move(currentWave));
                currentWave.clear();
                currentWaveKeys.clear();
            }
            currentWave.push_back(i);
            currentWaveKeys.push_back({ci, scan.ss, scan.se});
        }
        if (!currentWave.empty()) {
            waves.push_back(std::move(currentWave));
        }

        // Process each wave — parallel when multiple scans, serial otherwise.
        for (auto& wave : waves) {
            if (wave.size() == 1 || !ctx || !ctx->pool) {
                for (size_t idx : wave) {
                    auto& scan = collectedScans[idx];
                    scanComponentIndices = scan.componentIndices;
                    scanSpectralStart = scan.ss;
                    scanSpectralEnd = scan.se;
                    scanApproxHigh = scan.ah;
                    scanApproxLow = scan.al;
                    for (u32 ci : scanComponentIndices) {
                        components[ci].acHuffmanIndex = scan.acHuffIdx[ci];
                    }
                    bitstream.init(data, size, scan.entropyStart);
                    eobRun = 0;
                    if (!decodeProgressiveScan()) {
                        return false;
                    }
                }
            } else {
                // Parallel: each scan in its own task.
                // Use a blocking context (no timeline) so that decode() doesn't
                // return before the lambda captures of local data are consumed.
                JpegContext blockingCtx;
                blockingCtx.pool = ctx->pool;
                blockingCtx.sem = nullptr;
                blockingCtx.currentValue = 0;

                parallel_for_tasks(static_cast<u32>(wave.size()), &blockingCtx, [&](u32 taskIdx) {
                    auto& scan = collectedScans[wave[taskIdx]];
                    u32 ci = scan.componentIndices[0];
                    auto& comp = components[ci];
                    const auto& acTable = acHuffmanTables[scan.acHuffIdx[ci]];

                    BitstreamReader localBs;
                    localBs.init(data, size, scan.entropyStart);
                    u32 localEobRun = 0;

                    u32 totalBlocksX = comp.blocksPerRow;
                    u32 totalBlocksY = comp.blocksPerCol;
                    u32 mcuSeqIdx = 0;

                    for (u32 by = 0; by < totalBlocksY; ++by) {
                        for (u32 bx = 0; bx < totalBlocksX; ++bx) {
                            if (restartInterval > 0 && mcuSeqIdx > 0 &&
                                (mcuSeqIdx % restartInterval) == 0) {
                                localBs.handleRestartMarker();
                                localEobRun = 0;
                            }
                            auto& coeffs = comp.coefficientBlocks[by * totalBlocksX + bx];
                            if (scan.ah == 0) {
                                decodeProgressiveAcFirst(localBs, coeffs, acTable, localEobRun,
                                                         scan.ss, scan.se, scan.al);
                            } else {
                                decodeProgressiveAcRefine(localBs, coeffs, acTable, localEobRun,
                                                          scan.ss, scan.se, scan.al);
                            }
                            ++mcuSeqIdx;
                        }
                    }
                });
            }
        }

        // Dequantize + IDCT + assemble (fused for no-subsampling).
        if (noSubsampling) {
            return finalizeAndAssembleImage(outputImage);
        }
        if (!finalizeProgressiveImage()) {
            return false;
        }
    } else if (isProgressive || (ctx && ctx->pool && ctx->pool->threadCount() > 1)) {
        // Serial progressive (DHT-between-SOS fallback) or
        // baseline with pool (serial entropy → parallel IDCT).
        if (noSubsampling) {
            return finalizeAndAssembleImage(outputImage);
        }
        if (!finalizeProgressiveImage()) {
            return false;
        }
    }

    return assembleInterleavedImage(outputImage);
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

std::optional<Image> decode_raw(std::span<const u8> data, std::string* out_error, JpegContext* ctx,
                                Image* asyncOutput) {
    auto decoder = std::make_shared<JpegDecoder>();
    decoder->errorOutput = out_error;
    decoder->ctx = ctx;

    // Parsing + entropy decode must run synchronously: we need the decoded
    // coefficients / sample buffers and image dimensions before we can build
    // the parallel DAG for IDCT and pixel assembly.
    //
    // When ctx is non-null the caller's timeline is paused during this call;
    // only the IDCT and assembly phases are parallelised.
    auto outputImage = std::make_shared<Image>();
    if (!decoder->decode(data.data(), data.size(), *outputImage)) {
        return std::nullopt;
    }

    if (!ctx || !ctx->sem) {
        // Serial path — everything already completed in decode().
        return std::move(*outputImage);
    }

    // Async path — output will be delivered via asyncOutput after DAG completes.
    if (asyncOutput) {
        // Expose dimensions synchronously so callers can validate/allocate
        // before the pixel data arrives via the DAG.
        asyncOutput->width = outputImage->width;
        asyncOutput->height = outputImage->height;
        asyncOutput->components = outputImage->components;

        submitSingleTask(
            ctx, [decoder, outputImage, asyncOutput]() { *asyncOutput = std::move(*outputImage); });
    }
    return std::nullopt;
}

} // namespace whiteout::textures::jpeg
