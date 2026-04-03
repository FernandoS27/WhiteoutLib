// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "huffman_table.h"

namespace whiteout {

// ============================================================================
// MsbHuffmanTable::build (JPEG-style)
// ============================================================================

void MsbHuffmanTable::build(const std::array<u8, 16>& codeLengthCounts, const u8* syms) {
    isBuilt = false;
    fastLen.fill(0);
    fastSymbol.fill(0);

    i32 totalSymbols = 0;
    for (i32 lengthIndex = 0; lengthIndex < 16; lengthIndex++) {
        totalSymbols += codeLengthCounts[lengthIndex];
    }
    symbols.assign(syms, syms + totalSymbols);

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
                    fastSymbol[tablePrefix + entryIndex] = symbols[symbolIndex];
                    fastLen[tablePrefix + entryIndex] = static_cast<u8>(codeLength);
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
// MsbHuffmanTable::decodeSymbol (JPEG-style)
// ============================================================================

i32 MsbHuffmanTable::decodeSymbol(MsbBitReader& reader) const {
    if (reader.bitsAvail < 16) {
        reader.refill();
    }
    u32 fastTableIndex = (reader.bitBuf >> (32 - HUFFMAN_FAST_BITS)) & BIT_MASK[HUFFMAN_FAST_BITS];
    i32 codeLen = this->fastLen[fastTableIndex];
    if (codeLen > 0) {
        reader.consumeBits(codeLen);
        return fastSymbol[fastTableIndex];
    }

    // Slow path: linear scan for codes longer than HUFFMAN_FAST_BITS.
    u32 codeValue = reader.bitBuf >> 16;
    i32 codeLength;
    for (codeLength = HUFFMAN_FAST_BITS + 1; codeLength <= MAX_BITS; codeLength++) {
        if (codeValue < maxcode[codeLength]) {
            break;
        }
    }
    if (codeLength > MAX_BITS) {
        return -1;
    }
    u32 code = (reader.bitBuf >> (32 - codeLength)) & BIT_MASK[codeLength];
    reader.consumeBits(codeLength);
    return lookupSlow(code, codeLength);
}

// ============================================================================
// LsbHuffmanTable::build (DEFLATE-style)
// ============================================================================

bool LsbHuffmanTable::build(const u8* codeLengths, i32 count) {
    // Count codes of each length.
    std::array<i32, MAX_BITS + 1> blCount{};
    for (i32 i = 0; i < count; ++i) {
        if (codeLengths[i] > MAX_BITS)
            return false;
        blCount[codeLengths[i]]++;
    }
    blCount[0] = 0;

    // Compute starting code for each length.
    std::array<i32, MAX_BITS + 1> nextCode{};
    std::array<i32, MAX_BITS + 1> firstCode{};
    i32 code = 0;
    for (i32 bits = 1; bits <= MAX_BITS; ++bits) {
        code = (code + blCount[bits - 1]) << 1;
        nextCode[bits] = code;
        firstCode[bits] = code;
    }

    // Build symbol list ordered by (length, code).
    symbols.resize(count);
    fastLen.fill(0);
    fastSymbol.fill(0);

    std::vector<u16> sortedSymbols(count, 0xFFFF);
    std::array<i32, MAX_BITS + 1> counters{};
    std::array<i32, MAX_BITS + 1> firstSymIdx{};
    {
        i32 offset = 0;
        for (i32 bits = 1; bits <= MAX_BITS; ++bits) {
            counters[bits] = offset;
            firstSymIdx[bits] = offset;
            offset += blCount[bits];
        }
    }
    for (i32 i = 0; i < count; ++i) {
        i32 len = codeLengths[i];
        if (len > 0) {
            sortedSymbols[counters[len]++] = static_cast<u16>(i);
        }
    }
    symbols = sortedSymbols;

    // Build slow-path tables.
    for (i32 bits = 1; bits <= MAX_BITS; ++bits) {
        maxcode[bits] = firstCode[bits] + blCount[bits];
        indexDelta[bits] = firstSymIdx[bits] - firstCode[bits];
    }
    maxcode[MAX_BITS + 1] = 0xFFFFFFFF;

    // Build fast table with bit-reversed codes (DEFLATE is LSB-first).
    for (i32 i = 0; i < count; ++i) {
        i32 len = codeLengths[i];
        if (len == 0 || len > HUFFMAN_FAST_BITS)
            continue;
        i32 c = nextCode[len]++;
        // Reverse bits for the fast table.
        i32 rev = 0;
        for (i32 b = 0; b < len; ++b) {
            rev |= ((c >> (len - 1 - b)) & 1) << b;
        }
        for (i32 fill = rev; fill < FAST_SIZE; fill += (1 << len)) {
            fastSymbol[fill] = static_cast<u16>(i);
            fastLen[fill] = static_cast<u8>(len);
        }
    }
    // Advance nextCode for symbols that go in slow table.
    for (i32 i = 0; i < count; ++i) {
        i32 len = codeLengths[i];
        if (len > HUFFMAN_FAST_BITS) {
            nextCode[len]++;
        }
    }

    return true;
}

// ============================================================================
// HuffmanEncodeTable::build
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

} // namespace whiteout
