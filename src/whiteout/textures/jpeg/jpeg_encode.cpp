// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// JPEG encoder supporting baseline (SOF0) and progressive (SOF2) modes.
///
/// Encodes raw component values without RGB-to-Y'CbCr conversion.  BLP files
/// store BGRA colour components directly in the JPEG data stream, so the raw
/// values must be preserved.
///
/// Supported:   Baseline sequential DCT (SOF0), progressive DCT (SOF2),
///              8-bit precision, 1-4 channels, configurable quality (1-100),
///              no chroma subsampling.
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
// Progressive Scan Band Table
// ============================================================================

/// Default spectral band split for progressive AC scans.
/// Low-frequency coefficients (1-5) are sent first for coarse structural detail,
/// then high-frequency coefficients (6-63) for texture/edges.
struct SpectralBand {
    u8 ss;
    u8 se;
};

static constexpr std::array<SpectralBand, 2> DEFAULT_AC_BANDS = {{
    {1, 5},
    {6, 63},
}};

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

/// Write a SOF2 (Progressive DCT Frame Header) marker segment.
/// Identical format to SOF0, just a different marker byte.
void write_sof2(std::vector<u8>& out, u32 width, u32 height, u32 componentCount) {
    write_marker(out, MARKER_SOF2);
    u16 segmentLength = static_cast<u16>(8 + componentCount * 3);
    write_u16_be(out, segmentLength);
    write_u8(out, 8); // 8-bit sample precision.
    write_u16_be(out, static_cast<u16>(height));
    write_u16_be(out, static_cast<u16>(width));
    write_u8(out, static_cast<u8>(componentCount));

    for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
        write_u8(out, static_cast<u8>(componentIndex + 1));
        write_u8(out, SAMPLING_FACTOR_1x1);
        write_u8(out, 0);
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

/// Write a SOS marker for a progressive scan with custom spectral/approximation parameters.
/// @param componentIds  1-based component IDs to include in the scan.
/// @param dcTableIds    DC Huffman table index per component.
/// @param acTableIds    AC Huffman table index per component.
/// @param ss            Spectral selection start (0 for DC, >0 for AC).
/// @param se            Spectral selection end (0 for DC-only, 63 for full AC).
/// @param ah            Successive approximation high bit.
/// @param al            Successive approximation low bit.
void write_sos_progressive(std::vector<u8>& out,
                           const std::vector<u8>& componentIds,
                           const std::vector<u8>& dcTableIds,
                           const std::vector<u8>& acTableIds,
                           u8 ss, u8 se, u8 ah, u8 al) {
    u32 scanComponentCount = static_cast<u32>(componentIds.size());
    write_marker(out, MARKER_SOS);
    u16 segmentLength = static_cast<u16>(6 + scanComponentCount * 2);
    write_u16_be(out, segmentLength);
    write_u8(out, static_cast<u8>(scanComponentCount));

    for (u32 i = 0; i < scanComponentCount; ++i) {
        write_u8(out, componentIds[i]);
        write_u8(out, static_cast<u8>((dcTableIds[i] << 4) | acTableIds[i]));
    }

    write_u8(out, ss);
    write_u8(out, se);
    write_u8(out, static_cast<u8>((ah << 4) | al));
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

/// Return the Huffman table index for a given component (0 = luma, 1 = chroma).
u8 huffTableIndex(u32 compIndex, u32 componentCount) {
    return (componentCount > 1 && compIndex > 0) ? 1 : 0;
}

// ============================================================================
// Encoder State Machine
// ============================================================================

struct BaselineEncoder {
    std::vector<u8> outputBuffer;

    std::array<u16, BLOCK_PIXELS> quantTable{};
    std::array<HuffmanEncodeTable, 2> dcHuffTables;
    std::array<HuffmanEncodeTable, 2> acHuffTables;

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

    /// Encode all MCUs into the entropy-coded segment (baseline path).
    bool encodeScanData(const std::array<std::vector<u8>, MAX_COMPONENTS>& buffers,
                        const std::array<u32, MAX_COMPONENTS>& strides);

    /// FDCT + quantise all blocks for all components into coefficient storage.
    void buildCoefficientBlocks(const std::array<std::vector<u8>, MAX_COMPONENTS>& buffers,
                                const std::array<u32, MAX_COMPONENTS>& strides,
                                std::array<std::vector<std::array<i32, BLOCK_PIXELS>>,
                                           MAX_COMPONENTS>& coeffBlocks);

    /// Encode progressive DC first-pass scan (all components interleaved).
    /// @param al  Successive approximation low bit (0 = no SA).
    bool encodeProgressiveDcScan(
        const std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks,
        i32 al = 0);

    /// Encode progressive DC refinement scan (all components, one bit per block).
    /// @param al  The bit position to refine.
    bool encodeProgressiveDcRefineScan(
        const std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks,
        i32 al);

    /// Encode progressive AC first-pass scan for one component with EOBRUN.
    /// @param ss  Spectral selection start.
    /// @param se  Spectral selection end.
    /// @param al  Successive approximation low bit (0 = no SA).
    bool encodeProgressiveAcScan(
        const std::vector<std::array<i32, BLOCK_PIXELS>>& coeffBlocks, u32 compIndex,
        i32 ss, i32 se, i32 al = 0);

    /// Encode progressive AC refinement scan for one component.
    /// @param ss  Spectral selection start.
    /// @param se  Spectral selection end.
    /// @param al  The bit position to refine.
    bool encodeProgressiveAcRefineScan(
        const std::vector<std::array<i32, BLOCK_PIXELS>>& coeffBlocks, u32 compIndex,
        i32 ss, i32 se, i32 al);

    /// Top-level entry points.
    bool encode(const Image& image, i32 quality);
    bool encodeProgressive(const Image& image, i32 quality);
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

                u8 tblIdx = huffTableIndex(componentIndex, componentCount);

                // DC: differential coding.
                i32 dcDifference = quantisedCoefficients[0] - dcPredictions[componentIndex];
                dcPredictions[componentIndex] = quantisedCoefficients[0];
                encode_dc_coefficient(writer, dcHuffTables[tblIdx], dcDifference);

                // AC: run-length coding.
                encode_ac_coefficients(writer, acHuffTables[tblIdx], quantisedCoefficients);
            }
        }
    }

    writer.flushWithPadding();
    return true;
}

