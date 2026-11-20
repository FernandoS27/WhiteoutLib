// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "adpcm.h"

#include "../../../common/output_reserve.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace whiteout::storages::mpq {

namespace {

// IMA ADPCM step index adjustment table (indexed by encoded sample & 0x1F).
static constexpr std::array<int, 32> kNextStepTable = {{
    -1, 0, -1, 4, -1, 2, -1, 6, -1, 1, -1, 5, -1, 3, -1, 7,
    -1, 1, -1, 5, -1, 3, -1, 7, -1, 2, -1, 4, -1, 6, -1, 8,
}};

// IMA ADPCM step size table (89 entries, values 7..32767).
static constexpr std::array<int, 89> kStepSizeTable = {{
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
}};

static constexpr int kInitialStepIndex = 0x2C;
static constexpr int kMaxStepIndex = 88;

inline short getNextStepIndex(int stepIndex, unsigned int encodedSample) {
    stepIndex += kNextStepTable[encodedSample & 0x1F];
    return static_cast<short>(std::clamp(stepIndex, 0, kMaxStepIndex));
}

inline int updatePredictedSample(int predicted, int encoded, int difference, int signBit = 0x40) {
    if (encoded & signBit) {
        predicted -= difference;
        if (predicted <= -32768)
            predicted = -32768;
    } else {
        predicted += difference;
        if (predicted >= 32767)
            predicted = 32767;
    }
    return predicted;
}

inline int decodeSample(int predicted, int encoded, int stepSize, int difference) {
    if (encoded & 0x01)
        difference += (stepSize >> 0);
    if (encoded & 0x02)
        difference += (stepSize >> 1);
    if (encoded & 0x04)
        difference += (stepSize >> 2);
    if (encoded & 0x08)
        difference += (stepSize >> 3);
    if (encoded & 0x10)
        difference += (stepSize >> 4);
    if (encoded & 0x20)
        difference += (stepSize >> 5);
    return updatePredictedSample(predicted, encoded, difference);
}

/// What the decoder would otherwise work out per sample, per step index:
/// the part of the difference the six low bits of an encoded sample select
/// (decodeSample's six conditional adds), and the step index that follows
/// the five low bits (getNextStepIndex).
struct DecodeTables {
    std::array<std::array<u16, 64>, kMaxStepIndex + 1> difference{};
    std::array<std::array<u8, 32>, kMaxStepIndex + 1> nextStepIndex{};
};
constexpr DecodeTables buildDecodeTables() {
    DecodeTables tables;
    for (int index = 0; index <= kMaxStepIndex; ++index) {
        int const stepSize = kStepSizeTable[static_cast<size_t>(index)];
        for (int encoded = 0; encoded < 64; ++encoded) {
            int difference = 0;
            for (int bit = 0; bit < 6; ++bit)
                if (encoded & (1 << bit))
                    difference += stepSize >> bit;
            tables.difference[static_cast<size_t>(index)][static_cast<size_t>(encoded)] = static_cast<u16>(difference);
        }
        for (int encoded = 0; encoded < 32; ++encoded) {
            int const next = index + kNextStepTable[static_cast<size_t>(encoded)];
            tables.nextStepIndex[static_cast<size_t>(index)][static_cast<size_t>(encoded)] =
                static_cast<u8>(next < 0 ? 0 : (next > kMaxStepIndex ? kMaxStepIndex : next));
        }
    }
    return tables;
}
constexpr DecodeTables kDecodeTables = buildDecodeTables();

} // anonymous namespace

size_t adpcmDecompressInto(std::span<const u8> src, std::span<u8> dst, int channelCount) {
    if (channelCount < 1 || channelCount > 2 || src.size() < 4)
        return 0;

    const u8* in = src.data();
    const u8* const inEnd = in + src.size();
    u8* out = dst.data();
    u8* const outEnd = out + (dst.size() & ~size_t{1}); // Whole 16-bit samples only.

    int predicted[2] = {0, 0};
    int stepIndex[2] = {kInitialStepIndex, kInitialStepIndex};

    // First byte is always 0, second is the bit shift (compression level - 1).
    // A shift count wraps at the word, as the game's own shift does.
    ++in;
    u32 const bitShift = *in++ & 31u;

    // Each channel starts from a sample stored whole.
    for (int i = 0; i < channelCount; ++i) {
        if (inEnd - in < 2 || out >= outEnd)
            return static_cast<size_t>(out - dst.data());
        predicted[i] = static_cast<short>(in[0] | (static_cast<u16>(in[1]) << 8));
        out[0] = in[0];
        out[1] = in[1];
        in += 2;
        out += 2;
    }

    int const flip = channelCount - 1; // Stereo alternates channels; mono stays on 0.
    int channel = flip;
    while (in < inEnd && out < outEnd) {
        u32 const encoded = *in++;
        channel ^= flip;

        if (encoded == 0x80) {
            // Step index down; the predicted sample is written again.
            if (stepIndex[channel] != 0)
                --stepIndex[channel];
        } else if (encoded == 0x81) {
            // Step index up, no sample: the next byte is for this channel too.
            stepIndex[channel] = std::min(stepIndex[channel] + 8, kMaxStepIndex);
            channel ^= flip;
            continue;
        } else {
            // Bit 6 is the sign; the sample saturates at 16 bits either way.
            size_t const index = static_cast<size_t>(stepIndex[channel]);
            int const difference = (kStepSizeTable[index] >> bitShift) + kDecodeTables.difference[index][encoded & 0x3F];
            int const sample = predicted[channel] + ((encoded & 0x40) ? -difference : difference);
            predicted[channel] = std::clamp(sample, -32768, 32767);
            stepIndex[channel] = kDecodeTables.nextStepIndex[index][encoded & 0x1F];
        }

        short const sample = static_cast<short>(predicted[channel]);
        out[0] = static_cast<u8>(sample & 0xFF);
        out[1] = static_cast<u8>((sample >> 8) & 0xFF);
        out += 2;
    }
    return static_cast<size_t>(out - dst.data());
}

