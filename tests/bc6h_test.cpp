// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// The BC6H mode table is the part of the decoder that is easy to get silently
// wrong: a block header is a permutation of endpoint-field bits, and a layout
// that reads one block bit twice still decodes "something".  The bijection test
// below is what pins it down.

#include <catch2/catch_all.hpp>

#include "../src/whiteout/textures/bcn/bc6h.h"
#include "../src/whiteout/textures/bcn/bc6h_tables.h"

#include <map>
#include <set>
#include <vector>

using namespace whiteout;
using namespace whiteout::textures;

TEST_CASE("BC6H: every mode layout is a bijection over its header bits", "[bcn][bc6h]") {
    for (u32 mode_id = 1; mode_id <= 14; ++mode_id) {
        INFO("mode " << mode_id);
        const BC6HModeDesc& mode = BC6H_MODES[mode_id];

        REQUIRE(mode.layout != nullptr);
        CHECK(mode.header_bits == (mode.subsets == 2 ? 82u : 65u));

        // Each (field, bit) pair must be produced by exactly one block bit.
        std::map<std::pair<u8, u8>, u32> seen;
        for (u32 i = 0; i < mode.header_bits; ++i) {
            const auto key = std::make_pair(mode.layout[i].field, mode.layout[i].bit);
            INFO("block bit " << i << " field " << int(key.first) << " bit " << int(key.second));
            CHECK(seen.find(key) == seen.end());
            seen[key] = i;
        }
        CHECK(seen.size() == mode.header_bits);

        // Field widths must match the mode's declared precision, and each
        // field's bits must run 0..n-1 with no gaps.
        std::map<u8, std::set<u8>> bits_of;
        for (u32 i = 0; i < mode.header_bits; ++i)
            bits_of[mode.layout[i].field].insert(mode.layout[i].bit);

        auto width = [&](u8 field) { return bits_of.count(field) ? bits_of[field].size() : 0u; };

        for (u8 channel = 0; channel < 3; ++channel) {
            const u8 base = static_cast<u8>(channel * 4);
            INFO("channel " << int(channel));
            CHECK(width(base) == mode.endpoint_bits);                     // W
            CHECK(width(base + 1) == mode.delta_bits[channel]);           // X
            if (mode.subsets == 2) {
                CHECK(width(base + 2) == mode.delta_bits[channel]);       // Y
                CHECK(width(base + 3) == mode.delta_bits[channel]);       // Z
            } else {
                CHECK(width(base + 2) == 0);
                CHECK(width(base + 3) == 0);
            }
        }
        CHECK(width(BC6H_M) == (mode_id <= 2 ? 2u : 5u));
        CHECK(width(BC6H_D) == (mode.subsets == 2 ? 5u : 0u));

        for (const auto& [field, bits] : bits_of) {
            INFO("field " << int(field));
            u8 expected = 0;
            for (u8 b : bits)
                CHECK(b == expected++);
        }
    }
}

namespace {

/// Pack field values into a block using a mode's layout — the inverse of what
/// the decoder does, so a decode of the result must return the same values.
std::vector<u8> packBlock(u32 mode_id, const std::map<std::pair<u8, u8>, u32>& field_bits,
                          u64 indices_lo) {
    const BC6HModeDesc& mode = BC6H_MODES[mode_id];
    std::vector<u8> block(16, 0);
    for (u32 i = 0; i < mode.header_bits; ++i) {
        const auto key = std::make_pair(mode.layout[i].field, mode.layout[i].bit);
        const auto it = field_bits.find(key);
        if (it != field_bits.end() && it->second)
            block[i >> 3] |= static_cast<u8>(1u << (i & 7));
    }
    for (u32 i = 0; mode.header_bits + i < 128 && i < 64; ++i) {
        if ((indices_lo >> i) & 1ull) {
            const u32 pos = mode.header_bits + i;
            block[pos >> 3] |= static_cast<u8>(1u << (pos & 7));
        }
    }
    return block;
}

/// Set every bit of one field to `value`'s bits.
void putField(std::map<std::pair<u8, u8>, u32>& out, u8 field, u32 width, u32 value) {
    for (u32 b = 0; b < width; ++b)
        out[{field, static_cast<u8>(b)}] = (value >> b) & 1u;
}

} // namespace

