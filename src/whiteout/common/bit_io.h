// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bit_io.h
/// @brief Bit-level I/O primitives for both MSB-first (JPEG) and LSB-first (DEFLATE) codecs.
///
/// Provides:
///   - MsbBitReader / MsbBitWriter — MSB-first bit I/O with JPEG byte-stuffing
///     (0xFF 0x00) and restart-marker awareness.
///   - LsbBitReader / LsbBitWriter — LSB-first bit I/O for DEFLATE streams.

#pragma once

#include <array>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout {

// ============================================================================
// Constants
// ============================================================================

/// Precomputed bit-masks: BIT_MASK[n] == (1 << n) - 1.
inline constexpr std::array<u32, 17> BIT_MASK = {{
    0,
    1,
    3,
    7,
    15,
    31,
    63,
    127,
    255,
    511,
    1023,
    2047,
    4095,
    8191,
    16383,
    32767,
    65535,
}};

// ============================================================================
// MSB-first Bit Reader (plain — no byte-stuffing)
// ============================================================================

/// Reads bits MSB-first from a raw byte stream without any byte-stuffing.
/// Suitable for BZip2 and other non-JPEG MSB-first bit-packed formats.
struct PlainMsbBitReader {
    const u8* data = nullptr;
    size_t size = 0;
    size_t bytePos = 0;
    u32 bitBuf = 0;
    i32 bitsAvail = 0;

    void init(const u8* d, size_t s, size_t startByte = 0) {
        data = d;
        size = s;
        bytePos = startByte;
        bitBuf = 0;
        bitsAvail = 0;
    }

    void refill() {
        while (bitsAvail <= 24 && bytePos < size) {
            bitBuf |= static_cast<u32>(data[bytePos++]) << (24 - bitsAvail);
            bitsAvail += 8;
        }
    }

    u32 peekBits(i32 count) {
        refill();
        return (bitBuf >> (32 - count)) & BIT_MASK[count];
    }

    void consumeBits(i32 count) {
        bitBuf <<= count;
        bitsAvail -= count;
    }

    u32 readBits(i32 count) {
        u32 val = peekBits(count);
        consumeBits(count);
        return val;
    }

    bool hasData() const {
        return bytePos < size || bitsAvail > 0;
    }
};

// ============================================================================
// MSB-first Bit Reader (JPEG byte-stuffing aware)
// ============================================================================

/// Reads bits from a JPEG entropy-coded segment, handling the 0xFF 0x00
/// byte-stuffing sequence and detecting embedded restart/end markers.
struct MsbBitReader {
    const u8* data = nullptr;
    size_t size = 0;
    size_t bytePos = 0;

    u32 bitBuf = 0; ///< Buffered bits, MSB-aligned (next bit to read is bit 31).
    i32 bitsAvail = 0;
    u8 pendingMarker = 0; ///< Non-zero when a real marker (not byte-stuffing) was encountered.

    void init(const u8* d, size_t s, size_t startOffset);

    /// Fill the bit buffer until it holds at least 25 bits.
    void refill();

    /// Peek at the next `count` bits without consuming them.
    u32 peekBits(i32 count);

    /// Discard `count` bits from the front of the buffer.
    void consumeBits(i32 count);

    /// Read and consume `count` bits.
    u32 readBits(i32 count);

    /// Reset the bit buffer and skip past a restart marker (0xFF 0xDn).
    void handleRestartMarker();
};

// ============================================================================
// MSB-first Bit Writer (JPEG byte-stuffing aware)
// ============================================================================

/// Writes bits to a JPEG entropy-coded segment, handling the 0xFF → 0xFF 0x00
/// byte-stuffing requirement.
struct MsbBitWriter {
    std::vector<u8>* out = nullptr;
    u32 bitBuf = 0;   ///< Buffered bits, MSB-aligned.
    i32 bitsUsed = 0; ///< Number of valid bits in the buffer (counted from MSB).

    void init(std::vector<u8>* o);

    /// Write `count` bits (MSB-first) from the lower bits of `value`.
    void writeBits(u32 value, i32 count);

    /// Pad the remaining bits with 1-bits and flush (JPEG requires 1-bit padding).
    void flushWithPadding();
};

// ============================================================================
// LSB-first Bit Reader (DEFLATE convention)
// ============================================================================

/// Reads bits LSB-first from a raw byte stream (no byte-stuffing).
/// Uses a 64-bit accumulator so that up to 57 bits are available after
/// a single refill, reducing refill frequency in tight decode loops.
struct LsbBitReader {
    const u8* data = nullptr;
    size_t size = 0;
    size_t bytePos = 0;
    u64 bitBuf = 0;
    i32 bitsAvail = 0;

    void init(const u8* d, size_t s, size_t startByte) {
        data = d;
        size = s;
        bytePos = startByte;
        bitBuf = 0;
        bitsAvail = 0;
    }

    /// Bulk-refill: load up to 7 bytes at once so bitsAvail >= 56.
    void refill() {
        if (bitsAvail > 56) return; // Already have enough bits.
        // Fast path: if at least 8 bytes remain, load a full u64 and mask in.
        if (bytePos + 8 <= size) {
            u64 next = 0;
            std::memcpy(&next, data + bytePos, 8); // unaligned load
            bitBuf |= next << bitsAvail;
            i32 consume = (64 - bitsAvail) >> 3; // whole bytes that fit
            bytePos += consume;
            bitsAvail += consume * 8;
        } else {
            // Tail: byte-at-a-time.
            while (bitsAvail <= 56 && bytePos < size) {
                bitBuf |= static_cast<u64>(data[bytePos++]) << bitsAvail;
                bitsAvail += 8;
            }
        }
    }

    u32 readBits(i32 count) {
        if (bitsAvail < count) refill();
        u32 val = static_cast<u32>(bitBuf) & ((1u << count) - 1);
        bitBuf >>= count;
        bitsAvail -= count;
        return val;
    }

    u32 peekBits(i32 count) {
        if (bitsAvail < count) refill();
        return static_cast<u32>(bitBuf) & ((1u << count) - 1);
    }

    void consumeBits(i32 count) {
        bitBuf >>= count;
        bitsAvail -= count;
    }

    void alignToByte() {
        i32 discard = bitsAvail & 7;
        bitBuf >>= discard;
        bitsAvail -= discard;
    }

    bool hasData() const {
        return bytePos < size || bitsAvail > 0;
    }
};

// ============================================================================
// LSB-first Bit Writer (DEFLATE convention)
// ============================================================================

/// Writes bits LSB-first to a byte vector (no byte-stuffing).
struct LsbBitWriter {
    std::vector<u8>* out = nullptr;
    u32 bitBuf = 0;
    i32 bitsUsed = 0;

    void init(std::vector<u8>* o) {
        out = o;
        bitBuf = 0;
        bitsUsed = 0;
    }

    void writeBits(u32 value, i32 count) {
        bitBuf |= value << bitsUsed;
        bitsUsed += count;
        while (bitsUsed >= 8) {
            out->push_back(static_cast<u8>(bitBuf & 0xFF));
            bitBuf >>= 8;
            bitsUsed -= 8;
        }
    }

    void flush() {
        if (bitsUsed > 0) {
            out->push_back(static_cast<u8>(bitBuf & 0xFF));
            bitBuf = 0;
            bitsUsed = 0;
        }
    }
};

} // namespace whiteout
