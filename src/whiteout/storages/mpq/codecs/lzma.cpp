// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Minimal single-file LZMA decompressor (decode-only). No external deps.
// Implements the LZMA range-coded LZ77 decoder as documented in the LZMA
// specification and the Wikipedia article on LZMA. The encoder is not
// implemented — MPQ archives that use LZMA (0x12) can be read but we do
// not produce LZMA-compressed output.

#include "lzma.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace whiteout::storages::mpq {
namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr u32 kNumBitModelTotalBits = 11;
constexpr u32 kBitModelTotal = 1u << kNumBitModelTotalBits; // 2048
constexpr u32 kNumMoveBits = 5;
constexpr u32 kTopValue = 1u << 24;

constexpr u32 kNumPosBitsMax = 4;
constexpr u32 kNumPosStatesMax = 1u << kNumPosBitsMax; // 16

constexpr u32 kNumStates = 12;
constexpr u32 kNumLenToPosStates = 4;
constexpr u32 kNumAlignBits = 4;
constexpr u32 kAlignTableSize = 1u << kNumAlignBits; // 16
constexpr u32 kEndPosModelIndex = 14;
constexpr u32 kStartPosModelIndex = 4;
constexpr u32 kNumFullDistances = 1u << (kEndPosModelIndex >> 1); // 128

constexpr u32 kMatchMinLen = 2;
constexpr u32 kLenNumLowBits = 3;
constexpr u32 kLenNumLowSymbols = 1u << kLenNumLowBits;   // 8
constexpr u32 kLenNumMidBits = 3;
constexpr u32 kLenNumMidSymbols = 1u << kLenNumMidBits;   // 8
constexpr u32 kLenNumHighBits = 8;
constexpr u32 kLenNumHighSymbols = 1u << kLenNumHighBits;  // 256

constexpr u16 kProbInitValue = kBitModelTotal / 2; // 1024

// ---------------------------------------------------------------------------
// Probability variable type
// ---------------------------------------------------------------------------

using Prob = u16;

// ---------------------------------------------------------------------------
// Range decoder
// ---------------------------------------------------------------------------

class RangeDecoder {
  public:
    bool init(const u8* data, size_t size) {
        if (size < 5) return false;
        streamBytes = data;
        streamLength = size;
        readPosition = 0;

        // First byte is ignored (should be 0x00)
        readPosition++;

        code = 0;
        for (int i = 0; i < 4; ++i)
            code = (code << 8) | consumeByte();

        range = 0xFFFFFFFF;
        corrupted = false;
        return true;
    }

    bool isFinished() const {
        return readPosition >= streamLength && code == 0;
    }

    bool isCorrupted() const { return corrupted; }

    u32 decodeBit(Prob& probability) {
        normalize();
        u32 bound = (range >> kNumBitModelTotalBits) * probability;
        if (code < bound) {
            range = bound;
            probability += static_cast<Prob>((kBitModelTotal - probability) >> kNumMoveBits);
            return 0;
        } else {
            range -= bound;
            code -= bound;
            probability -= static_cast<Prob>(probability >> kNumMoveBits);
            return 1;
        }
    }

    u32 decodeDirectBits(u32 numBits) {
        u32 result = 0;
        for (u32 i = 0; i < numBits; ++i) {
            normalize();
            range >>= 1;
            code -= range;
            u32 signMask = 0 - (code >> 31); // 0 if code >= 0 (bit=1), ~0 if code < 0 (bit=0)
            code += range & signMask;         // correct code if bit was 0
            result = (result << 1) | (1 - (signMask & 1));
        }
        return result;
    }

  private:
    void normalize() {
        if (range < kTopValue) {
            range <<= 8;
            code = (code << 8) | consumeByte();
        }
    }

    u8 consumeByte() {
        if (readPosition < streamLength)
            return streamBytes[readPosition++];
        corrupted = true;
        return 0;
    }

    const u8* streamBytes = nullptr;
    size_t streamLength = 0;
    size_t readPosition = 0;
    u32 range = 0;
    u32 code = 0;
    bool corrupted = false;
};

// ---------------------------------------------------------------------------
// Bit-tree decoder (MSB-first)
// ---------------------------------------------------------------------------