std::vector<u8> adpcmDecompress(std::span<const u8> src, size_t expectedSize, int channelCount) {
    return whiteout::common::decodeGrowing(
        expectedSize, src.size(), [&](std::span<u8> dst) { return adpcmDecompressInto(src, dst, channelCount); });
}

// ============================================================================
// ADPCM Compression
// ============================================================================

std::vector<u8> adpcmCompress(std::span<const u8> src, int channelCount) {
    if (channelCount < 1 || channelCount > 2)
        return {};
    // Need at least one 16-bit sample per channel.
    if (src.size() < static_cast<size_t>(channelCount * 2))
        return {};
    // Must be an even number of bytes (16-bit samples).
    if (src.size() % 2 != 0)
        return {};

    const size_t sampleCount = src.size() / 2;

    // Read all 16-bit LE samples.
    std::vector<short> samples(sampleCount);
    for (size_t i = 0; i < sampleCount; i++) {
        samples[i] = static_cast<short>(static_cast<u16>(src[i * 2]) |
                                        (static_cast<u16>(src[i * 2 + 1]) << 8));
    }

    std::vector<u8> out;
    out.reserve(src.size());

    constexpr u8 kBitShift = 1; // compression level

    // Header: 0x00 byte + bitShift byte.
    out.push_back(0x00);
    out.push_back(kBitShift);

    std::array<short, 2> predictedSamples = {0, 0};
    std::array<short, 2> stepIndexes = {kInitialStepIndex, kInitialStepIndex};

    // Write initial sample for each channel.
    size_t samplePos = 0;
    for (int i = 0; i < channelCount; i++) {
        short const initialSample = samples[samplePos++];
        predictedSamples[i] = initialSample;
        out.push_back(static_cast<u8>(initialSample & 0xFF));
        out.push_back(static_cast<u8>((initialSample >> 8) & 0xFF));
    }

    int channelIndex = channelCount - 1;

    while (samplePos < sampleCount) {
        channelIndex = (channelIndex + 1) % channelCount;

        short const targetSample = samples[samplePos++];
        int const predicted = predictedSamples[channelIndex];
        int const stepIndex = stepIndexes[channelIndex];
        int const stepSize = kStepSizeTable[stepIndex];

        int difference = targetSample - predicted;
        bool const negative = (difference < 0);
        if (negative)
            difference = -difference;

        // Encode the absolute difference into 6 bits (bits 0-5 = magnitude, bit 6 = sign).
        // The decoder reconstructs: delta = sum of (stepSize >> k) for set bits + (stepSize >>
        // bitShift). We greedily pick bits from MSB to LSB of the magnitude portion.
        int encoded = 0;
        int const baseStep = stepSize >> kBitShift;
        int totalDelta = baseStep;

        if (difference >= totalDelta + (stepSize >> 0)) {
            encoded |= 0x01;
            totalDelta += (stepSize >> 0);
        }
        if (difference >= totalDelta + (stepSize >> 1)) {
            encoded |= 0x02;
            totalDelta += (stepSize >> 1);
        }
        if (difference >= totalDelta + (stepSize >> 2)) {
            encoded |= 0x04;
            totalDelta += (stepSize >> 2);
        }
        if (difference >= totalDelta + (stepSize >> 3)) {
            encoded |= 0x08;
            totalDelta += (stepSize >> 3);
        }
        if (difference >= totalDelta + (stepSize >> 4)) {
            encoded |= 0x10;
            totalDelta += (stepSize >> 4);
        }
        if (difference >= totalDelta + (stepSize >> 5)) {
            encoded |= 0x20;
            totalDelta += (stepSize >> 5);
        }

        if (negative)
            encoded |= 0x40;

        // Verify encoded byte doesn't collide with special markers.
        // 0x80 = step index decrement, 0x81 = step index increase.
        // The greedy encoding above produces values 0x00..0x7F, so no collision with 0x80/0x81.

        // Reconstruct exactly as decoder does to keep predictor in sync.
        predictedSamples[channelIndex] =
            static_cast<short>(decodeSample(predicted, encoded, stepSize, stepSize >> kBitShift));

        stepIndexes[channelIndex] = getNextStepIndex(stepIndex, encoded);

        out.push_back(static_cast<u8>(encoded));
    }

    return out;
}

} // namespace whiteout::storages::mpq
