// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Baseline (SOF0) JPEG encoder.
///
/// Encodes raw component values without RGB-to-Y'CbCr conversion.  BLP files
/// store BGRA colour components directly in the JPEG data stream, so the raw
/// values must be preserved.
///
/// Supported:   Baseline sequential DCT (SOF0), 8-bit precision, 1-4 channels,
///              configurable quality (1-100), no chroma subsampling.
///
/// Algorithms used:
///   - Forward DCT: Arai-Agui-Nakajima (AAN) separable 1-D butterfly,
///     applied row-then-column (Transactions on IEICE 1988).
///   - Huffman encoding: standard tables from ITU-T T.81 Annex K.
///   - Quantisation: scaled standard luminance table for all components.

#include "jpeg_encode.h"

#include "huffman.h"
#include "jpeg_common.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace whiteout::textures::jpeg {

namespace {

// ============================================================================
// JPEG Quality Scaling Constants (libjpeg convention)
// ============================================================================

/// For quality < 50: scale = QUALITY_LOW_NUMERATOR / quality.
static constexpr i32 QUALITY_LOW_NUMERATOR = 5000;

/// For quality >= 50: scale = QUALITY_HIGH_BASE - 2 * quality.
static constexpr i32 QUALITY_HIGH_BASE = 200;

/// DQT segment length for 8-bit precision: 2 (length) + 1 (table info) + 64 (values).
static constexpr u16 DQT_8BIT_SEGMENT_LENGTH = 2 + 1 + BLOCK_PIXELS;

/// JPEG sampling factor byte for 1x1 (no subsampling): (1 << 4) | 1.
static constexpr u8 SAMPLING_FACTOR_1x1 = 0x11;

// ============================================================================
// Standard Quantisation Table (ITU-T T.81, Annex K, Table K.1)
// ============================================================================

/// Standard JPEG luminance quantisation matrix in natural (row-major) order.
/// Used for all components in raw mode since there is no colourspace distinction.
static constexpr std::array<u8, BLOCK_PIXELS> STD_LUMINANCE_QUANT_NATURAL = {{
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
}};

// ============================================================================
// Forward DCT — Arai-Agui-Nakajima (AAN) Butterfly
// ============================================================================

/// AAN scale factors per 1-D frequency index.
///
/// The AAN butterfly produces outputs that are scaled non-uniformly relative
/// to the true DCT: output[k] = true_DCT[k] * aanscale[k] * sqrt(N).
/// Quantisation must compensate by dividing by (aanscale[row] * aanscale[col])
/// so that the values stored in the JPEG file match the standard definition.
static constexpr std::array<f32, BLOCK_SIZE> AAN_SCALE_FACTORS = {{
    1.0f,
    SQRT2_F * dct_cos(1),
    SQRT2_F * dct_cos(2),
    SQRT2_F * dct_cos(3),
    SQRT2_F * dct_cos(4),
    SQRT2_F * dct_cos(5),
    SQRT2_F * dct_cos(6),
    SQRT2_F * dct_cos(7),
}};

/// 1-D forward DCT butterfly on 8 values (AAN algorithm, IEICE 1988).
///
/// This is the forward (analysis) counterpart of the inverse DCT butterfly
/// in jpeg_decode.cpp.  Produces scaled output: each frequency bin k is
/// multiplied by AAN_SCALE_FACTORS[k] relative to the true DCT.
///
/// Constants are defined in jpeg_common.h, derived from cos(k*pi/16) factors.
void fdct_1d_butterfly(const std::array<f32, BLOCK_SIZE>& input,
                       std::array<f32, BLOCK_SIZE>& output) {
    // Stage 1: Spatial butterfly (sum/difference pairs).
    f32 tmp0 = input[0] + input[7];
    f32 tmp7 = input[0] - input[7];
    f32 tmp1 = input[1] + input[6];
    f32 tmp6 = input[1] - input[6];
    f32 tmp2 = input[2] + input[5];
    f32 tmp5 = input[2] - input[5];
    f32 tmp3 = input[3] + input[4];
    f32 tmp4 = input[3] - input[4];

    // Even part: produces coefficients at DCT positions 0, 2, 4, 6.
    f32 even_sum_03 = tmp0 + tmp3;
    f32 even_diff_03 = tmp0 - tmp3;
    f32 even_sum_12 = tmp1 + tmp2;
    f32 even_diff_12 = tmp1 - tmp2;

    output[0] = even_sum_03 + even_sum_12;
    output[4] = even_sum_03 - even_sum_12;

    f32 z1 = (even_diff_12 + even_diff_03) * COS_PI_OVER_4;
    output[2] = even_diff_03 + z1;
    output[6] = even_diff_03 - z1;

    // Odd part: produces coefficients at DCT positions 1, 3, 5, 7.
    f32 odd_sum_45 = tmp4 + tmp5;
    f32 odd_sum_56 = tmp5 + tmp6;
    f32 odd_sum_67 = tmp6 + tmp7;

    f32 z5 = (odd_sum_45 - odd_sum_67) * SIN_PI_OVER_8;
    f32 z2 = EVEN_ROTATION_K * odd_sum_45 + z5;
    f32 z4 = SQRT2_COS_PI_OVER_8 * odd_sum_67 + z5;
    f32 z3 = odd_sum_56 * COS_PI_OVER_4;

    f32 z11 = tmp7 + z3;
    f32 z13 = tmp7 - z3;

    output[5] = z13 + z2;
    output[3] = z13 - z2;
    output[1] = z11 + z4;
    output[7] = z11 - z4;
}

/// Apply the 2-D forward DCT to an 8x8 pixel block, then quantise.
///
/// Uses a separable approach: 1-D forward DCT along rows, then 1-D forward DCT
/// along columns.  The column pass includes a 0.125 (1/8) normalisation factor
/// that matches the decoder's IDCT scaling, ensuring a correct encode/decode
/// round-trip.
///
/// @param inputPixels        Pointer to the top-left pixel of the 8x8 block.
/// @param inputRowStride     Byte stride between rows of the input buffer.
/// @param quantisedCoeffs    Output: quantised coefficients in zig-zag order.
/// @param quantTable         Quantisation table values in zig-zag order.
void forward_dct_and_quantise(const u8* inputPixels, u32 inputRowStride,
                              std::array<i32, BLOCK_PIXELS>& quantisedCoeffs,
                              const std::array<u16, BLOCK_PIXELS>& quantTable) {
    std::array<f32, BLOCK_PIXELS> intermediateBuffer{};

    // Row pass (horizontal 1-D forward DCT with -128 level shift).
    for (i32 rowIndex = 0; rowIndex < BLOCK_SIZE; rowIndex++) {
        std::array<f32, BLOCK_SIZE> rowInput{};
        for (i32 columnIndex = 0; columnIndex < BLOCK_SIZE; columnIndex++) {
            rowInput[columnIndex] =
                static_cast<f32>(inputPixels[rowIndex * inputRowStride + columnIndex]) -
                DC_LEVEL_SHIFT;
        }

        // Optimisation: if all input values are equal, only DC is nonzero.
        bool allPixelsEqual = std::all_of(rowInput.begin() + 1, rowInput.end(),
                                          [&](f32 val) { return val == rowInput[0]; });

        if (allPixelsEqual) {
            intermediateBuffer[rowIndex * BLOCK_SIZE] = rowInput[0] * static_cast<f32>(BLOCK_SIZE);
            std::fill_n(&intermediateBuffer[rowIndex * BLOCK_SIZE + 1], BLOCK_SIZE - 1, 0.0f);
        } else {
            std::array<f32, BLOCK_SIZE> rowOutput{};
            fdct_1d_butterfly(rowInput, rowOutput);
            std::copy(rowOutput.begin(), rowOutput.end(),
                      &intermediateBuffer[rowIndex * BLOCK_SIZE]);
        }
    }

    // Column pass (vertical 1-D forward DCT + 0.125 normalisation).
    std::array<f32, BLOCK_PIXELS> dctCoefficients{};
    for (i32 columnIndex = 0; columnIndex < BLOCK_SIZE; columnIndex++) {
        std::array<f32, BLOCK_SIZE> columnInput{};
        for (i32 rowIndex = 0; rowIndex < BLOCK_SIZE; rowIndex++) {
            columnInput[rowIndex] = intermediateBuffer[rowIndex * BLOCK_SIZE + columnIndex];
        }

        std::array<f32, BLOCK_SIZE> columnOutput{};
        fdct_1d_butterfly(columnInput, columnOutput);

        for (i32 rowIndex = 0; rowIndex < BLOCK_SIZE; rowIndex++) {
            dctCoefficients[rowIndex * BLOCK_SIZE + columnIndex] =
                columnOutput[rowIndex] * DCT_2D_NORMALISATION;
        }
    }

    // Quantise and reorder to zig-zag.
    // The AAN butterfly leaves each coefficient scaled by aanscale[row] * aanscale[col].
    // Fold these scale factors into the quantisation divisor to produce standard values.
    for (i32 zigzagIndex = 0; zigzagIndex < BLOCK_PIXELS; zigzagIndex++) {
        i32 naturalPosition = ZIGZAG_ORDER[zigzagIndex];
        i32 row = naturalPosition / BLOCK_SIZE;
        i32 col = naturalPosition % BLOCK_SIZE;
        f32 divisor = static_cast<f32>(quantTable[zigzagIndex]) *
                      AAN_SCALE_FACTORS[row] * AAN_SCALE_FACTORS[col];
        f32 coefficientValue = dctCoefficients[naturalPosition] / divisor;
        // Round to nearest integer (away from zero for tie-breaking).
        quantisedCoeffs[zigzagIndex] = static_cast<i32>(
            coefficientValue > 0.0f ? coefficientValue + 0.5f : coefficientValue - 0.5f);
    }
}

// ============================================================================
// JPEG Category (Bit-Size) Encoding Helpers
// ============================================================================

/// Compute the category (number of bits) needed to encode a signed value.
/// JPEG DC/AC coefficients use a magnitude + sign encoding: the category is
/// ceil(log2(|value| + 1)), and the magnitude bits are the value itself for
/// positive values or the one's complement for negative values.
i32 compute_category(i32 value) {
    if (value == 0) {
        return 0;
    }
    i32 absoluteValue = value < 0 ? -value : value;
    i32 category = 0;
    while (absoluteValue > 0) {
        category++;
        absoluteValue >>= 1;
    }
    return category;
}

/// Compute the magnitude bits for a signed value in its category.
/// For positive values, the magnitude is the value itself.
/// For negative values, the magnitude is (value - 1) masked to the category
/// width (one's complement encoding).
u32 compute_magnitude_bits(i32 value, i32 category) {
    if (value >= 0) {
        return static_cast<u32>(value);
    }
    return static_cast<u32>(value + (1 << category) - 1);
}

// ============================================================================
// Quantisation Table Construction with Quality Scaling
// ============================================================================

/// Build a quantisation table in zig-zag order, scaled by the quality factor.
///
/// Quality scaling follows the standard libjpeg convention:
///   quality < 50  →  scale = 5000 / quality
///   quality >= 50 →  scale = 200 - 2 * quality
/// Each table entry is clamped to [1, 255] for 8-bit DQT precision.
std::array<u16, BLOCK_PIXELS> build_quant_table(i32 quality) {
    quality = std::clamp(quality, 1, 100);
    i32 scaleFactor =
        (quality < 50) ? (QUALITY_LOW_NUMERATOR / quality) : (QUALITY_HIGH_BASE - quality * 2);

    std::array<u16, BLOCK_PIXELS> quantTableZigzag{};
    for (i32 zigzagIndex = 0; zigzagIndex < BLOCK_PIXELS; zigzagIndex++) {
        i32 naturalPosition = ZIGZAG_ORDER[zigzagIndex];
        i32 baseValue = static_cast<i32>(STD_LUMINANCE_QUANT_NATURAL[naturalPosition]);
        i32 scaledValue = (baseValue * scaleFactor + 50) / 100;
        scaledValue = std::clamp(scaledValue, 1, 255);
        quantTableZigzag[zigzagIndex] = static_cast<u16>(scaledValue);
    }
    return quantTableZigzag;
}

// ============================================================================
// Marker Writers
// ============================================================================

void write_u8(std::vector<u8>& out, u8 value) {
    out.push_back(value);
}

void write_u16_be(std::vector<u8>& out, u16 value) {
    out.push_back(static_cast<u8>(value >> 8));
    out.push_back(static_cast<u8>(value & 0xFF));
}

void write_marker(std::vector<u8>& out, u8 markerCode) {
    out.push_back(0xFF);
    out.push_back(markerCode);
}

/// Write the SOI (Start of Image) marker.
void write_soi(std::vector<u8>& out) {
    write_marker(out, MARKER_SOI);
}

/// Write a DQT (Define Quantization Table) marker segment (8-bit precision).
void write_dqt(std::vector<u8>& out, u8 tableIndex,
               const std::array<u16, BLOCK_PIXELS>& quantTable) {
    write_marker(out, MARKER_DQT);
    // Length: 2 (length field) + 1 (table info) + 64 (8-bit values)
    write_u16_be(out, DQT_8BIT_SEGMENT_LENGTH);
    write_u8(out, tableIndex); // Upper nibble = 0 (8-bit precision), lower = index.
    for (i32 coefficient = 0; coefficient < BLOCK_PIXELS; coefficient++) {
        write_u8(out, static_cast<u8>(quantTable[coefficient]));
    }
}

/// Write a SOF0 (Baseline DCT Frame Header) marker segment.
void write_sof0(std::vector<u8>& out, u32 width, u32 height, u32 componentCount) {
    write_marker(out, MARKER_SOF0);
    // Length: 2 + 1 (precision) + 2 (height) + 2 (width) + 1 (num components)
    //       + componentCount * 3
    u16 segmentLength = static_cast<u16>(8 + componentCount * 3);
    write_u16_be(out, segmentLength);
    write_u8(out, 8); // 8-bit sample precision.
    write_u16_be(out, static_cast<u16>(height));
    write_u16_be(out, static_cast<u16>(width));
    write_u8(out, static_cast<u8>(componentCount));

    for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
        write_u8(out, static_cast<u8>(componentIndex + 1)); // Component ID (1-based).
        write_u8(out, SAMPLING_FACTOR_1x1); // Sampling factors: 1x1 (no subsampling).
        write_u8(out, 0);                   // Quantisation table index 0 for all.
    }
}