u32 bitTreeDecode(Prob* probabilities, u32 numBits, RangeDecoder& rangeDecoder) {
    u32 treeNode = 1;
    for (u32 i = 0; i < numBits; ++i)
        treeNode = (treeNode << 1) | rangeDecoder.decodeBit(probabilities[treeNode]);
    return treeNode - (1u << numBits);
}

// ---------------------------------------------------------------------------
// Reverse bit-tree decoder (LSB-first)
// ---------------------------------------------------------------------------

u32 bitTreeReverseDecode(Prob* probabilities, u32 numBits, RangeDecoder& rangeDecoder) {
    u32 treeNode = 1;
    u32 symbol = 0;
    for (u32 i = 0; i < numBits; ++i) {
        u32 bit = rangeDecoder.decodeBit(probabilities[treeNode]);
        treeNode = (treeNode << 1) | bit;
        symbol |= bit << i;
    }
    return symbol;
}

// ---------------------------------------------------------------------------
// Length decoder
// ---------------------------------------------------------------------------

struct LenDecoder {
    Prob choiceLow = kProbInitValue;
    Prob choiceHigh = kProbInitValue;
    std::array<Prob, kNumPosStatesMax * kLenNumLowSymbols> lowProbs{};
    std::array<Prob, kNumPosStatesMax * kLenNumMidSymbols> midProbs{};
    std::array<Prob, kLenNumHighSymbols> highProbs{};

    LenDecoder() {
        lowProbs.fill(kProbInitValue);
        midProbs.fill(kProbInitValue);
        highProbs.fill(kProbInitValue);
    }

    u32 decode(RangeDecoder& rangeDecoder, u32 posState) {
        if (rangeDecoder.decodeBit(choiceLow) == 0) {
            return bitTreeDecode(&lowProbs[posState * kLenNumLowSymbols], kLenNumLowBits, rangeDecoder);
        }
        if (rangeDecoder.decodeBit(choiceHigh) == 0) {
            return kLenNumLowSymbols +
                   bitTreeDecode(&midProbs[posState * kLenNumMidSymbols], kLenNumMidBits, rangeDecoder);
        }
        return kLenNumLowSymbols + kLenNumMidSymbols +
               bitTreeDecode(highProbs.data(), kLenNumHighBits, rangeDecoder);
    }
};

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------

u32 stateUpdateLiteral(u32 state) {
    if (state < 4) return 0;
    if (state < 10) return state - 3;
    return state - 6;
}

u32 stateUpdateMatch(u32 state) {
    return (state < 7) ? 7 : 10;
}

u32 stateUpdateRep(u32 state) {
    return (state < 7) ? 8 : 11;
}

u32 stateUpdateShortRep(u32 state) {
    return (state < 7) ? 9 : 11;
}

bool stateIsLiteral(u32 state) {
    return state < 7;
}

u32 getLenToPosState(u32 len) {
    len -= kMatchMinLen;
    return (len < kNumLenToPosStates) ? len : kNumLenToPosStates - 1;
}

// ---------------------------------------------------------------------------
// Output dictionary
// ---------------------------------------------------------------------------

class OutputWindow {
  public:
    void init(size_t dictSize, size_t expectedSize) {
        buffer.resize(expectedSize);
        writePosition = 0;
        dictionarySize = dictSize;
        outputCapacity = expectedSize;
    }

    bool isFull() const { return writePosition >= outputCapacity; }

    void putByte(u8 value) {
        if (writePosition < outputCapacity)
            buffer[writePosition++] = value;
    }

    u8 getByte(u32 distance) const {
        // distance is 0-based: 0 = last byte written
        if (distance + 1 > writePosition) return 0;
        return buffer[writePosition - 1 - distance];
    }

    void copyMatch(u32 distance, u32 length) {
        for (u32 i = 0; i < length && writePosition < outputCapacity; ++i) {
            u8 value = getByte(distance);
            putByte(value);
        }
    }

    std::vector<u8> finish() {
        buffer.resize(writePosition);
        return std::move(buffer);
    }

    size_t pos() const { return writePosition; }

