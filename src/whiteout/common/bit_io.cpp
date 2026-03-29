// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "bit_io.h"

namespace whiteout {

// ============================================================================
// MsbBitReader
// ============================================================================

void MsbBitReader::init(const u8* d, size_t s, size_t startOffset) {
    data = d;
    size = s;
    bytePos = startOffset;
    bitBuf = 0;
    bitsAvail = 0;
    pendingMarker = 0;
}

void MsbBitReader::refill() {
    while (bitsAvail <= 24) {
        u8 byte = 0;
        if (pendingMarker || bytePos >= size) {
            // Pad with zero bits when no more data is available.
        } else {
            byte = data[bytePos++];
            if (byte == 0xFF) {
                if (bytePos < size) {
                    u8 nextByte = data[bytePos];
                    if (nextByte == 0x00) {
                        bytePos++; // Byte-stuffing: literal 0xFF value
                    } else {
                        pendingMarker = nextByte; // Real marker encountered
                        bytePos++;
                        byte = 0;
                    }
                } else {
                    byte = 0;
                }
            }
        }
        bitBuf |= static_cast<u32>(byte) << (24 - bitsAvail);
        bitsAvail += 8;
    }
}

u32 MsbBitReader::peekBits(i32 count) {
    if (bitsAvail < count) {
        refill();
    }
    return (bitBuf >> (32 - count)) & BIT_MASK[count];
}

void MsbBitReader::consumeBits(i32 count) {
    bitBuf <<= count;
    bitsAvail -= count;
}

u32 MsbBitReader::readBits(i32 count) {
    if (count == 0) {
        return 0;
    }
    u32 value = peekBits(count);
    consumeBits(count);
    return value;
}

void MsbBitReader::handleRestartMarker() {
    bitBuf = 0;
    bitsAvail = 0;
    // JPEG restart markers: 0xD0\u20130xD7.
    if (pendingMarker >= 0xD0 && pendingMarker <= 0xD7) {
        // refill() already consumed the marker bytes — just clear the flag.
        pendingMarker = 0;
    } else if (pendingMarker == 0 && bytePos + 1 < size && data[bytePos] == 0xFF &&
               data[bytePos + 1] >= 0xD0 && data[bytePos + 1] <= 0xD7) {
        // refill() didn't reach the marker — skip past it manually.
        bytePos += 2;
    }
}

// ============================================================================
// MsbBitWriter
// ============================================================================

void MsbBitWriter::init(std::vector<u8>* o) {
    out = o;
    bitBuf = 0;
    bitsUsed = 0;
}

void MsbBitWriter::writeBits(u32 value, i32 count) {
    // Shift value into the MSB-aligned position, leaving room for existing bits.
    bitBuf |= (value & ((1u << count) - 1)) << (32 - bitsUsed - count);
    bitsUsed += count;

    // Flush complete bytes.
    while (bitsUsed >= 8) {
        u8 byte = static_cast<u8>(bitBuf >> 24);
        out->push_back(byte);
        if (byte == 0xFF) {
            out->push_back(0x00); // Byte-stuffing.
        }
        bitBuf <<= 8;
        bitsUsed -= 8;
    }
}

void MsbBitWriter::flushWithPadding() {
    if (bitsUsed > 0) {
        i32 padBits = 8 - bitsUsed;
        writeBits((1u << padBits) - 1, padBits);
    }
}

} // namespace whiteout