/// Write a DHT (Define Huffman Table) marker segment.
/// @param tableClass 0 = DC, 1 = AC.
void write_dht(std::vector<u8>& out, u8 tableClass, u8 tableIndex, const u8* lengthCounts,
               const u8* symbols, i32 symbolCount) {
    write_marker(out, MARKER_DHT);
    // Length: 2 + 1 (table info) + 16 (length counts) + symbolCount
    u16 segmentLength = static_cast<u16>(2 + 1 + 16 + symbolCount);
    write_u16_be(out, segmentLength);
    write_u8(out, static_cast<u8>((tableClass << 4) | tableIndex));
    for (i32 lengthIndex = 0; lengthIndex < 16; lengthIndex++) {
        write_u8(out, lengthCounts[lengthIndex]);
    }
    for (i32 symbolIndex = 0; symbolIndex < symbolCount; symbolIndex++) {
        write_u8(out, symbols[symbolIndex]);
    }
}

/// Write the SOS (Start of Scan) marker segment.
void write_sos(std::vector<u8>& out, u32 componentCount) {
    write_marker(out, MARKER_SOS);
    // Length: 2 + 1 (num components) + componentCount * 2 + 3 (spectral selection)
    u16 segmentLength = static_cast<u16>(6 + componentCount * 2);
    write_u16_be(out, segmentLength);
    write_u8(out, static_cast<u8>(componentCount));

    for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
        write_u8(out, static_cast<u8>(componentIndex + 1)); // Component selector (matches SOF0 ID).
        write_u8(out, 0x00);                                // DC table 0, AC table 0 for all.
    }

    // Spectral selection: Ss=0, Se=63, Ah|Al = 0 (baseline).
    write_u8(out, 0);  // Ss
    write_u8(out, 63); // Se
    write_u8(out, 0);  // Ah=0, Al=0
}

