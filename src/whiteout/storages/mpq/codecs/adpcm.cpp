// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "adpcm.h"

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

// Simple stream helpers to match StormLib's TADPCMStream pattern.
struct AdpcmStream {
    const u8* data;
    size_t size;
    size_t pos = 0;

    AdpcmStream(const u8* d, size_t s) : data(d), size(s) {}

    bool readByte(u8& out) {
        if (pos >= size)
            return false;
        out = data[pos++];
        return true;
    }

    bool readWord(short& out) {
        if (pos + 2 > size)
            return false;
        out = static_cast<short>(data[pos] | (static_cast<u16>(data[pos + 1]) << 8));
        pos += 2;
        return true;
    }
};

} // anonymous namespace

std::vector<u8> adpcmDecompress(std::span<const u8> src, size_t expectedSize, int channelCount) {
    if (channelCount < 1 || channelCount > 2)
        return {};
    if (src.size() < 4)
        return {};

    std::vector<u8> out;
    out.reserve(expectedSize);

    AdpcmStream is(src.data(), src.size());

    std::array<short, 2> predictedSamples = {0, 0};
    std::array<short, 2> stepIndexes = {kInitialStepIndex, kInitialStepIndex};

    // First byte is always 0, second is BitShift (compression level - 1).
    u8 dummy, bitShift;
    is.readByte(dummy);
    if (!is.readByte(bitShift))
        return {};

    // Read initial sample for each channel.
    for (int i = 0; i < channelCount; i++) {
        short initialSample;
        if (!is.readWord(initialSample))
            return out;
        predictedSamples[i] = initialSample;
        // Write initial sample to output (little-endian 16-bit).
        u8 lo = static_cast<u8>(initialSample & 0xFF);
        u8 hi = static_cast<u8>((initialSample >> 8) & 0xFF);
        out.push_back(lo);
        out.push_back(hi);
    }

    int channelIndex = channelCount - 1;

    u8 encodedSample;
    while (is.readByte(encodedSample)) {
        // Flip channel index.
        channelIndex = (channelIndex + 1) % channelCount;

        if (encodedSample == 0x80) {
            // Step index decrement marker — output the predicted sample unchanged.
            if (stepIndexes[channelIndex] != 0)
                stepIndexes[channelIndex]--;

            short sample = predictedSamples[channelIndex];
            out.push_back(static_cast<u8>(sample & 0xFF));
            out.push_back(static_cast<u8>((sample >> 8) & 0xFF));
        } else if (encodedSample == 0x81) {
            // Step index increase marker.
            stepIndexes[channelIndex] += 8;
            if (stepIndexes[channelIndex] > kMaxStepIndex)
                stepIndexes[channelIndex] = kMaxStepIndex;

            // Stay on the same channel for the next pass.
            channelIndex = (channelIndex + 1) % channelCount;
        } else {
            int stepIndex = stepIndexes[channelIndex];
            int stepSize = kStepSizeTable[stepIndex];

            predictedSamples[channelIndex] = static_cast<short>(decodeSample(
                predictedSamples[channelIndex], encodedSample, stepSize, stepSize >> bitShift));

            short sample = predictedSamples[channelIndex];
            out.push_back(static_cast<u8>(sample & 0xFF));
            out.push_back(static_cast<u8>((sample >> 8) & 0xFF));

            stepIndexes[channelIndex] = getNextStepIndex(stepIndex, encodedSample);
        }

        if (out.size() >= expectedSize)
            break;
    }

    return out;
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
        short initialSample = samples[samplePos++];
        predictedSamples[i] = initialSample;
        out.push_back(static_cast<u8>(initialSample & 0xFF));
        out.push_back(static_cast<u8>((initialSample >> 8) & 0xFF));
    }

    int channelIndex = channelCount - 1;

    while (samplePos < sampleCount) {
        channelIndex = (channelIndex + 1) % channelCount;

        short targetSample = samples[samplePos++];
        int predicted = predictedSamples[channelIndex];
        int stepIndex = stepIndexes[channelIndex];
        int stepSize = kStepSizeTable[stepIndex];

        int difference = targetSample - predicted;
        bool negative = (difference < 0);
        if (negative)
            difference = -difference;

        // Encode the absolute difference into 6 bits (bits 0-5 = magnitude, bit 6 = sign).
        // The decoder reconstructs: delta = sum of (stepSize >> k) for set bits + (stepSize >>
        // bitShift). We greedily pick bits from MSB to LSB of the magnitude portion.
        int encoded = 0;
        int baseStep = stepSize >> kBitShift;
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
