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

/// Extend a magnitude-coded value to its signed representation.
/// JPEG DC/AC coefficients use a magnitude + sign bit encoding where values
/// below 2^(category-1) are negative.
i32 extend_magnitude_to_signed(u32 magnitudeBits, i32 category) {
    i32 threshold = 1 << (category - 1);
    if (static_cast<i32>(magnitudeBits) < threshold) {
        return static_cast<i32>(magnitudeBits) - (2 * threshold - 1);
    }
    return static_cast<i32>(magnitudeBits);
}

// ============================================================================
// Inverse DCT — Loeffler-Ligtenberg-Moschytz (LLM) Butterfly
// ============================================================================

/// 1-D IDCT butterfly on 8 values (LLM algorithm, IEEE 1992).
///
/// The LLM factorisation computes:
///   X[k] = sum_{n=0..7} x[n] * cos(pi*(2k+1)*n / 16)
/// using 11 multiplies and 29 adds (instead of the naive 64 multiplies).
///
/// Constants are defined in jpeg_common.h, derived from cos(k*pi/16) factors.
void idct_1d_butterfly(const std::array<f32, BLOCK_SIZE>& input,
                       std::array<f32, BLOCK_SIZE>& output) {
    // Even-indexed butterflies (inputs: x0, x2, x4, x6)
    f32 rotation = (input[2] + input[6]) * EVEN_ROTATION_K;
    f32 even2 = rotation - input[6] * EVEN_ROTATION_A;
    f32 even3 = rotation + input[2] * EVEN_ROTATION_B;
    f32 even0 = input[0] + input[4];
    f32 even1 = input[0] - input[4];
    f32 stage_e0 = even0 + even3;
    f32 stage_e3 = even0 - even3;
    f32 stage_e1 = even1 + even2;
    f32 stage_e2 = even1 - even2;

    // Odd-indexed butterflies (inputs: x1, x3, x5, x7)
    f32 sum_73 = input[7] + input[3];
    f32 sum_51 = input[5] + input[1];
    f32 sum_71 = input[7] + input[1];
    f32 sum_53 = input[5] + input[3];
    f32 scaleFactor = (sum_73 + sum_51) * ODD_SCALE;

    f32 odd0 = input[7] * ODD_COEFF_X7;
    f32 odd1 = input[5] * ODD_COEFF_X5;
    f32 odd2 = input[3] * ODD_COEFF_X3;
    f32 odd3 = input[1] * ODD_COEFF_X1;
    sum_71 = scaleFactor + sum_71 * ODD_PAIR_71;
    sum_53 = scaleFactor + sum_53 * ODD_PAIR_53;
    sum_73 = sum_73 * ODD_PAIR_73;
    sum_51 = sum_51 * ODD_PAIR_51;
    odd3 += sum_71 + sum_51;
    odd2 += sum_53 + sum_73;
    odd1 += sum_53 + sum_51;
    odd0 += sum_71 + sum_73;

    // Final butterfly: combine even and odd parts.
    output[0] = stage_e0 + odd3;
    output[1] = stage_e1 + odd2;
    output[2] = stage_e2 + odd1;
    output[3] = stage_e3 + odd0;
    output[4] = stage_e3 - odd0;
    output[5] = stage_e2 - odd1;
    output[6] = stage_e1 - odd2;
    output[7] = stage_e0 - odd3;
}