/// Write the EOI (End of Image) marker.
void write_eoi(std::vector<u8>& out) {
    write_marker(out, MARKER_EOI);
}

// ============================================================================
// Entropy Encoding
// ============================================================================

/// Encode a single DC coefficient (differential coding).
void encode_dc_coefficient(BitstreamWriter& writer, const HuffmanEncodeTable& dcTable,
                           i32 dcDifference) {
    i32 category = compute_category(dcDifference);
    const auto& hcode = dcTable.codes[category];
    writer.writeBits(hcode.code, hcode.length);
    if (category > 0) {
        u32 magnitude = compute_magnitude_bits(dcDifference, category);
        writer.writeBits(magnitude, category);
    }
}

/// Encode the AC coefficients of an 8x8 block (run-length coding).
/// @param coeffs Quantised coefficients in zig-zag order (positions 1-63 are AC).
void encode_ac_coefficients(BitstreamWriter& writer, const HuffmanEncodeTable& acTable,
                            const std::array<i32, BLOCK_PIXELS>& coeffs) {
    i32 consecutiveZeros = 0;

    for (i32 coefficientIndex = 1; coefficientIndex < BLOCK_PIXELS; coefficientIndex++) {
        i32 currentCoefficient = coeffs[coefficientIndex];

        if (currentCoefficient == 0) {
            consecutiveZeros++;
            continue;
        }

        // Emit ZRL (zero run length of 16) symbols for runs longer than 15.
        while (consecutiveZeros > 15) {
            const auto& zrlCode = acTable.codes[0xF0]; // Symbol 0xF0 = ZRL
            writer.writeBits(zrlCode.code, zrlCode.length);
            consecutiveZeros -= 16;
        }

        i32 category = compute_category(currentCoefficient);
        u8 runLengthSymbol = static_cast<u8>((consecutiveZeros << 4) | category);
        const auto& huffmanCode = acTable.codes[runLengthSymbol];
        writer.writeBits(huffmanCode.code, huffmanCode.length);
        u32 magnitudeBits = compute_magnitude_bits(currentCoefficient, category);
        writer.writeBits(magnitudeBits, category);
        consecutiveZeros = 0;
    }

    // If the block ends with zeros, emit the EOB (End of Block) symbol.
    if (consecutiveZeros > 0) {
        const auto& eobCode = acTable.codes[0x00]; // Symbol 0x00 = EOB
        writer.writeBits(eobCode.code, eobCode.length);
    }
}