// ============================================================================
// Progressive Encoding Helpers
// ============================================================================

void BaselineEncoder::buildCoefficientBlocks(
    const std::array<std::vector<u8>, MAX_COMPONENTS>& buffers,
    const std::array<u32, MAX_COMPONENTS>& strides,
    std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks) {

    u32 totalBlocks = mcuColumnsCount * mcuRowsCount;
    for (u32 c = 0; c < componentCount; c++) {
        coeffBlocks[c].resize(totalBlocks);
    }

    for (u32 mcuRow = 0; mcuRow < mcuRowsCount; mcuRow++) {
        for (u32 mcuCol = 0; mcuCol < mcuColumnsCount; mcuCol++) {
            u32 blockIndex = mcuRow * mcuColumnsCount + mcuCol;
            for (u32 c = 0; c < componentCount; c++) {
                u32 blockPixelX = mcuCol * BLOCK_SIZE;
                u32 blockPixelY = mcuRow * BLOCK_SIZE;
                const u8* blockDataPointer =
                    buffers[c].data() + blockPixelY * strides[c] + blockPixelX;
                forward_dct_and_quantise(blockDataPointer, strides[c],
                                         coeffBlocks[c][blockIndex], quantTable);
            }
        }
    }
}

bool BaselineEncoder::encodeProgressiveDcScan(
    const std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks,
    i32 al) {

    BitstreamWriter writer;
    writer.init(&outputBuffer);

    std::array<i32, MAX_COMPONENTS> dcPredictions{};
    u32 totalBlocks = mcuColumnsCount * mcuRowsCount;

    for (u32 blockIdx = 0; blockIdx < totalBlocks; blockIdx++) {
        for (u32 c = 0; c < componentCount; c++) {
            i32 dc = coeffBlocks[c][blockIdx][0];
            i32 dcShifted = dc >> al;
            i32 diff = dcShifted - dcPredictions[c];
            dcPredictions[c] = dcShifted;
            u8 tblIdx = huffTableIndex(c, componentCount);
            encode_dc_coefficient(writer, dcHuffTables[tblIdx], diff);
        }
    }

    writer.flushWithPadding();
    return true;
}

