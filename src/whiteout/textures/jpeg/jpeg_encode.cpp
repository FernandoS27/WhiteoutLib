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
#include <bit>
#include <cmath>
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
    SQRT2_F* dct_cos(1),
    SQRT2_F* dct_cos(2),
    SQRT2_F* dct_cos(3),
    SQRT2_F* dct_cos(4),
    SQRT2_F* dct_cos(5),
    SQRT2_F* dct_cos(6),
    SQRT2_F* dct_cos(7),
}};

// ============================================================================
// Reverse Zig-Zag Table
// ============================================================================

/// Maps natural (row-major) position to zig-zag index.  Inverse of ZIGZAG_ORDER.
static constexpr auto NATURAL_TO_ZIGZAG = []() {
    std::array<u8, BLOCK_PIXELS> table{};
    for (i32 zz = 0; zz < BLOCK_PIXELS; ++zz) {
        table[ZIGZAG_ORDER[zz]] = static_cast<u8>(zz);
    }
    return table;
}();

/// Build a combined reciprocal table in natural (row-major) order.
/// Each element is DCT_2D_NORMALISATION / (quantTable[zz] * AAN[row] * AAN[col]),
/// folding the 0.125 normalisation, AAN scale compensation, and quantisation
/// divisor into a single multiply-per-coefficient.
std::array<f32, BLOCK_PIXELS> build_quant_reciprocal_natural(
    const std::array<u16, BLOCK_PIXELS>& quantTableZigzag) {
    std::array<f32, BLOCK_PIXELS> reciprocal{};
    for (i32 nat = 0; nat < BLOCK_PIXELS; ++nat) {
        const i32 zz = NATURAL_TO_ZIGZAG[nat];
        const i32 row = nat / BLOCK_SIZE;
        const i32 col = nat % BLOCK_SIZE;
        const f32 divisor = static_cast<f32>(quantTableZigzag[zz]) * AAN_SCALE_FACTORS[row] *
                            AAN_SCALE_FACTORS[col];
        reciprocal[nat] = DCT_2D_NORMALISATION / divisor;
    }
    return reciprocal;
}

// ============================================================================
// Forward DCT — Arai-Agui-Nakajima (AAN) Butterfly
// ============================================================================

/// 1-D in-place forward DCT on 8 contiguous floats (AAN algorithm).
///
/// Loads all inputs into registers, computes the butterfly, and writes
/// outputs back to the same buffer.  Operates on raw f32* for cache-friendly
/// access when used with transpose_8x8_inplace.
static inline void fdct_1d_inplace(f32* data) {
    const f32 d0 = data[0], d1 = data[1], d2 = data[2], d3 = data[3];
    const f32 d4 = data[4], d5 = data[5], d6 = data[6], d7 = data[7];

    // Stage 1: Spatial butterfly (sum/difference pairs).
    const f32 tmp0 = d0 + d7, tmp7 = d0 - d7;
    const f32 tmp1 = d1 + d6, tmp6 = d1 - d6;
    const f32 tmp2 = d2 + d5, tmp5 = d2 - d5;
    const f32 tmp3 = d3 + d4, tmp4 = d3 - d4;

    // Even part: DCT positions 0, 2, 4, 6.
    const f32 es03 = tmp0 + tmp3, ed03 = tmp0 - tmp3;
    const f32 es12 = tmp1 + tmp2, ed12 = tmp1 - tmp2;
    const f32 z1 = (ed12 + ed03) * COS_PI_OVER_4;

    // Odd part: DCT positions 1, 3, 5, 7.
    const f32 os45 = tmp4 + tmp5;
    const f32 os56 = tmp5 + tmp6;
    const f32 os67 = tmp6 + tmp7;
    const f32 z5 = (os45 - os67) * SIN_PI_OVER_8;
    const f32 z2 = EVEN_ROTATION_K * os45 + z5;
    const f32 z4 = SQRT2_COS_PI_OVER_8 * os67 + z5;
    const f32 z3 = os56 * COS_PI_OVER_4;
    const f32 z11 = tmp7 + z3;
    const f32 z13 = tmp7 - z3;

    data[0] = es03 + es12;
    data[4] = es03 - es12;
    data[2] = ed03 + z1;
    data[6] = ed03 - z1;
    data[5] = z13 + z2;
    data[3] = z13 - z2;
    data[1] = z11 + z4;
    data[7] = z11 - z4;
}