  private:
    std::vector<u8> buffer;
    size_t writePosition = 0;
    size_t dictionarySize = 0;
    size_t outputCapacity = 0;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

std::vector<u8> lzmaDecompress(std::span<const u8> src, size_t expectedSize) {
    // Minimum valid input: 5 bytes props header + 5 bytes range coder init
    if (src.size() < 10 || expectedSize == 0)
        return {};

    // -------------------------------------------------------------------
    // Parse properties header (5 bytes)
    // -------------------------------------------------------------------
    const u8 propsByte = src[0];
    if (propsByte >= 225) // 9 * 5 * 5 = 225 is the maximum valid value
        return {};

    const u32 literalContextBits = propsByte % 9;
    const u32 remainder = propsByte / 9;
    const u32 literalPosBits = remainder % 5;
    const u32 posBits = remainder / 5;

    u32 dictionarySize = 0;
    std::memcpy(&dictionarySize, src.data() + 1, 4); // little-endian on x86
    if (dictionarySize < 1) dictionarySize = 1;

    // Stream data starts at offset 5
    const u8* streamData = src.data() + 5;
    const size_t streamSize = src.size() - 5;

    // -------------------------------------------------------------------
    // Initialize range decoder
    // -------------------------------------------------------------------
    RangeDecoder rangeDecoder;
    if (!rangeDecoder.init(streamData, streamSize))
        return {};

    // -------------------------------------------------------------------
    // Initialize probability model
    // -------------------------------------------------------------------
    const u32 numLiteralProbSets = 1u << (literalContextBits + literalPosBits);
    const u32 kLiteralCoderSize = 0x300; // 3 * 256

    // Probability arrays
    std::array<Prob, kNumStates * kNumPosStatesMax> isMatchProbs{};
    std::array<Prob, kNumStates> isRepProbs{};
    std::array<Prob, kNumStates> isRepG0Probs{};
    std::array<Prob, kNumStates> isRepG1Probs{};
    std::array<Prob, kNumStates> isRepG2Probs{};
    std::array<Prob, kNumStates * kNumPosStatesMax> isRep0LongProbs{};

    // Literal probs: numLiteralProbSets * 0x300 entries
    std::vector<Prob> literalProbs(numLiteralProbSets * kLiteralCoderSize, kProbInitValue);

    // Distance probs
    std::array<Prob, kNumLenToPosStates * 64> distSlotProbs{};  // 64 = 1<<6, bit-tree of 6 bits
    std::array<Prob, kNumFullDistances - kEndPosModelIndex> distSpecialProbs{};
    std::array<Prob, kAlignTableSize> distAlignProbs{};

    // Fill all prob arrays with initial value
    isMatchProbs.fill(kProbInitValue);
    isRepProbs.fill(kProbInitValue);
    isRepG0Probs.fill(kProbInitValue);
    isRepG1Probs.fill(kProbInitValue);
    isRepG2Probs.fill(kProbInitValue);
    isRep0LongProbs.fill(kProbInitValue);
    distSlotProbs.fill(kProbInitValue);
    distSpecialProbs.fill(kProbInitValue);
    distAlignProbs.fill(kProbInitValue);

    LenDecoder matchLenDecoder;
    LenDecoder repLenDecoder;

    // -------------------------------------------------------------------
    // Initialize output + state
    // -------------------------------------------------------------------
    OutputWindow output;
    output.init(dictionarySize, expectedSize);

    u32 state = 0;
    u32 repDist0 = 0, repDist1 = 0, repDist2 = 0, repDist3 = 0;

    // -------------------------------------------------------------------
    // Main decode loop
    // -------------------------------------------------------------------
    while (!output.isFull()) {
        if (rangeDecoder.isCorrupted())
            return {};

        const u32 posState = static_cast<u32>(output.pos()) & ((1u << posBits) - 1);

        // Decode: is this a match or a literal?
        if (rangeDecoder.decodeBit(isMatchProbs[state * kNumPosStatesMax + posState]) == 0) {
            // -----------------------------------------------------------
            // LITERAL
            // -----------------------------------------------------------
            const u32 previousByte = (output.pos() > 0) ? output.getByte(0) : 0;
            const u32 literalState =
                ((static_cast<u32>(output.pos()) & ((1u << literalPosBits) - 1)) << literalContextBits) |
                (previousByte >> (8 - literalContextBits));

            Prob* contextProbs = &literalProbs[literalState * kLiteralCoderSize];

            u32 symbol = 1;
            if (stateIsLiteral(state)) {
                // Simple literal decode: bit-tree on 8 bits
                do {
                    symbol = (symbol << 1) | rangeDecoder.decodeBit(contextProbs[symbol]);
                } while (symbol < 0x100);
            } else {
                // Match-literal: context is the byte at repDist0 distance
                u32 matchByte = output.getByte(repDist0);
                u32 matchContextOffset = 0x100;
                do {
                    matchByte <<= 1;
                    u32 matchBit = (matchByte >> 8) & 1;
                    u32 bit = rangeDecoder.decodeBit(contextProbs[matchContextOffset + (matchBit << 8) + symbol]);
                    symbol = (symbol << 1) | bit;
                    // Stay in match-context while bits agree; once they differ, offset→0 permanently
                    matchContextOffset = (matchBit == bit) ? matchContextOffset : 0;
                } while (symbol < 0x100);
            }

            output.putByte(static_cast<u8>(symbol & 0xFF));
            state = stateUpdateLiteral(state);

        } else {
            // -----------------------------------------------------------
            // Some kind of match/rep
            // -----------------------------------------------------------
            u32 matchLength;

            if (rangeDecoder.decodeBit(isRepProbs[state]) == 0) {
                // ---------------------------------------------------
                // Simple MATCH
                // ---------------------------------------------------
                repDist3 = repDist2;
                repDist2 = repDist1;
                repDist1 = repDist0;

                matchLength = matchLenDecoder.decode(rangeDecoder, posState) + kMatchMinLen;
                state = stateUpdateMatch(state);

                // Decode distance
                u32 lengthState = getLenToPosState(matchLength);
                u32 distanceSlot = bitTreeDecode(&distSlotProbs[lengthState * 64], 6, rangeDecoder);

                if (distanceSlot < kStartPosModelIndex) {
                    repDist0 = distanceSlot;
                } else {
                    u32 numDirectBits = (distanceSlot >> 1) - 1;
                    repDist0 = (2 | (distanceSlot & 1)) << numDirectBits;

                    if (distanceSlot < kEndPosModelIndex) {
                        // Context-encoded bits (reverse bit-tree)
                        repDist0 += bitTreeReverseDecode(
                            &distSpecialProbs[repDist0 - distanceSlot - 1],
                            numDirectBits, rangeDecoder);
                    } else {
                        // Fixed-probability bits + align bits
                        repDist0 += rangeDecoder.decodeDirectBits(numDirectBits - kNumAlignBits) << kNumAlignBits;
                        repDist0 += bitTreeReverseDecode(distAlignProbs.data(), kNumAlignBits, rangeDecoder);
                    }
                }

                // Check for end-of-stream marker (dist = 0xFFFFFFFF)
                if (repDist0 == 0xFFFFFFFF) {
                    break; // End of stream
                }

                // Validate distance
                if (repDist0 >= output.pos() && repDist0 >= dictionarySize) {
                    return {}; // Invalid distance
                }

            } else {
                // ---------------------------------------------------
                // REP match
                // ---------------------------------------------------
                if (rangeDecoder.decodeBit(isRepG0Probs[state]) == 0) {
                    // repDist0 stays as is
                    if (rangeDecoder.decodeBit(isRep0LongProbs[state * kNumPosStatesMax + posState]) == 0) {
                        // SHORT REP (length 1)
                        if (output.pos() == 0)
                            return {}; // can't rep from empty dict
                        state = stateUpdateShortRep(state);
                        output.putByte(output.getByte(repDist0));
                        continue;
                    }
                    // LONG REP[0]
                } else {
                    u32 selectedDist;
                    if (rangeDecoder.decodeBit(isRepG1Probs[state]) == 0) {
                        selectedDist = repDist1;
                    } else {
                        if (rangeDecoder.decodeBit(isRepG2Probs[state]) == 0) {
                            selectedDist = repDist2;
                        } else {
                            selectedDist = repDist3;
                            repDist3 = repDist2;
                        }
                        repDist2 = repDist1;
                    }
                    repDist1 = repDist0;
                    repDist0 = selectedDist;
                }

                matchLength = repLenDecoder.decode(rangeDecoder, posState) + kMatchMinLen;
                state = stateUpdateRep(state);
            }

            // Execute the copy
            output.copyMatch(repDist0, matchLength);
        }
    }

    return output.finish();
}

} // namespace whiteout::storages::mpq
