// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Baseline (SOF0) JPEG decoder.
///
/// Returns raw component values without Y'CbCr-to-RGB conversion.  BLP files
/// store BGRA colour components directly in the JPEG data stream, so the raw
/// values must be preserved.
///
/// Supported:   Baseline sequential DCT (SOF0), 8-bit precision, 1-4 channels,
///              restart markers (DRI/RST), chroma subsampling.
/// Unsupported: Progressive (SOF2), arithmetic coding, lossless, hierarchical.
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
};

// ============================================================================
// Decoder State Machine
// ============================================================================

struct BaselineDecoder {
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

    std::string* errorOutput = nullptr;

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

    // -- Output Assembly --

    bool assembleInterleavedImage(Image& outputImage);

    // -- Top-Level Entry Point --

    bool decode(const u8* data, size_t size, Image& outputImage);
};

// ============================================================================
// Marker Segment Parsers
// ============================================================================

/// DQT — Define Quantization Table (ITU-T T.81, Section B.2.4.1)
bool BaselineDecoder::parseQuantizationTable(size_t dataOffset, size_t dataLength) {
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
bool BaselineDecoder::parseFrameHeader(size_t dataOffset, size_t dataLength) {
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
    }
    return true;
}

/// DHT — Define Huffman Table (ITU-T T.81, Section B.2.4.2)
bool BaselineDecoder::parseHuffmanTable(size_t dataOffset, size_t dataLength) {
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
bool BaselineDecoder::parseRestartInterval(size_t dataOffset, size_t dataLength) {
    if (dataLength < 2) {
        return reportError("DRI: segment too short");
    }
    restartInterval = readBigEndianU16(dataOffset);
    return true;
}

/// SOS — Start of Scan header (ITU-T T.81, Section B.2.3)
bool BaselineDecoder::parseScanHeader(size_t dataOffset, size_t dataLength, size_t& scanDataStart) {
    if (dataLength < 1) {
        return reportError("SOS: segment too short");
    }
    u8 scanComponentCount = bitstream.data[dataOffset];
    if (scanComponentCount != componentCount) {
        return reportError("SOS: scan component count does not match frame header");
    }
    if (dataLength < 1 + scanComponentCount * 2 + 3) {
        return reportError("SOS: segment too short for component selectors");
    }

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
                foundMatchingComponent = true;
                break;
            }
        }
        if (!foundMatchingComponent) {
            return reportError("SOS: no matching component for selector id " +
                               std::to_string(selectorId));
        }
    }

    // Bytes after component selectors: Ss, Se, Ah|Al (spectral selection /
    // successive approximation).  Baseline always uses 0, 63, 0.
    scanDataStart = dataOffset + 1 + scanComponentCount * 2 + 3;
    return true;
}

// ============================================================================
// Entropy-Coded Block Decoding (ITU-T T.81, Section F.2.2)
// ============================================================================

/// Decode one 8x8 DCT block from the Huffman-coded bitstream, dequantise the
/// coefficients using the quantisation table, and store them in natural
/// (row-major) order via the zig-zag index table.
bool BaselineDecoder::decodeDctBlock(std::array<i32, BLOCK_PIXELS>& coefficients,
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
bool BaselineDecoder::decodeScanData() {
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
// Output Assembly (Interleave + Nearest-Neighbour Upsample)
// ============================================================================

/// Combine the per-component sample buffers into a single interleaved image.
/// Components with smaller sampling factors than the maximum are upsampled
/// using nearest-neighbour replication.
bool BaselineDecoder::assembleInterleavedImage(Image& outputImage) {
    outputImage.width = imageWidth;
    outputImage.height = imageHeight;
    outputImage.components = componentCount;
    outputImage.pixels.resize(static_cast<size_t>(imageWidth) * imageHeight * componentCount);

    for (u32 pixelY = 0; pixelY < imageHeight; pixelY++) {
        for (u32 pixelX = 0; pixelX < imageWidth; pixelX++) {
            size_t destinationOffset =
                (static_cast<size_t>(pixelY) * imageWidth + pixelX) * componentCount;
            for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
                // Map output pixel coordinates to component sample coordinates
                // (nearest-neighbour upsampling for subsampled components).
                u32 sampleX =
                    pixelX * components[componentIndex].horizontalSampling / maxHorizontalSampling;
                u32 sampleY =
                    pixelY * components[componentIndex].verticalSampling / maxVerticalSampling;
                u32 sampleBufferOffset =
                    sampleY * components[componentIndex].sampleBufferStride + sampleX;
                outputImage.pixels[destinationOffset + componentIndex] =
                    components[componentIndex].sampleBuffer[sampleBufferOffset];
            }
        }
    }
    return true;
}

// ============================================================================
// Top-Level: Parse Markers + Decode (ITU-T T.81 Figure E.6)
// ============================================================================

bool BaselineDecoder::decode(const u8* data, size_t size, Image& outputImage) {
    bitstream.init(data, size, 0);

    // Verify SOI (Start of Image) marker.
    if (size < 2 || data[0] != 0xFF || data[1] != 0xD8) {
        return reportError("Not a JPEG file (no SOI marker)");
    }
    size_t parsePosition = 2;

    bool foundFrameHeader = false;
    bool foundScanHeader = false;
    size_t scanDataPosition = 0;

    // Walk through marker segments until we hit SOS (start of entropy data).
    while (parsePosition + 1 < size && !foundScanHeader) {
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
            return reportError("Progressive JPEG (SOF2) is not supported");
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
                return reportError("SOS marker encountered before SOF0 frame header");
            }
            if (!parseScanHeader(segmentDataOffset, segmentDataLength, scanDataPosition)) {
                return false;
            }
            // scanDataPosition is already an absolute offset (parseScanHeader
            // receives an absolute dataOffset and adds the header size to it).
            foundScanHeader = true;
            break;
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
        return reportError("No SOF0 frame header found in JPEG data");
    }
    if (!foundScanHeader) {
        return reportError("No SOS scan header found in JPEG data");
    }

    // Decode the entropy-coded scan data.
    bitstream.init(data, size, scanDataPosition);
    if (!decodeScanData()) {
        return false;
    }

    return assembleInterleavedImage(outputImage);
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

std::optional<Image> decode_raw(std::span<const u8> data, std::string* out_error) {
    BaselineDecoder decoder;
    decoder.errorOutput = out_error;
    Image image;
    if (!decoder.decode(data.data(), data.size(), image)) {
        return std::nullopt;
    }
    return image;
}

} // namespace whiteout::textures::jpeg