// ============================================================================
// Encoder State Machine
// ============================================================================

struct BaselineEncoder {
    std::vector<u8> outputBuffer;

    std::array<u16, BLOCK_PIXELS> quantTable{};
    HuffmanEncodeTable dcHuffTable;
    HuffmanEncodeTable acHuffTable;

    u32 imageWidth = 0;
    u32 imageHeight = 0;
    u32 componentCount = 0;

    u32 mcuColumnsCount = 0;
    u32 mcuRowsCount = 0;

    std::string* errorOutput = nullptr;

    // -- Helpers --

    bool reportError(const std::string& message) {
        if (errorOutput) {
            *errorOutput = message;
        }
        return false;
    }

    // -- Encoding Steps --

    /// Prepare per-component sample buffers from the interleaved input image.
    /// Each component is extracted into a separate MCU-aligned buffer with
    /// edge pixels replicated to fill partial MCUs.
    void buildComponentBuffers(const Image& image,
                               std::array<std::vector<u8>, MAX_COMPONENTS>& buffers,
                               std::array<u32, MAX_COMPONENTS>& strides);

    /// Encode all MCUs into the entropy-coded segment.
    bool encodeScanData(const std::array<std::vector<u8>, MAX_COMPONENTS>& buffers,
                        const std::array<u32, MAX_COMPONENTS>& strides);