/// Apply the 2-D IDCT to an 8x8 block of dequantised coefficients.
///
/// Uses a separable approach: 1-D IDCT along rows, then 1-D IDCT along columns.
/// The final output includes the standard +128 DC level-shift and is clamped to
/// [0, 255].  The 1/8 normalisation factor for the 2-D transform is applied in
/// the column pass (0.125 = 1/8).
void inverse_dct_block(const std::array<i32, BLOCK_PIXELS>& dequantisedCoefficients,
                       u8* outputPixels, u32 outputRowStride) {
    std::array<f32, BLOCK_PIXELS> intermediateBuffer{};

    // Row pass (horizontal 1-D IDCT).
    for (i32 rowIndex = 0; rowIndex < BLOCK_SIZE; rowIndex++) {
        std::array<f32, BLOCK_SIZE> rowInput{};
        for (i32 columnIndex = 0; columnIndex < BLOCK_SIZE; columnIndex++) {
            rowInput[columnIndex] =
                static_cast<f32>(dequantisedCoefficients[rowIndex * BLOCK_SIZE + columnIndex]);
        }

        // Optimisation: if all AC coefficients in the row are zero, broadcast DC.
        bool allAcCoefficientsZero =
            std::all_of(rowInput.begin() + 1, rowInput.end(), [](f32 val) { return val == 0.0f; });

        if (allAcCoefficientsZero) {
            std::fill_n(&intermediateBuffer[rowIndex * BLOCK_SIZE], BLOCK_SIZE, rowInput[0]);
        } else {
            std::array<f32, BLOCK_SIZE> rowOutput{};
            idct_1d_butterfly(rowInput, rowOutput);
            std::copy(rowOutput.begin(), rowOutput.end(),
                      &intermediateBuffer[rowIndex * BLOCK_SIZE]);
        }
    }

    // Column pass (vertical 1-D IDCT + normalisation + level-shift).
    for (i32 columnIndex = 0; columnIndex < BLOCK_SIZE; columnIndex++) {
        std::array<f32, BLOCK_SIZE> columnInput{};
        for (i32 rowIndex = 0; rowIndex < BLOCK_SIZE; rowIndex++) {
            columnInput[rowIndex] = intermediateBuffer[rowIndex * BLOCK_SIZE + columnIndex];
        }

        std::array<f32, BLOCK_SIZE> columnOutput{};
        idct_1d_butterfly(columnInput, columnOutput);

        for (i32 rowIndex = 0; rowIndex < BLOCK_SIZE; rowIndex++) {
            i32 pixelValue = static_cast<i32>(columnOutput[rowIndex] * DCT_2D_NORMALISATION +
                                              DC_LEVEL_SHIFT_AND_ROUND);
            outputPixels[rowIndex * outputRowStride + columnIndex] =
                static_cast<u8>(std::clamp(pixelValue, 0, 255));
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
    u8 scanSpectralStart = 0;  ///< Ss from current scan's SOS.
    u8 scanSpectralEnd = 63;   ///< Se from current scan's SOS.
    u8 scanApproxHigh = 0;     ///< Ah (successive approximation high bit).
    u8 scanApproxLow = 0;      ///< Al (successive approximation low bit).
    u32 eobRun = 0;            ///< EOBRUN counter for progressive AC scans.

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
        return static_cast<u16>((bitstream.data[offset] << 8) |
                                bitstream.data[offset + 1]);
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

    // -- Progressive Entropy Decoding --

    bool decodeProgressiveScan();
    bool decodeProgressiveDcFirst(u32 compIdx, std::array<i32, BLOCK_PIXELS>& coeffs,
                                  i32& dcPrediction);
    bool decodeProgressiveDcRefine(std::array<i32, BLOCK_PIXELS>& coeffs);
    bool decodeProgressiveAcFirst(std::array<i32, BLOCK_PIXELS>& coeffs,
                                  const HuffmanTable& acTable);
    bool decodeProgressiveAcRefine(std::array<i32, BLOCK_PIXELS>& coeffs,
                                   const HuffmanTable& acTable);
    bool finalizeProgressiveImage();

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
            components[componentIndex].coefficientBlocks.assign(
                static_cast<size_t>(bpr) * bpc, zeroBlock);
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
        if (scanSpectralStart > 63 || scanSpectralEnd > 63 ||
            scanSpectralStart > scanSpectralEnd) {
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
                        std::array<i32, BLOCK_PIXELS> dctCoefficients{};
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
bool JpegDecoder::decodeProgressiveAcFirst(std::array<i32, BLOCK_PIXELS>& coeffs,
                                               const HuffmanTable& acTable) {
    if (eobRun > 0) {
        --eobRun;
        return true;
    }

    for (i32 k = scanSpectralStart; k <= scanSpectralEnd; ++k) {
        i32 symbol = acTable.decodeSymbol(bitstream);
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
            eobRun = (1u << runLength);
            if (runLength > 0) {
                eobRun += bitstream.readBits(runLength);
            }
            --eobRun; // Current block counts as one.
            return true;
        }

        k += runLength;
        if (k > scanSpectralEnd) {
            return reportError("Progressive AC first: coefficient index out of range");
        }
        u32 magnitudeBits = bitstream.readBits(category);
        i32 value = extend_magnitude_to_signed(magnitudeBits, category);
        coeffs[k] = value << scanApproxLow;
    }
    return true;
}

/// Apply one correction bit to a previously-nonzero coefficient.
/// If the read bit is 1, the coefficient magnitude is increased by correctionBit
/// (added for positive values, subtracted for negative).
void applyRefinementBit(BitstreamReader& bitstream, i32& coeff, i32 correctionBit) {
    u32 bit = bitstream.readBits(1);
    if (bit) {
        if (coeff > 0)
            coeff += correctionBit;
        else
            coeff -= correctionBit;
    }
}

/// Decode AC refinement for one block in a progressive scan.
/// Ss>0, Ah>0 — refine previously-coded AC coefficients in range [Ss, Se].
///
/// This is the most complex progressive pass. For each block:
///  - Previously-zero coefficients may become nonzero (category=1) or stay zero (run skip).
///  - Previously-nonzero coefficients receive one correction bit per pass.
///  - EOBRUN mechanism skips remaining zero positions.
bool JpegDecoder::decodeProgressiveAcRefine(std::array<i32, BLOCK_PIXELS>& coeffs,
                                                const HuffmanTable& acTable) {
    i32 k = scanSpectralStart;
    i32 correctionBit = 1 << scanApproxLow;

    if (eobRun > 0) {
        // In an EOB run: just refine existing nonzero coefficients.
        for (; k <= scanSpectralEnd; ++k) {
            if (coeffs[k] != 0) {
                applyRefinementBit(bitstream, coeffs[k], correctionBit);
            }
        }
        --eobRun;
        return true;
    }

    for (; k <= scanSpectralEnd; ++k) {
        i32 symbol = acTable.decodeSymbol(bitstream);
        if (symbol < 0) {
            return reportError("Progressive AC refine: Huffman decode error");
        }
        i32 runLength = (symbol >> 4) & 0x0F;
        i32 category = symbol & 0x0F;

        i32 newValue = 0; // Will be set if a new nonzero coefficient is created.
        if (category == 0) {
            if (runLength < 15) {
                // EOBn.
                eobRun = (1u << runLength);
                if (runLength > 0) {
                    eobRun += bitstream.readBits(runLength);
                }
                // Refine remaining nonzero coefficients in this block, then done.
                for (; k <= scanSpectralEnd; ++k) {
                    if (coeffs[k] != 0) {
                        applyRefinementBit(bitstream, coeffs[k], correctionBit);
                    }
                }
                --eobRun;
                return true;
            }
            // runLength == 15: ZRL with no new nonzero — skip 16 zero positions.
        } else if (category == 1) {
            // New nonzero coefficient: read sign bit.
            u32 signBit = bitstream.readBits(1);
            newValue = signBit ? correctionBit : -correctionBit;
        } else {
            return reportError("Progressive AC refine: unexpected category " +
                               std::to_string(category));
        }

        // Skip `runLength` zero-valued positions, refining nonzero ones along the way.
        i32 zerosToSkip = runLength;
        for (; k <= scanSpectralEnd; ++k) {
            if (coeffs[k] != 0) {
                // Refine existing nonzero coefficient.
                applyRefinementBit(bitstream, coeffs[k], correctionBit);
            } else {
                if (zerosToSkip == 0) {
                    break; // Found the target zero position.
                }
                --zerosToSkip;
            }
        }

        // Place the new nonzero coefficient (if any) at position k.
        if (newValue != 0 && k <= scanSpectralEnd) {
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
                    if (!decodeProgressiveAcFirst(coeffs, acTable))
                        return false;
                } else {
                    if (!decodeProgressiveAcRefine(coeffs, acTable))
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

    const u32 compCnt = componentCount;

    parallel_for_blocks(totalBlocks, ctx,
        [this, compCnt, info](u32 flatBegin, u32 flatEnd) {
            for (u32 flat = flatBegin; flat < flatEnd; ++flat) {
                // Find which component and which block within it.
                u32 ci = 0;
                while (ci + 1 < compCnt &&
                       flat >= info[ci + 1].blockOffset) {
                    ++ci;
                }
                u32 localIdx = flat - info[ci].blockOffset;
                u32 bx = localIdx % info[ci].blocksPerRow;
                u32 by = localIdx / info[ci].blocksPerRow;

                auto& comp = components[ci];
                auto& coeffs = comp.coefficientBlocks[by * info[ci].blocksPerRow + bx];
                const auto& quantTable = quantTables[info[ci].quantTableIndex];

                // Dequantize: multiply each zig-zag-ordered coefficient by the
                // quant value and place in natural (row-major) order for the IDCT.
                std::array<i32, BLOCK_PIXELS> dequantised{};
                for (i32 zigzag = 0; zigzag < BLOCK_PIXELS; ++zigzag) {
                    dequantised[ZIGZAG_ORDER[zigzag]] = coeffs[zigzag] * quantTable[zigzag];
                }

                u32 pixelX = bx * BLOCK_SIZE;
                u32 pixelY = by * BLOCK_SIZE;
                inverse_dct_block(dequantised,
                                  comp.sampleBuffer.data() +
                                      pixelY * comp.sampleBufferStride + pixelX,
                                  comp.sampleBufferStride);
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
        layouts[ci] = {components[ci].horizontalSampling,
                       components[ci].verticalSampling,
                       components[ci].sampleBufferStride,
                       components[ci].sampleBuffer.data()};
    }

    parallel_for_blocks(imageHeight, ctx,
        [&outputImage, imgW, compCnt, maxHS, maxVS, layouts](u32 rowBegin, u32 rowEnd) {
            for (u32 pixelY = rowBegin; pixelY < rowEnd; ++pixelY) {
                for (u32 pixelX = 0; pixelX < imgW; ++pixelX) {
                    size_t destOff =
                        (static_cast<size_t>(pixelY) * imgW + pixelX) * compCnt;
                    for (u32 ci = 0; ci < compCnt; ++ci) {
                        u32 sampleX = pixelX * layouts[ci].horizontalSampling / maxHS;
                        u32 sampleY = pixelY * layouts[ci].verticalSampling / maxVS;
                        u32 srcOff = sampleY * layouts[ci].sampleBufferStride + sampleX;
                        outputImage.pixels[destOff + ci] = layouts[ci].sampleData[srcOff];
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
            if (!parseHuffmanTable(segmentDataOffset, segmentDataLength)) {
                return false;
            }
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

            bitstream.init(data, size, scanDataPosition);

            if (isProgressive) {
                // Reset DC predictions at the start of each progressive scan.
                for (u32 ci : scanComponentIndices) {
                    components[ci].dcPrediction = 0;
                }
                if (!decodeProgressiveScan()) {
                    return false;
                }
            } else {
                if (!decodeScanData()) {
                    return false;
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

    // For progressive mode: run IDCT on accumulated coefficients.
    if (isProgressive) {
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

std::optional<Image> decode_raw(std::span<const u8> data, std::string* out_error,
                                JpegContext* ctx, Image* asyncOutput) {
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

        submitSingleTask(ctx, [decoder, outputImage, asyncOutput]() {
            *asyncOutput = std::move(*outputImage);
        });
    }
    return std::nullopt;
}

} // namespace whiteout::textures::jpeg
