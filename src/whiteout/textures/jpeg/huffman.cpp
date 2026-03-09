// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "huffman.h"

namespace whiteout {
namespace textures {
namespace jpeg {

// ============================================================================
// BitstreamReader
// ============================================================================

void BitstreamReader::reset(const u8* data, size_t size, size_t startOffset) {
    streamData = data;
    streamSize = size;
    readPosition = startOffset;
    bitBuffer = 0;
    bitsAvailable = 0;
    pendingMarker = 0;
}

void BitstreamReader::refillBuffer() {
    while (bitsAvailable <= 24) {
        u8 byte = 0;
        if (pendingMarker || readPosition >= streamSize) {
            // Pad with zero bits when no more data is available.
        } else {
            byte = streamData[readPosition++];
            if (byte == 0xFF) {
                if (readPosition < streamSize) {
                    u8 nextByte = streamData[readPosition];
                    if (nextByte == 0x00) {
                        readPosition++; // Byte-stuffing: literal 0xFF value
                    } else {
                        pendingMarker = nextByte; // Real marker encountered
                        readPosition++;
                        byte = 0;
                    }
                } else {
                    byte = 0;
                }
            }
        }
        bitBuffer |= static_cast<u32>(byte) << (24 - bitsAvailable);
        bitsAvailable += 8;
    }
}

u32 BitstreamReader::peekBits(i32 count) {
    if (bitsAvailable < count) {
        refillBuffer();
    }
    return (bitBuffer >> (32 - count)) & BIT_MASK[count];
}

void BitstreamReader::consumeBits(i32 count) {
    bitBuffer <<= count;
    bitsAvailable -= count;
}

u32 BitstreamReader::readBits(i32 count) {
    if (count == 0) {
        return 0;
    }
    u32 value = peekBits(count);
    consumeBits(count);
    return value;
}

void BitstreamReader::handleRestartMarker() {
    bitBuffer = 0;
    bitsAvailable = 0;
    if (pendingMarker >= MARKER_RST0 && pendingMarker <= MARKER_RST0 + 7) {
        pendingMarker = 0;
    }
}

// ============================================================================
// HuffmanTable::build  (Decoder)
// ============================================================================

void HuffmanTable::build(const std::array<u8, 16>& codeLengthCounts, const u8* symbols) {
    isBuilt = false;
    fastCodeLength.fill(0);
    fastSymbol.fill(0);

    i32 totalSymbols = 0;
    for (i32 lengthIndex = 0; lengthIndex < 16; lengthIndex++) {
        totalSymbols += codeLengthCounts[lengthIndex];
    }
    std::memcpy(symbolTable.data(), symbols, totalSymbols);

    u16 currentCode = 0;
    i32 symbolIndex = 0;

    for (i32 codeLength = 1; codeLength <= 16; codeLength++) {
        indexDelta[codeLength] = symbolIndex - currentCode;

        i32 symbolsAtThisLength = codeLengthCounts[codeLength - 1];
        for (i32 symbolCount = 0; symbolCount < symbolsAtThisLength; symbolCount++) {
            // Populate the fast look-up table for short codes.
            if (codeLength <= HUFFMAN_FAST_BITS) {
                i32 tablePrefix = currentCode << (HUFFMAN_FAST_BITS - codeLength);
                i32 entriesPerCode = 1 << (HUFFMAN_FAST_BITS - codeLength);
                for (i32 entryIndex = 0; entryIndex < entriesPerCode; entryIndex++) {
                    fastSymbol[tablePrefix + entryIndex] = symbolTable[symbolIndex];
                    fastCodeLength[tablePrefix + entryIndex] = static_cast<u8>(codeLength);
                }
            }
            symbolIndex++;
            currentCode++;
        }
        maxcode[codeLength] = static_cast<u32>(currentCode) << (16 - codeLength);
        currentCode <<= 1;
    }
    maxcode[17] = 0x10000u; // Sentinel: always larger than any 16-bit value.
    isBuilt = true;
}

// ============================================================================
// HuffmanTable::decodeSymbol  (Decoder)
// ============================================================================

i32 HuffmanTable::decodeSymbol(BitstreamReader& reader) const {
    // Ensure at least 16 bits are available so the slow-path comparison
    // (which examines the top 16 bits of the buffer) has valid data in
    // all bit positions.  Without this, bits beyond bitsAvailable would
    // be zero-filled from prior shifts, causing incorrect code matching.
    if (reader.bitsAvailable < 16) {
        reader.refillBuffer();
    }
    u32 fastTableIndex =
        (reader.bitBuffer >> (32 - HUFFMAN_FAST_BITS)) & BIT_MASK[HUFFMAN_FAST_BITS];
    i32 fastLen = this->fastCodeLength[fastTableIndex];
    if (fastLen > 0) {
        reader.consumeBits(fastLen);
        return fastSymbol[fastTableIndex];
    }

    // Slow path: linear scan for codes longer than HUFFMAN_FAST_BITS.
    u32 codeValue = reader.bitBuffer >> 16;
    i32 codeLength;
    for (codeLength = HUFFMAN_FAST_BITS + 1; codeLength <= 16; codeLength++) {
        if (codeValue < maxcode[codeLength]) {
            break;
        }
    }
    if (codeLength > 16) {
        return -1;
    }
    i32 symbolTableIndex =
        static_cast<i32>((reader.bitBuffer >> (32 - codeLength)) & BIT_MASK[codeLength]) +
        indexDelta[codeLength];
    reader.consumeBits(codeLength);
    return symbolTable[symbolTableIndex];
}

// ============================================================================
// HuffmanEncodeTable::build  (Encoder)
// ============================================================================

void HuffmanEncodeTable::build(const u8* lengthCounts, const u8* symbols, i32 symbolCount) {
    u16 code = 0;
    i32 symbolIndex = 0;
    for (i32 length = 1; length <= 16; length++) {
        for (i32 j = 0; j < lengthCounts[length - 1]; j++) {
            if (symbolIndex < symbolCount) {
                codes[symbols[symbolIndex]].code = code;
                codes[symbols[symbolIndex]].length = static_cast<u8>(length);
                symbolIndex++;
            }
            code++;
        }
        code <<= 1;
    }
}

// ============================================================================
// BitstreamWriter
// ============================================================================

void BitstreamWriter::init(std::vector<u8>* out) {
    output = out;
    bitBuffer = 0;
    bitsUsed = 0;
}

void BitstreamWriter::writeBits(u32 value, i32 count) {
    // Shift value into the MSB-aligned position, leaving room for existing bits.
    bitBuffer |= (value & ((1u << count) - 1)) << (32 - bitsUsed - count);
    bitsUsed += count;

    // Flush complete bytes.
    while (bitsUsed >= 8) {
        u8 byte = static_cast<u8>(bitBuffer >> 24);
        output->push_back(byte);
        if (byte == 0xFF) {
            output->push_back(0x00); // Byte-stuffing.
        }
        bitBuffer <<= 8;
        bitsUsed -= 8;
    }
}

void BitstreamWriter::flushWithPadding() {
    if (bitsUsed > 0) {
        i32 padBits = 8 - bitsUsed;
        writeBits((1u << padBits) - 1, padBits);
    }
}

} // namespace jpeg
} // namespace textures
} // namespace whiteout