TEST_CASE("BC6H: a flat block decodes to its endpoint colour in every mode", "[bcn][bc6h]") {
    // With both endpoints of a subset equal, every index interpolates to the
    // same value, so the whole 4x4 block must come out constant — whatever the
    // mode's bit scatter is.  A layout that misplaces a bit breaks this.
    for (u32 mode_id = 1; mode_id <= 14; ++mode_id) {
        INFO("mode " << mode_id);
        const BC6HModeDesc& mode = BC6H_MODES[mode_id];

        std::map<std::pair<u8, u8>, u32> fields;
        // Mode bits.
        const u32 mode_code = (mode_id == 1) ? 0u : (mode_id == 2) ? 1u : [&] {
            static const u32 codes[15] = {0,      0,      1,      0b00010, 0b00110, 0b01010,
                                          0b01110, 0b10010, 0b10110, 0b11010, 0b11110, 0b00011,
                                          0b00111, 0b01011, 0b01111};
            return codes[mode_id];
        }();
        putField(fields, BC6H_M, mode_id <= 2 ? 2 : 5, mode_code);

        // Base endpoint = 0, all deltas = 0 -> both endpoints identical.
        for (u8 channel = 0; channel < 3; ++channel) {
            const u8 base = static_cast<u8>(channel * 4);
            putField(fields, base, mode.endpoint_bits, 0);
            for (u8 e = 1; e < (mode.subsets == 2 ? 4 : 2); ++e)
                putField(fields, static_cast<u8>(base + e), mode.delta_bits[channel], 0);
        }

        const auto block = packBlock(mode_id, fields, 0);
        const auto pixels = bc6h::decodeBlocks(block, 4, 4, false);
        REQUIRE(pixels.size() == 4 * 4 * 4);
        for (u32 i = 0; i < 16; ++i) {
            CHECK(pixels[i * 4 + 0] == Catch::Approx(pixels[0]));
            CHECK(pixels[i * 4 + 1] == Catch::Approx(pixels[1]));
            CHECK(pixels[i * 4 + 2] == Catch::Approx(pixels[2]));
            CHECK(pixels[i * 4 + 3] == Catch::Approx(1.0f));
        }
    }
}

TEST_CASE("BC6H: unsigned decode spans the half-float range", "[bcn][bc6h]") {
    // Mode 11, 1 subset, 10-bit endpoints, no transform.  The maximum endpoint
    // must decode near the top of the half-float range (~65504), not to ~1.5 —
    // that was the symptom of the old unquantize scale.
    std::map<std::pair<u8, u8>, u32> fields;
    putField(fields, BC6H_M, 5, 0b00011);
    for (u8 channel = 0; channel < 3; ++channel) {
        putField(fields, static_cast<u8>(channel * 4), 10, 1023);     // W
        putField(fields, static_cast<u8>(channel * 4 + 1), 10, 1023); // X
    }
    const auto block = packBlock(11, fields, 0);
    const auto pixels = bc6h::decodeBlocks(block, 4, 4, false);
    REQUIRE(pixels.size() == 4 * 4 * 4);
    CHECK(pixels[0] > 60000.0f);
    CHECK(pixels[0] < 66000.0f);
}