bool BaselineEncoder::encodeProgressiveDcRefineScan(
    const std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks,
    i32 al) {

    BitstreamWriter writer;
    writer.init(&outputBuffer);

    u32 totalBlocks = mcuColumnsCount * mcuRowsCount;

    for (u32 blockIdx = 0; blockIdx < totalBlocks; blockIdx++) {
        for (u32 c = 0; c < componentCount; c++) {
            i32 dc = coeffBlocks[c][blockIdx][0];
            u32 bit = (static_cast<u32>(dc) >> al) & 1u;
            writer.writeBits(bit, 1);
        }
    }

    writer.flushWithPadding();
    return true;
}

bool BaselineEncoder::encodeProgressiveAcScan(
    const std::vector<std::array<i32, BLOCK_PIXELS>>& coeffBlocks, u32 compIndex,
    i32 ss, i32 se, i32 al) {

    u8 tblIdx = huffTableIndex(compIndex, componentCount);
    const auto& acTable = acHuffTables[tblIdx];

    BitstreamWriter writer;
    writer.init(&outputBuffer);

    // Write a single EOB (symbol 0x00) for one all-zero block.
    // Note: Standard Huffman tables (Annex K) only include EOB0 (0x00), not
    // multi-block EOBn symbols (0x10, 0x20, ...).  Optimal Huffman coding
    // would be needed to use EOBn batching; for now we emit individual EOBs.
    auto writeEob = [&]() {
        const auto& hcode = acTable.codes[0x00];
        writer.writeBits(hcode.code, hcode.length);
    };

    for (size_t blockIdx = 0; blockIdx < coeffBlocks.size(); blockIdx++) {
        const auto& coeffs = coeffBlocks[blockIdx];

        // Check if all AC coefficients in [ss, se] are zero (after point transform).
        // Point transform uses truncation toward zero: shift absolute value, restore sign.
        bool allZero = true;
        for (i32 k = ss; k <= se; k++) {
            i32 absVal = coeffs[k] < 0 ? -coeffs[k] : coeffs[k];
            if ((absVal >> al) != 0) { allZero = false; break; }
        }

        if (allZero) {
            writeEob();
            continue;
        }

        i32 consecutiveZeros = 0;
        for (i32 k = ss; k <= se; k++) {
            // Apply point transform with truncation toward zero (ITU-T T.81 §G.1.2.2).
            i32 absVal = coeffs[k] < 0 ? -coeffs[k] : coeffs[k];
            i32 coeff = coeffs[k] < 0 ? -(absVal >> al) : (absVal >> al);
            if (coeff == 0) { consecutiveZeros++; continue; }

            while (consecutiveZeros > 15) {
                const auto& zrlCode = acTable.codes[0xF0];
                writer.writeBits(zrlCode.code, zrlCode.length);
                consecutiveZeros -= 16;
            }

            i32 cat = compute_category(coeff);
            u8 rlSymbol = static_cast<u8>((consecutiveZeros << 4) | cat);
            const auto& hc = acTable.codes[rlSymbol];
            writer.writeBits(hc.code, hc.length);
            writer.writeBits(compute_magnitude_bits(coeff, cat), cat);
            consecutiveZeros = 0;
        }

        if (consecutiveZeros > 0) {
            writeEob();
        }
    }

    writer.flushWithPadding();
    return true;
}