    /// Top-level entry point.
    bool encode(const Image& image, i32 quality);
};

/// Extract each component from the interleaved image into a separate buffer
/// whose dimensions are rounded up to the next multiple of 8.  Edge pixels
/// are replicated to avoid boundary artefacts in the DCT.
void BaselineEncoder::buildComponentBuffers(const Image& image,
                                            std::array<std::vector<u8>, MAX_COMPONENTS>& buffers,
                                            std::array<u32, MAX_COMPONENTS>& strides) {
    u32 paddedWidth = ((imageWidth + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;
    u32 paddedHeight = ((imageHeight + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;

    for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
        strides[componentIndex] = paddedWidth;
        buffers[componentIndex].resize(static_cast<size_t>(paddedWidth) * paddedHeight, 0);

        for (u32 destY = 0; destY < paddedHeight; destY++) {
            u32 sourceY = std::min(destY, imageHeight - 1);
            for (u32 destX = 0; destX < paddedWidth; destX++) {
                u32 sourceX = std::min(destX, imageWidth - 1);
                u32 destinationOffset = destY * paddedWidth + destX;
                u32 sourceOffset =
                    (sourceY * imageWidth + sourceX) * componentCount + componentIndex;
                buffers[componentIndex][destinationOffset] = image.pixels[sourceOffset];
            }
        }
    }
}

/// Encode all MCU blocks sequentially.  Each MCU contains one 8x8 block per
/// component (no subsampling), processed in component order.
bool BaselineEncoder::encodeScanData(const std::array<std::vector<u8>, MAX_COMPONENTS>& buffers,
                                     const std::array<u32, MAX_COMPONENTS>& strides) {

    BitstreamWriter writer;
    writer.init(&outputBuffer);

    std::array<i32, MAX_COMPONENTS> dcPredictions{};

    for (u32 mcuRow = 0; mcuRow < mcuRowsCount; mcuRow++) {
        for (u32 mcuColumn = 0; mcuColumn < mcuColumnsCount; mcuColumn++) {
            for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
                u32 blockPixelX = mcuColumn * BLOCK_SIZE;
                u32 blockPixelY = mcuRow * BLOCK_SIZE;

                const u8* blockDataPointer = buffers[componentIndex].data() +
                                             blockPixelY * strides[componentIndex] + blockPixelX;

                std::array<i32, BLOCK_PIXELS> quantisedCoefficients{};
                forward_dct_and_quantise(blockDataPointer, strides[componentIndex],
                                         quantisedCoefficients, quantTable);

                // DC: differential coding.
                i32 dcDifference = quantisedCoefficients[0] - dcPredictions[componentIndex];
                dcPredictions[componentIndex] = quantisedCoefficients[0];
                encode_dc_coefficient(writer, dcHuffTable, dcDifference);

                // AC: run-length coding.
                encode_ac_coefficients(writer, acHuffTable, quantisedCoefficients);
            }
        }
    }

    writer.flushWithPadding();
    return true;
}

bool BaselineEncoder::encode(const Image& image, i32 quality) {
    imageWidth = image.width;
    imageHeight = image.height;
    componentCount = image.components;

    if (imageWidth == 0 || imageHeight == 0) {
        return reportError("Cannot encode an image with zero dimensions");
    }
    if (componentCount == 0 || componentCount > MAX_COMPONENTS) {
        return reportError("Unsupported component count " + std::to_string(componentCount));
    }
    if (imageWidth > 65535 || imageHeight > 65535) {
        return reportError("Image dimensions exceed JPEG maximum (65535)");
    }
    if (image.pixels.size() < static_cast<size_t>(imageWidth) * imageHeight * componentCount) {
        return reportError("Pixel buffer too small for the specified dimensions");
    }

    // Build quantisation table.
    quantTable = build_quant_table(quality);

    // Build Huffman encoding tables from the standard Annex K tables.
    dcHuffTable.build(DC_LUMA_COUNTS.data(), DC_LUMA_SYMBOLS.data(),
                      static_cast<i32>(DC_LUMA_SYMBOLS.size()));
    acHuffTable.build(AC_LUMA_COUNTS.data(), AC_LUMA_SYMBOLS.data(),
                      static_cast<i32>(AC_LUMA_SYMBOLS.size()));

    // Estimate output size (typical JPEG is 1-3 bytes/pixel).
    outputBuffer.clear();
    outputBuffer.reserve(static_cast<size_t>(imageWidth) * imageHeight * componentCount);

    mcuColumnsCount = (imageWidth + BLOCK_SIZE - 1) / BLOCK_SIZE;
    mcuRowsCount = (imageHeight + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // --- Write JPEG marker segments ---

    write_soi(outputBuffer);
    write_dqt(outputBuffer, 0, quantTable);
    write_sof0(outputBuffer, imageWidth, imageHeight, componentCount);
    write_dht(outputBuffer, 0, 0, DC_LUMA_COUNTS.data(), DC_LUMA_SYMBOLS.data(),
              static_cast<i32>(DC_LUMA_SYMBOLS.size()));
    write_dht(outputBuffer, 1, 0, AC_LUMA_COUNTS.data(), AC_LUMA_SYMBOLS.data(),
              static_cast<i32>(AC_LUMA_SYMBOLS.size()));
    write_sos(outputBuffer, componentCount);

    // --- Encode entropy-coded segment ---

    std::array<std::vector<u8>, MAX_COMPONENTS> componentBuffers;
    std::array<u32, MAX_COMPONENTS> componentStrides{};
    buildComponentBuffers(image, componentBuffers, componentStrides);

    if (!encodeScanData(componentBuffers, componentStrides)) {
        return false;
    }

    write_eoi(outputBuffer);
    return true;
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

std::vector<u8> encode_raw(const Image& image, i32 quality, std::string* out_error) {
    BaselineEncoder encoder;
    encoder.errorOutput = out_error;
    if (!encoder.encode(image, quality)) {
        return {};
    }
    return std::move(encoder.outputBuffer);
}

} // namespace whiteout::textures::jpeg