TEST_CASE("BC6H: signed decode produces negative values", "[bcn][bc6h]") {
    // Mode 11 with the base endpoint's sign bit set: signed decoding must give
    // a negative result where unsigned decoding gives a large positive one.
    std::map<std::pair<u8, u8>, u32> fields;
    putField(fields, BC6H_M, 5, 0b00011);
    for (u8 channel = 0; channel < 3; ++channel) {
        putField(fields, static_cast<u8>(channel * 4), 10, 0x200);     // W = -512 signed
        putField(fields, static_cast<u8>(channel * 4 + 1), 10, 0x200); // X = -512 signed
    }
    const auto block = packBlock(11, fields, 0);

    const auto signed_pixels = bc6h::decodeBlocks(block, 4, 4, true);
    const auto unsigned_pixels = bc6h::decodeBlocks(block, 4, 4, false);
    REQUIRE(signed_pixels.size() == 4 * 4 * 4);
    REQUIRE(unsigned_pixels.size() == 4 * 4 * 4);

    CHECK(signed_pixels[0] < 0.0f);
    CHECK(unsigned_pixels[0] > 0.0f);
    CHECK(signed_pixels[3] == Catch::Approx(1.0f)); // alpha stays opaque
}

TEST_CASE("BC6H: encode/decode round-trips a smooth HDR image", "[bcn][bc6h]") {
    constexpr u32 kWidth = 16;
    constexpr u32 kHeight = 16;

    Texture source = Texture::create2D(PixelFormat::RGBA32F, kWidth, kHeight, 1);
    auto mip = source.mipData(0, 0);
    auto* texels = reinterpret_cast<f32*>(mip.data());
    // The encoder emits mode 11: one subset, so a block's colours must lie on a
    // line to be representable.  A ramp with fixed channel ratios is the honest
    // thing to measure round-trip fidelity against.
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
            const f32 t = static_cast<f32>(x + y + 4) * 8.0f;
            texels[i + 0] = t;
            texels[i + 1] = t * 0.5f;
            texels[i + 2] = t * 0.25f;
            texels[i + 3] = 1.0f;
        }
    }

    std::string error;
    auto encoded = bc6h::encodeTexture(source, &error);
    INFO("encode error: " << error);
    REQUIRE(encoded.has_value());

    auto decoded = bc6h::decodeTexture(*encoded, &error);
    INFO("decode error: " << error);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->format() == PixelFormat::RGBA32F);

    const auto* out = reinterpret_cast<const f32*>(decoded->mipData(0, 0).data());
    f64 total_error = 0.0;
    f64 total_magnitude = 0.0;
    for (u32 i = 0; i < kWidth * kHeight; ++i) {
        for (u32 c = 0; c < 3; ++c) {
            total_error += std::abs(static_cast<f64>(out[i * 4 + c]) - texels[i * 4 + c]);
            total_magnitude += texels[i * 4 + c];
        }
    }
    // Residual here is index quantisation: 16 levels spread linearly across the
    // block's range in half-float bit-pattern space.  Before the mode-code and
    // unquantize fixes this ratio was ~1.0.
    CHECK(total_error / total_magnitude < 0.01);
}

TEST_CASE("BC6H: a constant colour survives encode/decode", "[bcn][bc6h]") {
    // Every index maps to the same colour, so only endpoint quantisation applies.
    constexpr f32 kRed = 12.0f;
    constexpr f32 kGreen = 3.5f;
    constexpr f32 kBlue = 240.0f;

    Texture source = Texture::create2D(PixelFormat::RGBA32F, 8, 8, 1);
    auto* texels = reinterpret_cast<f32*>(source.mipData(0, 0).data());
    for (u32 i = 0; i < 8 * 8; ++i) {
        texels[i * 4 + 0] = kRed;
        texels[i * 4 + 1] = kGreen;
        texels[i * 4 + 2] = kBlue;
        texels[i * 4 + 3] = 1.0f;
    }

    auto encoded = bc6h::encodeTexture(source);
    REQUIRE(encoded.has_value());
    auto decoded = bc6h::decodeTexture(*encoded);
    REQUIRE(decoded.has_value());

    const auto* out = reinterpret_cast<const f32*>(decoded->mipData(0, 0).data());
    for (u32 i = 0; i < 8 * 8; ++i) {
        CHECK(out[i * 4 + 0] == Catch::Approx(kRed).epsilon(0.01));
        CHECK(out[i * 4 + 1] == Catch::Approx(kGreen).epsilon(0.01));
        CHECK(out[i * 4 + 2] == Catch::Approx(kBlue).epsilon(0.01));
    }
}