bool BaselineEncoder::encodeProgressiveAcRefineScan(
    const std::vector<std::array<i32, BLOCK_PIXELS>>& coeffBlocks, u32 compIndex,
    i32 ss, i32 se, i32 al) {

    u8 tblIdx = huffTableIndex(compIndex, componentCount);
    const auto& acTable = acHuffTables[tblIdx];

    BitstreamWriter writer;
    writer.init(&outputBuffer);

    // Write a single EOB (symbol 0x00) followed by any pending correction bits.
    // Note: Standard Huffman tables only include EOB0, not multi-block EOBn.
    auto writeEobWithCorrections = [&](const std::vector<u32>& corrections) {
        const auto& hcode = acTable.codes[0x00];
        writer.writeBits(hcode.code, hcode.length);
        for (u32 bit : corrections) {
            writer.writeBits(bit, 1);
        }
    };

    for (size_t blockIdx = 0; blockIdx < coeffBlocks.size(); blockIdx++) {
        const auto& coeffs = coeffBlocks[blockIdx];

        // Find the last position with a newly-nonzero coefficient (EOBPTR
        // equivalent). ZRL symbols are only emitted while we haven't passed
        // this point; beyond it any remaining run folds into an EOB.
        i32 lastNewNzPos = -1;
        for (i32 k = se; k >= ss; k--) {
            i32 absCoeff = coeffs[k] < 0 ? -coeffs[k] : coeffs[k];
            bool prevNonzero = (absCoeff >> (al + 1)) != 0;
            bool newNonzero = !prevNonzero && ((absCoeff >> al) & 1) != 0;
            if (newNonzero) { lastNewNzPos = k; break; }
        }

        if (lastNewNzPos < 0) {
            // No new nonzero coefficients — emit EOB0 with correction bits.
            std::vector<u32> corrections;
            for (i32 k = ss; k <= se; k++) {
                i32 absCoeff = coeffs[k] < 0 ? -coeffs[k] : coeffs[k];
                if ((absCoeff >> (al + 1)) != 0) {
                    corrections.push_back((static_cast<u32>(absCoeff) >> al) & 1u);
                }
            }
            writeEobWithCorrections(corrections);
            continue;
        }

        // Encode AC coefficients with refinement.
        // ZRL checks happen at every nonzero position (not just newly-nonzero)
        // so that correction bits are written in the correct interleaved order.
        i32 zerosToSkip = 0;
        std::vector<u32> localCorrections;

        for (i32 k = ss; k <= se; k++) {
            i32 absCoeff = coeffs[k] < 0 ? -coeffs[k] : coeffs[k];
            i32 shifted = absCoeff >> al;

            if (shifted == 0) {
                // Zero-history position that remains zero.
                zerosToSkip++;
                continue;
            }

            // Nonzero position (either previously-nonzero or newly-nonzero).
            // Emit pending ZRL symbols BEFORE processing this coefficient.
            // Only emit ZRL while we haven't passed the last new-nonzero
            // position; beyond it the run folds into an EOB instead.
            while (zerosToSkip > 15 && k <= lastNewNzPos) {
                const auto& zrlCode = acTable.codes[0xF0];
                writer.writeBits(zrlCode.code, zrlCode.length);
                for (u32 bit : localCorrections) {
                    writer.writeBits(bit, 1);
                }
                localCorrections.clear();
                zerosToSkip -= 16;
            }

            bool prevNonzero = (shifted >> 1) != 0;

            if (prevNonzero) {
                // Previously-nonzero: buffer correction bit (AFTER ZRL emission).
                localCorrections.push_back(static_cast<u32>(shifted) & 1u);
                continue;
            }

            // Newly-nonzero coefficient.
            // Encode (runLength, 1) + sign bit + pending correction bits.
            u8 rlSymbol = static_cast<u8>((zerosToSkip << 4) | 1);
            const auto& hc = acTable.codes[rlSymbol];
            writer.writeBits(hc.code, hc.length);

            u32 signBit = (coeffs[k] >= 0) ? 1u : 0u;
            writer.writeBits(signBit, 1);

            for (u32 bit : localCorrections) {
                writer.writeBits(bit, 1);
            }
            localCorrections.clear();
            zerosToSkip = 0;
        }

        // Trailing run with no more new nonzeros — emit EOB0 with corrections.
        if (zerosToSkip > 0 || !localCorrections.empty()) {
            writeEobWithCorrections(localCorrections);
        }
    }

    writer.flushWithPadding();
    return true;
}

