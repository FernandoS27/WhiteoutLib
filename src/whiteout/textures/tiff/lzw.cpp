// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "lzw.h"

#include <array>
#include <cstring>

#include <whiteout/common_types.h>

namespace whiteout::textures::tiff {

namespace {

constexpr u32 kClearCode = 256;
constexpr u32 kEoiCode = 257;
constexpr u32 kFirstFreeCode = 258;
constexpr u32 kMaxCode = 1u << 12; // 12-bit codes max
constexpr int kMinCodeSize = 9;
constexpr int kMaxCodeSize = 12;

/// MSB-first bit reader over a contiguous byte stream.
struct MsbBitReader {
    const u8* data;
    size_t size;
    size_t bytePos = 0;
    u32 bitBuf = 0;
    int bitsAvail = 0;

    MsbBitReader(std::span<const u8> s) : data(s.data()), size(s.size()) {}

    void refill() {
        while (bitsAvail <= 24 && bytePos < size) {
            bitBuf |= static_cast<u32>(data[bytePos++]) << (24 - bitsAvail);
            bitsAvail += 8;
        }
    }

    /// Returns -1 on EOF (insufficient bits remaining).
    int readBits(int n) {
        if (bitsAvail < n)
            refill();
        if (bitsAvail < n)
            return -1;
        const u32 v = (bitBuf >> (32 - n)) & ((1u << n) - 1u);
        bitBuf <<= n;
        bitsAvail -= n;
        return static_cast<int>(v);
    }
};

/// String table entry — fixed-capacity ring of (prefix, suffix) pairs.
/// During decode we walk the prefix chain to reconstruct the full string.
struct Entry {
    u16 prefix; // index of the previous entry (or 0xFFFF for root literals)
    u8 suffix;
    u16 length; // total decoded length of this entry (>= 1)
};

constexpr u16 kNoPrefix = 0xFFFF;

/// Reconstruct the byte sequence for `code` by walking the prefix chain,
/// writing into `out` in reverse, then returning the start byte. Returns
/// the number of bytes written, or 0 if the code is invalid.
u16 emitCodeBytes(const std::array<Entry, kMaxCode>& table, u32 code,
                  u8* out, size_t outCap, size_t writePos) {
    // First locate length and validate.
    if (code >= kMaxCode)
        return 0;
    u32 cur = code;
    u16 len = table[cur].length;
    if (len == 0)
        return 0;
    if (writePos + len > outCap)
        return 0;

    // Walk backwards filling out[writePos + len - 1] down to writePos.
    u8* dst = out + writePos + len;
    while (cur != kNoPrefix && len > 0) {
        *(--dst) = table[cur].suffix;
        cur = table[cur].prefix;
        --len;
    }
    return table[code].length;
}

} // namespace

std::vector<u8> lzwDecompress(std::span<const u8> src, size_t expectedSize) {
    if (src.empty())
        return {};

    std::vector<u8> out;
    if (expectedSize > 0)
        out.reserve(expectedSize);

    std::array<Entry, kMaxCode> table{};
    // Initialize the first 256 entries as single-byte literals.
    for (u32 i = 0; i < 256; ++i)
        table[i] = Entry{kNoPrefix, static_cast<u8>(i), 1};
    // Codes 256 (CLEAR) and 257 (EOI) carry no data.

    MsbBitReader br(src);

    int codeSize = kMinCodeSize;
    u32 nextCode = kFirstFreeCode;
    u32 prevCode = static_cast<u32>(-1);

    while (true) {
        int code = br.readBits(codeSize);
        if (code < 0) {
            // Some encoders (libtiff in certain configurations) terminate
            // strips by exhausting the bitstream rather than by emitting an
            // explicit EOI code. If we've already produced the caller's
            // expected output size, accept the stream as complete.
            if (expectedSize > 0 && out.size() == expectedSize)
                return out;
            return {};
        }

        const u32 c = static_cast<u32>(code);

        if (c == kEoiCode)
            break;

        if (c == kClearCode) {
            codeSize = kMinCodeSize;
            nextCode = kFirstFreeCode;
            prevCode = static_cast<u32>(-1);
            continue;
        }

        // Two valid cases: existing code (c < nextCode) or KwKwK (c == nextCode).
        if (c > nextCode)
            return {}; // invalid code

        // Reserve space then walk-emit.
        const u16 writePos16 = 0; // unused; we use the index into `out`.
        (void)writePos16;

        if (c < nextCode) {
            // Existing entry — emit its string.
            const u16 len = table[c].length;
            const size_t before = out.size();
            out.resize(before + len);
            if (emitCodeBytes(table, c, out.data(), out.size(), before) == 0)
                return {};

            if (prevCode != static_cast<u32>(-1) && nextCode < kMaxCode) {
                // Add new table entry: prev + first byte of current.
                const u8 firstByte = out[before];
                Entry& e = table[nextCode];
                e.prefix = static_cast<u16>(prevCode);
                e.suffix = firstByte;
                e.length = static_cast<u16>(table[prevCode].length + 1);
                ++nextCode;
                // TIFF early-change: bump when next_code == (1 << codeSize) - 1.
                if (codeSize < kMaxCodeSize &&
                    nextCode == ((1u << codeSize) - 1))
                    ++codeSize;
            }
        } else {
            // KwKwK: c == nextCode, emit prev + prev[0].
            if (prevCode == static_cast<u32>(-1))
                return {}; // invalid: KwKwK without a previous code
            const u16 prevLen = table[prevCode].length;
            const size_t before = out.size();
            const size_t newLen = static_cast<size_t>(prevLen) + 1;
            out.resize(before + newLen);
            // Reconstruct prev, then append prev[0].
            if (emitCodeBytes(table, prevCode, out.data(), out.size(), before) == 0)
                return {};
            const u8 firstByte = out[before];
            out[before + prevLen] = firstByte;

            if (nextCode < kMaxCode) {
                Entry& e = table[nextCode];
                e.prefix = static_cast<u16>(prevCode);
                e.suffix = firstByte;
                e.length = static_cast<u16>(prevLen + 1);
                ++nextCode;
                if (codeSize < kMaxCodeSize &&
                    nextCode == ((1u << codeSize) - 1))
                    ++codeSize;
            }
        }

        prevCode = c;

        if (expectedSize > 0 && out.size() > expectedSize)
            return {};
    }

    return out;
}

} // namespace whiteout::textures::tiff