/// Transpose an 8x8 float matrix in-place.
/// Turns rows into columns for contiguous-access FDCT passes.
static inline void transpose_8x8_inplace(f32* block) {
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

/// Apply the 2-D forward DCT to an 8x8 pixel block, then quantise.
///
/// Strategy: load + level-shift → row FDCT → transpose → column FDCT
/// (contiguous) → transpose back → quantise with precomputed reciprocals
/// → reorder to zig-zag.
///
/// @param inputPixels           Pointer to the top-left pixel of the 8x8 block.
/// @param inputRowStride        Byte stride between rows of the input buffer.
/// @param quantisedCoeffs       Output: quantised coefficients in zig-zag order.
/// @param quantReciprocalNatural  Precomputed reciprocal table in natural order
///                              (folds 0.125 normalisation + AAN scale + quant).
void forward_dct_and_quantise(const u8* inputPixels, u32 inputRowStride,
                              std::array<i32, BLOCK_PIXELS>& quantisedCoeffs,
                              const std::array<f32, BLOCK_PIXELS>& quantReciprocalNatural) {
    alignas(32) f32 block[BLOCK_PIXELS];

    // Load pixels and apply -128 level shift.
    for (i32 row = 0; row < BLOCK_SIZE; ++row) {
        const u8* srcRow = inputPixels + row * inputRowStride;
        f32* dst = block + row * BLOCK_SIZE;
        for (i32 col = 0; col < BLOCK_SIZE; ++col) {
            dst[col] = static_cast<f32>(srcRow[col]) - DC_LEVEL_SHIFT;
        }
    }

    // Row FDCT pass (contiguous rows).
    for (i32 row = 0; row < BLOCK_SIZE; ++row) {
        fdct_1d_inplace(block + row * BLOCK_SIZE);
    }

    // Transpose so columns become contiguous rows.
    transpose_8x8_inplace(block);

    // Column FDCT pass (now contiguous after transpose).
    for (i32 col = 0; col < BLOCK_SIZE; ++col) {
        fdct_1d_inplace(block + col * BLOCK_SIZE);
    }

    // Transpose back to natural (row-major) order.
    transpose_8x8_inplace(block);

    // Quantise and reorder to zig-zag.
    // The reciprocal table folds 0.125 normalisation, AAN scale compensation,
    // and quantisation into a single multiply per coefficient.
    // Inner loop reads block[] contiguously; writes scatter via NATURAL_TO_ZIGZAG.
    for (i32 nat = 0; nat < BLOCK_PIXELS; ++nat) {
        const f32 val = block[nat] * quantReciprocalNatural[nat];
        quantisedCoeffs[NATURAL_TO_ZIGZAG[nat]] = static_cast<i32>(val + std::copysignf(0.5f, val));
    }
}

// ============================================================================
// JPEG Category (Bit-Size) Encoding Helpers
// ============================================================================

/// Fused category + magnitude result.  Avoids computing abs/bit-width twice.
struct CategoryMagnitude {
    i32 category;
    u32 magnitude;
};

/// Compute category (bit-width of |value|) and magnitude bits in one pass.
/// For positive values the magnitude is the value itself.  For negative values
/// it is the one's-complement encoding (value + (1 << category) - 1).
CategoryMagnitude compute_category_magnitude(i32 value) {
    const i32 mask = value >> 31;                            // 0 or -1
    const u32 abs = static_cast<u32>((value ^ mask) - mask); // branchless abs
    const i32 cat = static_cast<i32>(std::bit_width(abs));
    const u32 mag = static_cast<u32>(value + (mask & ((1 << cat) - 1)));
    return {cat, mag};
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

/// Write a SOF (Start of Frame) marker segment.
/// @param sofMarker  MARKER_SOF0 for baseline, MARKER_SOF2 for progressive.
void write_sof(std::vector<u8>& out, u8 sofMarker, u32 width, u32 height, u32 componentCount) {
    write_marker(out, sofMarker);
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

/// Write a SOS (Start of Scan) marker segment.
/// @param componentIds  1-based component IDs to include in the scan.
/// @param dcTableIds    DC Huffman table index per component.
/// @param acTableIds    AC Huffman table index per component.
/// @param ss            Spectral selection start (0 for DC, >0 for AC).
/// @param se            Spectral selection end (0 for DC-only, 63 for full AC).
/// @param ah            Successive approximation high bit.
/// @param al            Successive approximation low bit.
void write_sos(std::vector<u8>& out, const u8* compIds, const u8* dcIds, const u8* acIds, u32 count,
               u8 ss, u8 se, u8 ah, u8 al) {
    write_marker(out, MARKER_SOS);
    write_u16_be(out, static_cast<u16>(6 + count * 2));
    write_u8(out, static_cast<u8>(count));
    for (u32 i = 0; i < count; ++i) {
        write_u8(out, compIds[i]);
        write_u8(out, static_cast<u8>((dcIds[i] << 4) | acIds[i]));
    }
    write_u8(out, ss);
    write_u8(out, se);
    write_u8(out, static_cast<u8>((ah << 4) | al));
}

/// Write a DRI (Define Restart Interval) marker segment.
void write_dri(std::vector<u8>& out, u16 restartInterval) {
    write_marker(out, MARKER_DRI);
    write_u16_be(out, 4); // Segment length: 2 (length) + 2 (interval).
    write_u16_be(out, restartInterval);
}

/// Write a restart marker (RST0 through RST7, cycling modulo 8).
void write_rst(std::vector<u8>& out, u32 index) {
    write_marker(out, static_cast<u8>(MARKER_RST0 + (index & 7)));
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
    const auto [cat, mag] = compute_category_magnitude(dcDifference);
    const auto& hcode = dcTable.codes[cat];
    writer.writeBits(hcode.code, hcode.length);
    if (cat > 0) {
        writer.writeBits(mag, cat);
    }
}

/// Encode the AC coefficients of an 8x8 block (run-length coding).
/// Scans from the end to find the last nonzero coefficient, then only
/// iterates up to that point — avoids counting trailing zeros for sparse blocks.
void encode_ac_coefficients(BitstreamWriter& writer, const HuffmanEncodeTable& acTable,
                            const std::array<i32, BLOCK_PIXELS>& coeffs) {
    // Find last nonzero AC coefficient (scan from the end).
    i32 lastNonzero = BLOCK_PIXELS - 1;
    while (lastNonzero > 0 && coeffs[lastNonzero] == 0)
        --lastNonzero;

    if (lastNonzero == 0) {
        // All AC coefficients are zero — emit EOB.
        const auto& eob = acTable.codes[0x00];
        writer.writeBits(eob.code, eob.length);
        return;
    }

    i32 consecutiveZeros = 0;
    for (i32 k = 1; k <= lastNonzero; ++k) {
        if (coeffs[k] == 0) {
            consecutiveZeros++;
            continue;
        }

        // Emit ZRL (zero run length of 16) symbols for runs longer than 15.
        while (consecutiveZeros > 15) {
            const auto& zrl = acTable.codes[0xF0];
            writer.writeBits(zrl.code, zrl.length);
            consecutiveZeros -= 16;
        }

        const auto [cat, mag] = compute_category_magnitude(coeffs[k]);
        const u8 rlSymbol = static_cast<u8>((consecutiveZeros << 4) | cat);
        const auto& hc = acTable.codes[rlSymbol];
        writer.writeBits(hc.code, hc.length);
        writer.writeBits(mag, cat);
        consecutiveZeros = 0;
    }

    // The loop stopped at lastNonzero; if lastNonzero < 63 there are trailing
    // zeros that require an EOB marker.
    if (lastNonzero < BLOCK_PIXELS - 1) {
        const auto& eob = acTable.codes[0x00];
        writer.writeBits(eob.code, eob.length);
    }
}

/// Return the Huffman table index for a given component (0 = luma, 1 = chroma).
u8 huffTableIndex(u32 compIndex, u32 componentCount) {
    return (componentCount > 1 && compIndex > 0) ? 1 : 0;
}

// ============================================================================
// Encoder State Machine
// ============================================================================

struct JpegEncoder {
    std::vector<u8> outputBuffer;

    std::array<u16, BLOCK_PIXELS> quantTable{};
    std::array<f32, BLOCK_PIXELS> quantReciprocal{};
    std::array<HuffmanEncodeTable, 2> dcHuffTables;
    std::array<HuffmanEncodeTable, 2> acHuffTables;

    u32 imageWidth = 0;
    u32 imageHeight = 0;
    u32 componentCount = 0;

    u32 mcuColumnsCount = 0;
    u32 mcuRowsCount = 0;

    std::string* errorOutput = nullptr;
    JpegContext* ctx = nullptr;

    // -- Helpers --

    bool reportError(const std::string& message) {
        if (errorOutput) {
            *errorOutput = message;
        }
        return false;
    }

    /// Validate input image and initialise common encoder state.
    bool initFromImage(const Image& image, i32 quality);

    /// Write all DHT (Define Huffman Table) markers to the output buffer.
    void writeAllDhtMarkers();

    // -- Encoding Steps --

    /// Prepare per-component sample buffers from the interleaved input image.
    /// Each component is extracted into a separate MCU-aligned buffer with
    /// edge pixels replicated to fill partial MCUs.
    void buildComponentBuffers(const Image& image,
                               std::array<std::vector<u8>, MAX_COMPONENTS>& buffers,
                               std::array<u32, MAX_COMPONENTS>& strides);

    /// Encode all MCUs from precomputed coefficient blocks (baseline path).
    /// Entropy coding is inherently serial.
    bool encodeScanDataFromCoeffs(
        const std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks);

    /// FDCT + quantise all blocks for all components into coefficient storage.
    void buildCoefficientBlocks(
        const std::array<std::vector<u8>, MAX_COMPONENTS>& buffers,
        const std::array<u32, MAX_COMPONENTS>& strides,
        std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks);

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
    /// @param ss      Spectral selection start.
    /// @param se      Spectral selection end.
    /// @param al      Successive approximation low bit (0 = no SA).
    /// @param output  Destination buffer for entropy-coded data.
    bool encodeProgressiveAcScan(const std::vector<std::array<i32, BLOCK_PIXELS>>& coeffBlocks,
                                 u32 compIndex, i32 ss, i32 se, i32 al, std::vector<u8>& output);

    /// Encode progressive AC refinement scan for one component.
    /// @param ss      Spectral selection start.
    /// @param se      Spectral selection end.
    /// @param al      The bit position to refine.
    /// @param output  Destination buffer for entropy-coded data.
    bool encodeProgressiveAcRefineScan(
        const std::vector<std::array<i32, BLOCK_PIXELS>>& coeffBlocks, u32 compIndex, i32 ss,
        i32 se, i32 al, std::vector<u8>& output);

    /// Top-level serial stream writers (called from within a DAG task).
    /// These write all JPEG markers + entropy-coded data to outputBuffer.
    void writeBaselineStream(
        const std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks);
    void writeProgressiveStream(
        const std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks);
};

/// Validate input image and initialise common encoder state (quant table,
/// Huffman tables, MCU grid, output buffer).
bool JpegEncoder::initFromImage(const Image& image, i32 quality) {
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
    quantReciprocal = build_quant_reciprocal_natural(quantTable);

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

    outputBuffer.clear();
    outputBuffer.reserve(static_cast<size_t>(imageWidth) * imageHeight * componentCount);

    mcuColumnsCount = (imageWidth + BLOCK_SIZE - 1) / BLOCK_SIZE;
    mcuRowsCount = (imageHeight + BLOCK_SIZE - 1) / BLOCK_SIZE;

    return true;
}

/// Write all DHT marker segments to the output buffer.
void JpegEncoder::writeAllDhtMarkers() {
    // Luminance tables (DC class 0 index 0, AC class 1 index 0).
    write_dht(outputBuffer, 0, 0, DC_LUMA_COUNTS.data(), DC_LUMA_SYMBOLS.data(),
              static_cast<i32>(DC_LUMA_SYMBOLS.size()));
    write_dht(outputBuffer, 1, 0, AC_LUMA_COUNTS.data(), AC_LUMA_SYMBOLS.data(),
              static_cast<i32>(AC_LUMA_SYMBOLS.size()));

    // Chrominance tables (DC class 0 index 1, AC class 1 index 1).
    if (componentCount > 1) {
        write_dht(outputBuffer, 0, 1, DC_CHROMA_COUNTS.data(), DC_CHROMA_SYMBOLS.data(),
                  static_cast<i32>(DC_CHROMA_SYMBOLS.size()));
        write_dht(outputBuffer, 1, 1, AC_CHROMA_COUNTS.data(), AC_CHROMA_SYMBOLS.data(),
                  static_cast<i32>(AC_CHROMA_SYMBOLS.size()));
    }
}

/// Extract each component from the interleaved image into a separate buffer
/// whose dimensions are rounded up to the next multiple of 8.  Edge pixels
/// are replicated to avoid boundary artefacts in the DCT.
void JpegEncoder::buildComponentBuffers(const Image& image,
                                        std::array<std::vector<u8>, MAX_COMPONENTS>& buffers,
                                        std::array<u32, MAX_COMPONENTS>& strides) {
    u32 paddedWidth = ((imageWidth + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;
    u32 paddedHeight = ((imageHeight + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;

    for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
        strides[componentIndex] = paddedWidth;
        buffers[componentIndex].resize(static_cast<size_t>(paddedWidth) * paddedHeight, 0);
    }

    // Each row-range writes to non-overlapping regions of each component buffer.
    const u32 compCnt = componentCount;
    const u32 imgW = imageWidth;
    const u32 imgH = imageHeight;
    const u8* srcPixels = image.pixels.data();

    parallel_for_blocks(
        paddedHeight, ctx,
        [&, paddedWidth, compCnt, imgW, imgH, srcPixels](u32 rowBegin, u32 rowEnd) {
            for (u32 destY = rowBegin; destY < rowEnd; destY++) {
                const u32 sourceY = std::min(destY, imgH - 1);
                const u8* srcRow = srcPixels + sourceY * imgW * compCnt;

                if (compCnt == 1) {
                    // Single component: memcpy valid region, memset pad.
                    u8* destRow = buffers[0].data() + destY * paddedWidth;
                    std::memcpy(destRow, srcRow, imgW);
                    if (paddedWidth > imgW) {
                        std::memset(destRow + imgW, destRow[imgW - 1], paddedWidth - imgW);
                    }
                } else if (compCnt == 4) {
                    // 4-component (BGRA): deinterleave all 4 in one pass.
                    u8* d0 = buffers[0].data() + destY * paddedWidth;
                    u8* d1 = buffers[1].data() + destY * paddedWidth;
                    u8* d2 = buffers[2].data() + destY * paddedWidth;
                    u8* d3 = buffers[3].data() + destY * paddedWidth;
                    for (u32 x = 0; x < imgW; ++x) {
                        const u32 srcOff = x * 4;
                        d0[x] = srcRow[srcOff];
                        d1[x] = srcRow[srcOff + 1];
                        d2[x] = srcRow[srcOff + 2];
                        d3[x] = srcRow[srcOff + 3];
                    }
                    if (paddedWidth > imgW) {
                        std::memset(d0 + imgW, d0[imgW - 1], paddedWidth - imgW);
                        std::memset(d1 + imgW, d1[imgW - 1], paddedWidth - imgW);
                        std::memset(d2 + imgW, d2[imgW - 1], paddedWidth - imgW);
                        std::memset(d3 + imgW, d3[imgW - 1], paddedWidth - imgW);
                    }
                } else {
                    for (u32 ci = 0; ci < compCnt; ++ci) {
                        u8* destRow = buffers[ci].data() + destY * paddedWidth;
                        // Valid region: extract component from interleaved source.
                        for (u32 x = 0; x < imgW; ++x) {
                            destRow[x] = srcRow[x * compCnt + ci];
                        }
                        // Pad region: replicate last column.
                        if (paddedWidth > imgW) {
                            std::memset(destRow + imgW, destRow[imgW - 1], paddedWidth - imgW);
                        }
                    }
                }
            }
        });
}

/// Encode all MCU blocks from precomputed coefficient blocks (baseline path).
/// Entropy coding is inherently serial — coefficients must be visited in the
/// same MCU-row, MCU-column, component order for DC differential coding.
bool JpegEncoder::encodeScanDataFromCoeffs(
    const std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks) {

    BitstreamWriter writer;
    writer.init(&outputBuffer);

    std::array<i32, MAX_COMPONENTS> dcPredictions{};
    u32 totalBlocks = mcuColumnsCount * mcuRowsCount;

    for (u32 blockIdx = 0; blockIdx < totalBlocks; blockIdx++) {
        for (u32 componentIndex = 0; componentIndex < componentCount; componentIndex++) {
            const auto& quantisedCoefficients = coeffBlocks[componentIndex][blockIdx];
            u8 tblIdx = huffTableIndex(componentIndex, componentCount);

            // DC: differential coding.
            i32 dcDifference = quantisedCoefficients[0] - dcPredictions[componentIndex];
            dcPredictions[componentIndex] = quantisedCoefficients[0];
            encode_dc_coefficient(writer, dcHuffTables[tblIdx], dcDifference);

            // AC: run-length coding.
            encode_ac_coefficients(writer, acHuffTables[tblIdx], quantisedCoefficients);
        }
    }

    writer.flushWithPadding();
    return true;
}

// ============================================================================
// Progressive Encoding Helpers
// ============================================================================

void JpegEncoder::buildCoefficientBlocks(
    const std::array<std::vector<u8>, MAX_COMPONENTS>& buffers,
    const std::array<u32, MAX_COMPONENTS>& strides,
    std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks) {

    u32 totalBlocks = mcuColumnsCount * mcuRowsCount;
    for (u32 c = 0; c < componentCount; c++) {
        coeffBlocks[c].resize(totalBlocks);
    }

    // Each block writes to its own coeffBlocks[c][blockIndex] — no overlap.
    const u32 mcuCols = mcuColumnsCount;
    const u32 compCnt = componentCount;

    const auto& qr = quantReciprocal;
    parallel_for_blocks(totalBlocks, ctx, [&, mcuCols, compCnt](u32 blockBegin, u32 blockEnd) {
        for (u32 blockIndex = blockBegin; blockIndex < blockEnd; blockIndex++) {
            const u32 mcuRow = blockIndex / mcuCols;
            const u32 mcuCol = blockIndex % mcuCols;
            const u32 blockPixelX = mcuCol * BLOCK_SIZE;
            const u32 blockPixelY = mcuRow * BLOCK_SIZE;
            for (u32 c = 0; c < compCnt; c++) {
                const u8* blockDataPointer =
                    buffers[c].data() + blockPixelY * strides[c] + blockPixelX;
                forward_dct_and_quantise(blockDataPointer, strides[c], coeffBlocks[c][blockIndex],
                                         qr);
            }
        }
    });
}

bool JpegEncoder::encodeProgressiveDcScan(
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

bool JpegEncoder::encodeProgressiveDcRefineScan(
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

bool JpegEncoder::encodeProgressiveAcScan(
    const std::vector<std::array<i32, BLOCK_PIXELS>>& coeffBlocks, u32 compIndex, i32 ss, i32 se,
    i32 al, std::vector<u8>& output) {

    u8 tblIdx = huffTableIndex(compIndex, componentCount);
    const auto& acTable = acHuffTables[tblIdx];

    BitstreamWriter writer;
    writer.init(&output);

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

        // Find last nonzero in [ss, se] after point transform (scan from end).
        i32 lastNz = ss - 1;
        for (i32 k = se; k >= ss; --k) {
            const i32 absVal = coeffs[k] < 0 ? -coeffs[k] : coeffs[k];
            if ((absVal >> al) != 0) {
                lastNz = k;
                break;
            }
        }

        if (lastNz < ss) {
            writeEob();
            continue;
        }

        i32 consecutiveZeros = 0;
        for (i32 k = ss; k <= lastNz; ++k) {
            // Point transform: truncation toward zero (ITU-T T.81 §G.1.2.2).
            const i32 absVal = coeffs[k] < 0 ? -coeffs[k] : coeffs[k];
            const i32 coeff = coeffs[k] < 0 ? -(absVal >> al) : (absVal >> al);
            if (coeff == 0) {
                consecutiveZeros++;
                continue;
            }

            while (consecutiveZeros > 15) {
                const auto& zrl = acTable.codes[0xF0];
                writer.writeBits(zrl.code, zrl.length);
                consecutiveZeros -= 16;
            }

            const auto [cat, mag] = compute_category_magnitude(coeff);
            const u8 rlSymbol = static_cast<u8>((consecutiveZeros << 4) | cat);
            const auto& hc = acTable.codes[rlSymbol];
            writer.writeBits(hc.code, hc.length);
            writer.writeBits(mag, cat);
            consecutiveZeros = 0;
        }

        // Trailing zeros past lastNz require EOB.
        if (lastNz < se) {
            writeEob();
        }
    }

    writer.flushWithPadding();
    return true;
}

bool JpegEncoder::encodeProgressiveAcRefineScan(
    const std::vector<std::array<i32, BLOCK_PIXELS>>& coeffBlocks, u32 compIndex, i32 ss, i32 se,
    i32 al, std::vector<u8>& output) {

    u8 tblIdx = huffTableIndex(compIndex, componentCount);
    const auto& acTable = acHuffTables[tblIdx];

    BitstreamWriter writer;
    writer.init(&output);

    // Stack-allocated correction bit buffer (max 63 AC positions per block).
    // Avoids heap allocation in the per-block loop.
    u32 corrBits[BLOCK_PIXELS];
    u32 corrCount = 0;

    auto flushCorrections = [&](BitstreamWriter& w) {
        for (u32 i = 0; i < corrCount; ++i) {
            w.writeBits(corrBits[i], 1);
        }
        corrCount = 0;
    };

    auto writeEobWithCorrections = [&]() {
        const auto& hcode = acTable.codes[0x00];
        writer.writeBits(hcode.code, hcode.length);
        flushCorrections(writer);
    };

    for (size_t blockIdx = 0; blockIdx < coeffBlocks.size(); blockIdx++) {
        const auto& coeffs = coeffBlocks[blockIdx];

        // Find the last position with a newly-nonzero coefficient.
        i32 lastNewNzPos = -1;
        for (i32 k = se; k >= ss; k--) {
            const i32 absCoeff = coeffs[k] < 0 ? -coeffs[k] : coeffs[k];
            const bool prevNonzero = (absCoeff >> (al + 1)) != 0;
            const bool newNonzero = !prevNonzero && ((absCoeff >> al) & 1) != 0;
            if (newNonzero) {
                lastNewNzPos = k;
                break;
            }
        }

        corrCount = 0;

        if (lastNewNzPos < 0) {
            // No new nonzero coefficients — emit EOB0 with correction bits.
            for (i32 k = ss; k <= se; k++) {
                const i32 absCoeff = coeffs[k] < 0 ? -coeffs[k] : coeffs[k];
                if ((absCoeff >> (al + 1)) != 0) {
                    corrBits[corrCount++] = (static_cast<u32>(absCoeff) >> al) & 1u;
                }
            }
            writeEobWithCorrections();
            continue;
        }

        // Encode AC coefficients with refinement.
        i32 zerosToSkip = 0;

        for (i32 k = ss; k <= se; k++) {
            const i32 absCoeff = coeffs[k] < 0 ? -coeffs[k] : coeffs[k];
            const i32 shifted = absCoeff >> al;

            if (shifted == 0) {
                zerosToSkip++;
                continue;
            }

            // Emit pending ZRL symbols before processing this coefficient.
            while (zerosToSkip > 15 && k <= lastNewNzPos) {
                const auto& zrlCode = acTable.codes[0xF0];
                writer.writeBits(zrlCode.code, zrlCode.length);
                flushCorrections(writer);
                zerosToSkip -= 16;
            }

            const bool prevNonzero = (shifted >> 1) != 0;

            if (prevNonzero) {
                corrBits[corrCount++] = static_cast<u32>(shifted) & 1u;
                continue;
            }

            // Newly-nonzero coefficient.
            const u8 rlSymbol = static_cast<u8>((zerosToSkip << 4) | 1);
            const auto& hc = acTable.codes[rlSymbol];
            writer.writeBits(hc.code, hc.length);

            const u32 signBit = (coeffs[k] >= 0) ? 1u : 0u;
            writer.writeBits(signBit, 1);

            flushCorrections(writer);
            zerosToSkip = 0;
        }

        // Trailing run with no more new nonzeros — emit EOB0 with corrections.
        if (zerosToSkip > 0 || corrCount > 0) {
            writeEobWithCorrections();
        }
    }

    writer.flushWithPadding();
    return true;
}

void JpegEncoder::writeProgressiveStream(
    const std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks) {

    // --- Successive approximation parameters ---
    // Al=1: first pass encodes top bits, one refinement pass fills bit 0.
    constexpr i32 SA_AL = 1;

    // --- Write marker segments ---

    write_soi(outputBuffer);
    write_dqt(outputBuffer, 0, quantTable);
    write_sof(outputBuffer, MARKER_SOF2, imageWidth, imageHeight, componentCount);
    writeAllDhtMarkers();

    // --- DC first pass: all components interleaved, Ss=0, Se=0, Ah=0, Al=SA_AL ---
    {
        u8 compIds[MAX_COMPONENTS], dcIds[MAX_COMPONENTS], acIds[MAX_COMPONENTS];
        for (u32 c = 0; c < componentCount; c++) {
            compIds[c] = static_cast<u8>(c + 1);
            dcIds[c] = huffTableIndex(c, componentCount);
            acIds[c] = 0;
        }
        write_sos(outputBuffer, compIds, dcIds, acIds, componentCount, 0, 0, 0, SA_AL);
        encodeProgressiveDcScan(coeffBlocks, SA_AL);
    }

    // --- AC first passes: per component, per band, Ah=0, Al=SA_AL ---
    for (u32 c = 0; c < componentCount; c++) {
        const u8 tblIdx = huffTableIndex(c, componentCount);
        const u8 compId = static_cast<u8>(c + 1);
        const u8 dcId = 0;
        for (const auto& band : DEFAULT_AC_BANDS) {
            write_sos(outputBuffer, &compId, &dcId, &tblIdx, 1, band.ss, band.se, 0, SA_AL);
            encodeProgressiveAcScan(coeffBlocks[c], c, band.ss, band.se, SA_AL, outputBuffer);
        }
    }

    // --- Refinement passes (only when SA_AL > 0) ---
    if (SA_AL > 0) {
        // DC refinement: all components, Ss=0, Se=0, Ah=SA_AL, Al=0
        {
            u8 compIds[MAX_COMPONENTS], dcIds[MAX_COMPONENTS], acIds[MAX_COMPONENTS];
            for (u32 c = 0; c < componentCount; c++) {
                compIds[c] = static_cast<u8>(c + 1);
                dcIds[c] = huffTableIndex(c, componentCount);
                acIds[c] = 0;
            }
            write_sos(outputBuffer, compIds, dcIds, acIds, componentCount, 0, 0, SA_AL, 0);
            encodeProgressiveDcRefineScan(coeffBlocks, 0);
        }

        // AC refinement: per component, per band, Ah=SA_AL, Al=0
        for (u32 c = 0; c < componentCount; c++) {
            const u8 tblIdx = huffTableIndex(c, componentCount);
            const u8 compId = static_cast<u8>(c + 1);
            const u8 dcId = 0;
            for (const auto& band : DEFAULT_AC_BANDS) {
                write_sos(outputBuffer, &compId, &dcId, &tblIdx, 1, band.ss, band.se, SA_AL, 0);
                encodeProgressiveAcRefineScan(coeffBlocks[c], c, band.ss, band.se, 0, outputBuffer);
            }
        }
    }

    write_eoi(outputBuffer);
}

void JpegEncoder::writeBaselineStream(
    const std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>& coeffBlocks) {

    write_soi(outputBuffer);
    write_dqt(outputBuffer, 0, quantTable);
    write_sof(outputBuffer, MARKER_SOF0, imageWidth, imageHeight, componentCount);
    writeAllDhtMarkers();

    // SOS: per-component table IDs (baseline: Ss=0, Se=63, Ah=0, Al=0).
    {
        u8 compIds[MAX_COMPONENTS], dcIds[MAX_COMPONENTS], acIds[MAX_COMPONENTS];
        for (u32 c = 0; c < componentCount; c++) {
            compIds[c] = static_cast<u8>(c + 1);
            const u8 tblIdx = huffTableIndex(c, componentCount);
            dcIds[c] = tblIdx;
            acIds[c] = tblIdx;
        }
        write_sos(outputBuffer, compIds, dcIds, acIds, componentCount, 0, 63, 0, 0);
    }

    encodeScanDataFromCoeffs(coeffBlocks);
    write_eoi(outputBuffer);
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

/// Coefficient block type alias for readability.
using CoeffBlocks = std::array<std::vector<std::array<i32, BLOCK_PIXELS>>, MAX_COMPONENTS>;

std::vector<u8> encode_raw(const Image& image, i32 quality, std::string* out_error,
                           bool progressive, JpegContext* ctx, std::vector<u8>* asyncOutput) {
    auto enc = std::make_shared<JpegEncoder>();
    enc->errorOutput = out_error;
    enc->ctx = ctx;

    if (!enc->initFromImage(image, quality)) {
        return {};
    }

    // --- DAG node 1: Build per-component sample buffers (parallel) ---
    auto compBufs = std::make_shared<std::array<std::vector<u8>, MAX_COMPONENTS>>();
    auto compStrides = std::make_shared<std::array<u32, MAX_COMPONENTS>>();
    enc->buildComponentBuffers(image, *compBufs, *compStrides);

    // --- DAG node 2: FDCT + quantise all blocks (parallel) ---
    auto coeffs = std::make_shared<CoeffBlocks>();
    enc->buildCoefficientBlocks(*compBufs, *compStrides, *coeffs);

    // --- Entropy encoding stages ---
    // Determine whether parallel entropy encoding is feasible.
    bool canParallel = ctx && ctx->pool && ctx->pool->threadCount() > 1;
    bool useParallelBaseline = canParallel && !progressive && enc->mcuRowsCount >= 2;
    bool useParallelProgressive = canParallel && progressive && enc->componentCount >= 2;

    if (useParallelBaseline) {
        // ---- Parallel baseline: restart-interval encoding via timeline ----

        const u32 nIntervals = enc->mcuRowsCount;

        // Stage 3: Write header markers (SOI, DQT, SOF0, DHT, DRI, SOS).
        submitSingleTask(ctx, [enc, compBufs]() {
            for (u32 c = 0; c < enc->componentCount; c++)
                (*compBufs)[c] = {};

            write_soi(enc->outputBuffer);
            write_dqt(enc->outputBuffer, 0, enc->quantTable);
            write_sof(enc->outputBuffer, MARKER_SOF0, enc->imageWidth, enc->imageHeight,
                      enc->componentCount);
            enc->writeAllDhtMarkers();
            write_dri(enc->outputBuffer, static_cast<u16>(enc->mcuColumnsCount));

            u8 compIds[MAX_COMPONENTS], dcIds[MAX_COMPONENTS], acIds[MAX_COMPONENTS];
            for (u32 c = 0; c < enc->componentCount; c++) {
                compIds[c] = static_cast<u8>(c + 1);
                const u8 tblIdx = huffTableIndex(c, enc->componentCount);
                dcIds[c] = tblIdx;
                acIds[c] = tblIdx;
            }
            write_sos(enc->outputBuffer, compIds, dcIds, acIds, enc->componentCount, 0, 63, 0, 0);
        });

        // Stage 4: Encode each MCU row interval in parallel (on timeline).
        auto intervalBufs = std::make_shared<std::vector<std::vector<u8>>>(nIntervals);
        parallel_for_blocks(nIntervals, ctx, [enc, coeffs, intervalBufs](u32 begin, u32 end) {
            for (u32 interval = begin; interval < end; ++interval) {
                BitstreamWriter writer;
                writer.init(&(*intervalBufs)[interval]);
                std::array<i32, MAX_COMPONENTS> dcPreds{};
                for (u32 mcuCol = 0; mcuCol < enc->mcuColumnsCount; ++mcuCol) {
                    u32 blockIdx = interval * enc->mcuColumnsCount + mcuCol;
                    for (u32 ci = 0; ci < enc->componentCount; ++ci) {
                        const auto& qc = (*coeffs)[ci][blockIdx];
                        u8 tblIdx = huffTableIndex(ci, enc->componentCount);
                        i32 dcDiff = qc[0] - dcPreds[ci];
                        dcPreds[ci] = qc[0];
                        encode_dc_coefficient(writer, enc->dcHuffTables[tblIdx], dcDiff);
                        encode_ac_coefficients(writer, enc->acHuffTables[tblIdx], qc);
                    }
                }
                writer.flushWithPadding();
            }
        });

        // Stage 5: Concatenate intervals with RST markers + write EOI.
        submitSingleTask(ctx, [enc, intervalBufs, nIntervals]() {
            enc->outputBuffer.insert(enc->outputBuffer.end(), (*intervalBufs)[0].begin(),
                                     (*intervalBufs)[0].end());
            for (u32 i = 1; i < nIntervals; ++i) {
                write_rst(enc->outputBuffer, i - 1);
                enc->outputBuffer.insert(enc->outputBuffer.end(), (*intervalBufs)[i].begin(),
                                         (*intervalBufs)[i].end());
            }
            write_eoi(enc->outputBuffer);
        });

    } else if (useParallelProgressive) {
        // ---- Parallel progressive: AC scans via timeline ----

        constexpr i32 SA_AL = 1;
        const u32 numBands = static_cast<u32>(DEFAULT_AC_BANDS.size());
        const u32 totalAcScans = enc->componentCount * numBands;

        // Stage 3: Write markers + DC first pass.
        submitSingleTask(ctx, [enc, compBufs, coeffs]() {
            for (u32 c = 0; c < enc->componentCount; c++)
                (*compBufs)[c] = {};

            write_soi(enc->outputBuffer);
            write_dqt(enc->outputBuffer, 0, enc->quantTable);
            write_sof(enc->outputBuffer, MARKER_SOF2, enc->imageWidth, enc->imageHeight,
                      enc->componentCount);
            enc->writeAllDhtMarkers();

            // DC first pass (serial, interleaved).
            {
                u8 compIds[MAX_COMPONENTS], dcIds[MAX_COMPONENTS], acIds[MAX_COMPONENTS];
                for (u32 c = 0; c < enc->componentCount; c++) {
                    compIds[c] = static_cast<u8>(c + 1);
                    dcIds[c] = huffTableIndex(c, enc->componentCount);
                    acIds[c] = 0;
                }
                write_sos(enc->outputBuffer, compIds, dcIds, acIds, enc->componentCount, 0, 0, 0,
                          SA_AL);
                enc->encodeProgressiveDcScan(*coeffs, SA_AL);
            }
        });

        // Stage 4: Encode AC first-pass scans in parallel (on timeline).
        auto acFirstBufs = std::make_shared<std::vector<std::vector<u8>>>(totalAcScans);
        parallel_for_tasks(totalAcScans, ctx, [enc, coeffs, acFirstBufs, numBands](u32 scanIdx) {
            constexpr i32 al = 1; // SA_AL
            u32 c = scanIdx / numBands;
            u32 b = scanIdx % numBands;
            const auto& band = DEFAULT_AC_BANDS[b];
            enc->encodeProgressiveAcScan((*coeffs)[c], c, band.ss, band.se, al,
                                         (*acFirstBufs)[scanIdx]);
        });

        // Stage 5: Assemble AC first-pass + DC refinement.
        submitSingleTask(ctx, [enc, coeffs, acFirstBufs, numBands]() {
            constexpr i32 SA_AL = 1;
            // Assemble AC first-pass scan data with SOS markers.
            for (u32 c = 0; c < enc->componentCount; ++c) {
                u8 tblIdx = huffTableIndex(c, enc->componentCount);
                for (u32 b = 0; b < numBands; ++b) {
                    const auto& band = DEFAULT_AC_BANDS[b];
                    const u8 compId = static_cast<u8>(c + 1);
                    const u8 dcId = 0;
                    write_sos(enc->outputBuffer, &compId, &dcId, &tblIdx, 1, band.ss, band.se, 0,
                              SA_AL);
                    enc->outputBuffer.insert(enc->outputBuffer.end(),
                                             (*acFirstBufs)[c * numBands + b].begin(),
                                             (*acFirstBufs)[c * numBands + b].end());
                }
            }

            // DC refinement (serial).
            {
                u8 compIds[MAX_COMPONENTS], dcIds[MAX_COMPONENTS], acIds[MAX_COMPONENTS];
                for (u32 c = 0; c < enc->componentCount; c++) {
                    compIds[c] = static_cast<u8>(c + 1);
                    dcIds[c] = huffTableIndex(c, enc->componentCount);
                    acIds[c] = 0;
                }
                write_sos(enc->outputBuffer, compIds, dcIds, acIds, enc->componentCount, 0, 0,
                          SA_AL, 0);
                enc->encodeProgressiveDcRefineScan(*coeffs, 0);
            }
        });

        // Stage 6: Encode AC refinement scans in parallel (on timeline).
        auto acRefineBufs = std::make_shared<std::vector<std::vector<u8>>>(totalAcScans);
        parallel_for_tasks(totalAcScans, ctx, [enc, coeffs, acRefineBufs, numBands](u32 scanIdx) {
            u32 c = scanIdx / numBands;
            u32 b = scanIdx % numBands;
            const auto& band = DEFAULT_AC_BANDS[b];
            enc->encodeProgressiveAcRefineScan((*coeffs)[c], c, band.ss, band.se, 0,
                                               (*acRefineBufs)[scanIdx]);
        });

        // Stage 7: Assemble AC refinement + EOI.
        submitSingleTask(ctx, [enc, acRefineBufs, numBands]() {
            constexpr i32 SA_AL = 1;
            for (u32 c = 0; c < enc->componentCount; ++c) {
                u8 tblIdx = huffTableIndex(c, enc->componentCount);
                for (u32 b = 0; b < numBands; ++b) {
                    const auto& band = DEFAULT_AC_BANDS[b];
                    const u8 compId = static_cast<u8>(c + 1);
                    const u8 dcId = 0;
                    write_sos(enc->outputBuffer, &compId, &dcId, &tblIdx, 1, band.ss, band.se,
                              SA_AL, 0);
                    enc->outputBuffer.insert(enc->outputBuffer.end(),
                                             (*acRefineBufs)[c * numBands + b].begin(),
                                             (*acRefineBufs)[c * numBands + b].end());
                }
            }
            write_eoi(enc->outputBuffer);
        });

    } else {
        // ---- Serial path (no pool or insufficient parallelism) ----
        submitSingleTask(ctx, [enc, compBufs, compStrides, coeffs, progressive]() {
            for (u32 c = 0; c < enc->componentCount; c++) {
                (*compBufs)[c] = {};
            }
            if (progressive) {
                enc->writeProgressiveStream(*coeffs);
            } else {
                enc->writeBaselineStream(*coeffs);
            }
        });
    }

    // --- Optional output transfer for async callers ---
    if (asyncOutput && ctx && ctx->sem) {
        submitSingleTask(ctx,
                         [enc, asyncOutput]() { *asyncOutput = std::move(enc->outputBuffer); });
        return {};
    }

    return std::move(enc->outputBuffer);
}

} // namespace whiteout::textures::jpeg