bool BaselineEncoder::encodeProgressive(const Image& image, i32 quality) {
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

    quantTable = build_quant_table(quality);

    // Build Huffman tables: table 0 = luminance, table 1 = chrominance.
    dcHuffTables[0].build(DC_LUMA_COUNTS.data(), DC_LUMA_SYMBOLS.data(),
                          static_cast<i32>(DC_LUMA_SYMBOLS.size()));
    acHuffTables[0].build(AC_LUMA_COUNTS.data(), AC_LUMA_SYMBOLS.data(),
                          static_cast<i32>(AC_LUMA_SYMBOLS.size()));
    if (componentCount > 1) {
        dcHuffTables[1].build(DC_CHROMA_COUNTS.data(), DC_CHROMA_SYMBOLS.data(),
                              static_cast<i32>(DC_CHROMA_SYMBOLS.size()));
        acHuffTables[1].build(AC_CHROMA_COUNTS.data(), AC_CHROMA_SYMBOLS.data(),
                              static_cast<i32>(AC_CHROMA_SYMBOLS.size()));
    }

    outputBuffer.clear();
    outputBuffer.reserve(static_cast<size_t>(imageWidth) * imageHeight * componentCount);

    mcuColumnsCount = (imageWidth + BLOCK_SIZE - 1) / BLOCK_SIZE;
    mcuRowsCount = (imageHeight + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // --- Build all quantised coefficient blocks ---

    std::array<std::vector<u8>, MAX_COMPONENTS> componentBuffers;
    std::array<u32, MAX_COMPONENTS> componentStrides{};
    buildComponentBuffers(image, componentBuffers, componentStrides);

    std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS> coeffBlocks;
    buildCoefficientBlocks(componentBuffers, componentStrides, coeffBlocks);

    for (u32 c = 0; c < componentCount; c++) {
        componentBuffers[c] = {};
    }

    // --- Successive approximation parameters ---
    // Al=1: first pass encodes top bits, one refinement pass fills bit 0.
    constexpr i32 SA_AL = 1; // First pass encodes top bits, one refinement pass fills bit 0.

    // --- Write marker segments ---

    write_soi(outputBuffer);
    write_dqt(outputBuffer, 0, quantTable);
    write_sof2(outputBuffer, imageWidth, imageHeight, componentCount);

    // DHT: luminance tables (DC class 0 index 0, AC class 1 index 0).
    write_dht(outputBuffer, 0, 0, DC_LUMA_COUNTS.data(), DC_LUMA_SYMBOLS.data(),
              static_cast<i32>(DC_LUMA_SYMBOLS.size()));
    write_dht(outputBuffer, 1, 0, AC_LUMA_COUNTS.data(), AC_LUMA_SYMBOLS.data(),
              static_cast<i32>(AC_LUMA_SYMBOLS.size()));

    // DHT: chrominance tables (DC class 0 index 1, AC class 1 index 1).
    if (componentCount > 1) {
        write_dht(outputBuffer, 0, 1, DC_CHROMA_COUNTS.data(), DC_CHROMA_SYMBOLS.data(),
                  static_cast<i32>(DC_CHROMA_SYMBOLS.size()));
        write_dht(outputBuffer, 1, 1, AC_CHROMA_COUNTS.data(), AC_CHROMA_SYMBOLS.data(),
                  static_cast<i32>(AC_CHROMA_SYMBOLS.size()));
    }

    // --- DC first pass: all components interleaved, Ss=0, Se=0, Ah=0, Al=SA_AL ---
    {
        std::vector<u8> compIds(componentCount);
        std::vector<u8> dcIds(componentCount);
        std::vector<u8> acIds(componentCount, 0);
        for (u32 c = 0; c < componentCount; c++) {
            compIds[c] = static_cast<u8>(c + 1);
            dcIds[c] = huffTableIndex(c, componentCount);
        }
        write_sos_progressive(outputBuffer, compIds, dcIds, acIds, 0, 0, 0, SA_AL);
        if (!encodeProgressiveDcScan(coeffBlocks, SA_AL)) return false;
    }

    // --- AC first passes: per component, per band, Ah=0, Al=SA_AL ---
    for (u32 c = 0; c < componentCount; c++) {
        u8 tblIdx = huffTableIndex(c, componentCount);
        for (const auto& band : DEFAULT_AC_BANDS) {
            std::vector<u8> compIds = { static_cast<u8>(c + 1) };
            std::vector<u8> dcIds = { 0 };
            std::vector<u8> acIds = { tblIdx };
            write_sos_progressive(outputBuffer, compIds, dcIds, acIds,
                                  band.ss, band.se, 0, SA_AL);
            if (!encodeProgressiveAcScan(coeffBlocks[c], c, band.ss, band.se, SA_AL))
                return false;
        }
    }

    // --- Refinement passes (only when SA_AL > 0) ---
    if (SA_AL > 0) {
        // DC refinement: all components, Ss=0, Se=0, Ah=SA_AL, Al=0
        {
            std::vector<u8> compIds(componentCount);
            std::vector<u8> dcIds(componentCount);
            std::vector<u8> acIds(componentCount, 0);
            for (u32 c = 0; c < componentCount; c++) {
                compIds[c] = static_cast<u8>(c + 1);
                dcIds[c] = huffTableIndex(c, componentCount);
            }
            write_sos_progressive(outputBuffer, compIds, dcIds, acIds, 0, 0, SA_AL, 0);
            if (!encodeProgressiveDcRefineScan(coeffBlocks, 0)) return false;
        }

        // AC refinement: per component, per band, Ah=SA_AL, Al=0
        for (u32 c = 0; c < componentCount; c++) {
            u8 tblIdx = huffTableIndex(c, componentCount);
            for (const auto& band : DEFAULT_AC_BANDS) {
                std::vector<u8> compIds = { static_cast<u8>(c + 1) };
                std::vector<u8> dcIds = { 0 };
                std::vector<u8> acIds = { tblIdx };
                write_sos_progressive(outputBuffer, compIds, dcIds, acIds,
                                      band.ss, band.se, SA_AL, 0);
                if (!encodeProgressiveAcRefineScan(coeffBlocks[c], c, band.ss, band.se, 0))
                    return false;
            }
        }
    }

    write_eoi(outputBuffer);
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

    // Build Huffman encoding tables: table 0 = luminance, table 1 = chrominance.
    dcHuffTables[0].build(DC_LUMA_COUNTS.data(), DC_LUMA_SYMBOLS.data(),
                          static_cast<i32>(DC_LUMA_SYMBOLS.size()));
    acHuffTables[0].build(AC_LUMA_COUNTS.data(), AC_LUMA_SYMBOLS.data(),
                          static_cast<i32>(AC_LUMA_SYMBOLS.size()));
    if (componentCount > 1) {
        dcHuffTables[1].build(DC_CHROMA_COUNTS.data(), DC_CHROMA_SYMBOLS.data(),
                              static_cast<i32>(DC_CHROMA_SYMBOLS.size()));
        acHuffTables[1].build(AC_CHROMA_COUNTS.data(), AC_CHROMA_SYMBOLS.data(),
                              static_cast<i32>(AC_CHROMA_SYMBOLS.size()));
    }

    // Estimate output size (typical JPEG is 1-3 bytes/pixel).
    outputBuffer.clear();
    outputBuffer.reserve(static_cast<size_t>(imageWidth) * imageHeight * componentCount);

    mcuColumnsCount = (imageWidth + BLOCK_SIZE - 1) / BLOCK_SIZE;
    mcuRowsCount = (imageHeight + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // --- Write JPEG marker segments ---

    write_soi(outputBuffer);
    write_dqt(outputBuffer, 0, quantTable);
    write_sof0(outputBuffer, imageWidth, imageHeight, componentCount);

    // DHT: luminance tables.
    write_dht(outputBuffer, 0, 0, DC_LUMA_COUNTS.data(), DC_LUMA_SYMBOLS.data(),
              static_cast<i32>(DC_LUMA_SYMBOLS.size()));
    write_dht(outputBuffer, 1, 0, AC_LUMA_COUNTS.data(), AC_LUMA_SYMBOLS.data(),
              static_cast<i32>(AC_LUMA_SYMBOLS.size()));

    // DHT: chrominance tables (only for multi-component images).
    if (componentCount > 1) {
        write_dht(outputBuffer, 0, 1, DC_CHROMA_COUNTS.data(), DC_CHROMA_SYMBOLS.data(),
                  static_cast<i32>(DC_CHROMA_SYMBOLS.size()));
        write_dht(outputBuffer, 1, 1, AC_CHROMA_COUNTS.data(), AC_CHROMA_SYMBOLS.data(),
                  static_cast<i32>(AC_CHROMA_SYMBOLS.size()));
    }

    // SOS: per-component table IDs.
    {
        write_marker(outputBuffer, MARKER_SOS);
        u16 segmentLength = static_cast<u16>(6 + componentCount * 2);
        write_u16_be(outputBuffer, segmentLength);
        write_u8(outputBuffer, static_cast<u8>(componentCount));

        for (u32 c = 0; c < componentCount; c++) {
            write_u8(outputBuffer, static_cast<u8>(c + 1));
            u8 tblIdx = huffTableIndex(c, componentCount);
            write_u8(outputBuffer, static_cast<u8>((tblIdx << 4) | tblIdx));
        }
        write_u8(outputBuffer, 0);  // Ss
        write_u8(outputBuffer, 63); // Se
        write_u8(outputBuffer, 0);  // Ah=0, Al=0
    }

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

std::vector<u8> encode_raw(const Image& image, i32 quality, std::string* out_error,
                           bool progressive) {
    BaselineEncoder encoder;
    encoder.errorOutput = out_error;
    bool ok = progressive ? encoder.encodeProgressive(image, quality)
                          : encoder.encode(image, quality);
    if (!ok) {
        return {};
    }
    return std::move(encoder.outputBuffer);
}

} // namespace whiteout::textures::jpeg
