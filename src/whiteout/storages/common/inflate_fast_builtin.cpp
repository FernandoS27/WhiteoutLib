// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file inflate_fast_builtin.cpp
/// @brief Built-in high-throughput zlib inflate for CASC BLTE frames.
///
/// Implementation behind storages::common::zlibInflateFast when WhiteoutLib is
/// built WITHOUT zlib-ng (WHITEOUT_USE_ZLIBNG=OFF). The zlib-ng-backed variant
/// lives in common/deflate_zlibng.cpp.
///
/// Differences from the general-purpose deflate.cpp inflate:
///   - Skips Adler-32 verification (BLTE frames use MD5 content hashes).
///   - When expectedSize is known (always for BLTE), pre-allocates output
///     exactly and eliminates all capacity checks from the hot loop.
///   - Uses 11-bit Huffman fast LUT (covers more codes without slow path).
///   - Packed u32 Huffman LUT (cache-line aligned, single access per lookup).
///   - Split fast/careful inflate loops (unconditional refill in hot path).
///   - 8-byte wide back-reference copies (compiler emits wide MOV pairs).
///   - Fixed-size symbol arrays (zero heap allocation during decompression).
///   - Stack-allocates dynamic Huffman code lengths (no heap per-block).

#include "inflate_fast.h"

#include <array>
#include <cstring>
#include <span>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::storages::common {

namespace inflate_detail {

// ============================================================================
// Bit Reader — 64-bit accumulator, LSB-first (DEFLATE convention)
// ============================================================================

struct BitReader {
    const u8* data;
    size_t bytePos;
    size_t size;
    u64 bitBuf;
    i32 bitsAvail;

    void init(const u8* d, size_t s, size_t start) {
        data = d;
        size = s;
        bytePos = start;
        bitBuf = 0;
        bitsAvail = 0;
    }

    void refill() {
        if (bitsAvail > 56)
            return;
        if (bytePos + 8 <= size) {
            u64 next = 0;
            std::memcpy(&next, data + bytePos, 8);
            bitBuf |= next << bitsAvail;
            i32 consume = (64 - bitsAvail) >> 3;
            bytePos += consume;
            bitsAvail += consume * 8;
        } else {
            while (bitsAvail <= 56 && bytePos < size) {
                bitBuf |= static_cast<u64>(data[bytePos++]) << bitsAvail;
                bitsAvail += 8;
            }
        }
    }

    u32 readBits(i32 count) {
        if (bitsAvail < count)
            refill();
        u32 val = static_cast<u32>(bitBuf) & ((1u << count) - 1);
        bitBuf >>= count;
        bitsAvail -= count;
        return val;
    }

    void alignToByte() {
        i32 discard = bitsAvail & 7;
        bitBuf >>= discard;
        bitsAvail -= discard;
    }
};

// ============================================================================
// Huffman Table — packed u32 fast LUT, cache-line aligned
// ============================================================================

inline constexpr i32 HUFF_FAST_BITS = 11;
inline constexpr i32 HUFF_FAST_SIZE = 1 << HUFF_FAST_BITS;
inline constexpr i32 HUFF_MAX_BITS = 15;
inline constexpr i32 MAX_SYMBOLS = 320; // 288 lit/len + 32 dist

struct HuffTable {
    // Packed entry: bits [15:0] = symbol, bits [23:16] = code length.
    // Length 0 → slow path.  Single contiguous array halves cache footprint
    // vs separate symbol/length arrays (one lookup instead of two).
    alignas(64) std::array<u32, HUFF_FAST_SIZE> fast{};

    std::array<u32, HUFF_MAX_BITS + 2> maxcode{};
    std::array<i32, HUFF_MAX_BITS + 1> indexDelta{};
    std::array<u16, MAX_SYMBOLS> symbols{};

    i32 lookupSlow(u32 code, i32 codeLength) const {
        i32 idx = static_cast<i32>(code) + indexDelta[codeLength];
        if (idx >= 0 && idx < MAX_SYMBOLS) [[likely]]
            return static_cast<i32>(symbols[idx]);
        return -1;
    }

    /// Slow-path decode operating directly on the caller's local bit
    /// variables, avoiding save/restore overhead through BitReader.
    i32 decodeSlow(u64& bits, i32& bitsLeft, const u8* brData, size_t& brPos, size_t brSize) const {
        if (bitsLeft < HUFF_MAX_BITS) {
            if (brPos + 8 <= brSize) {
                u64 next = 0;
                std::memcpy(&next, brData + brPos, 8);
                bits |= next << bitsLeft;
                i32 consume = (64 - bitsLeft) >> 3;
                brPos += consume;
                bitsLeft += consume * 8;
            } else {
                while (bitsLeft < HUFF_MAX_BITS && brPos < brSize) {
                    bits |= static_cast<u64>(brData[brPos++]) << bitsLeft;
                    bitsLeft += 8;
                }
            }
        }
        u32 c = 0;
        for (i32 len = 1; len <= HUFF_MAX_BITS; ++len) {
            c = (c << 1) | (static_cast<u32>(bits) & 1);
            bits >>= 1;
            bitsLeft -= 1;
            if (c < maxcode[len])
                return lookupSlow(c, len);
        }
        return -1;
    }

    bool build(const u8* codeLengths, i32 count) {
        std::array<i32, HUFF_MAX_BITS + 1> blCount{};
        for (i32 i = 0; i < count; ++i) {
            if (codeLengths[i] > HUFF_MAX_BITS)
                return false;
            blCount[codeLengths[i]]++;
        }
        blCount[0] = 0;

        std::array<i32, HUFF_MAX_BITS + 1> nextCode{};
        std::array<i32, HUFF_MAX_BITS + 1> firstCode{};
        i32 code = 0;
        for (i32 bits = 1; bits <= HUFF_MAX_BITS; ++bits) {
            code = (code + blCount[bits - 1]) << 1;
            nextCode[bits] = code;
            firstCode[bits] = code;
        }

        symbols.fill(0xFFFF);
        fast.fill(0);

        std::array<i32, HUFF_MAX_BITS + 1> counters{};
        std::array<i32, HUFF_MAX_BITS + 1> firstSymIdx{};
        {
            i32 offset = 0;
            for (i32 bits = 1; bits <= HUFF_MAX_BITS; ++bits) {
                counters[bits] = offset;
                firstSymIdx[bits] = offset;
                offset += blCount[bits];
            }
        }
        for (i32 i = 0; i < count; ++i) {
            i32 len = codeLengths[i];
            if (len > 0)
                symbols[counters[len]++] = static_cast<u16>(i);
        }

        for (i32 bits = 1; bits <= HUFF_MAX_BITS; ++bits) {
            maxcode[bits] = firstCode[bits] + blCount[bits];
            indexDelta[bits] = firstSymIdx[bits] - firstCode[bits];
        }
        maxcode[HUFF_MAX_BITS + 1] = 0xFFFFFFFF;

        for (i32 i = 0; i < count; ++i) {
            i32 len = codeLengths[i];
            if (len == 0 || len > HUFF_FAST_BITS)
                continue;
            i32 c = nextCode[len]++;
            i32 rev = 0;
            for (i32 b = 0; b < len; ++b)
                rev |= ((c >> (len - 1 - b)) & 1) << b;
            // Build packed fast entry: symbol | (codeLength << 16).
            u32 packed = static_cast<u32>(static_cast<u16>(i)) | (static_cast<u32>(len) << 16);
            for (i32 fill = rev; fill < HUFF_FAST_SIZE; fill += (1 << len))
                fast[fill] = packed;
        }
        for (i32 i = 0; i < count; ++i) {
            i32 len = codeLengths[i];
            if (len > HUFF_FAST_BITS)
                nextCode[len]++;
        }
        return true;
    }

    i32 decode(BitReader& br) const {
        br.refill();
        u32 peek = static_cast<u32>(br.bitBuf) & (HUFF_FAST_SIZE - 1);
        u32 entry = fast[peek];
        u8 codeLen = static_cast<u8>(entry >> 16);
        if (codeLen != 0) [[likely]] {
            br.bitBuf >>= codeLen;
            br.bitsAvail -= codeLen;
            return static_cast<i32>(static_cast<u16>(entry));
        }
        u32 c = 0;
        for (i32 len = 1; len <= HUFF_MAX_BITS; ++len) {
            c = (c << 1) | br.readBits(1);
            if (c < maxcode[len])
                return lookupSlow(c, len);
        }
        return -1;
    }
};

// ============================================================================
// DEFLATE Tables (RFC 1951)
// ============================================================================

struct LenDistEntry {
    u16 base;
    u8 extraBits;
};

static constexpr std::array<LenDistEntry, 29> LENGTH_TABLE = {{
    {3, 0},  {4, 0},  {5, 0},  {6, 0},   {7, 0},   {8, 0},   {9, 0},   {10, 0},  {11, 1},  {13, 1},
    {15, 1}, {17, 1}, {19, 2}, {23, 2},  {27, 2},  {31, 2},  {35, 3},  {43, 3},  {51, 3},  {59, 3},
    {67, 4}, {83, 4}, {99, 4}, {115, 4}, {131, 5}, {163, 5}, {195, 5}, {227, 5}, {258, 0},
}};

static constexpr std::array<LenDistEntry, 30> DISTANCE_TABLE = {{
    {1, 0},     {2, 0},     {3, 0},     {4, 0},      {5, 1},      {7, 1},
    {9, 2},     {13, 2},    {17, 3},    {25, 3},     {33, 4},     {49, 4},
    {65, 5},    {97, 5},    {129, 6},   {193, 6},    {257, 7},    {385, 7},
    {513, 8},   {769, 8},   {1025, 9},  {1537, 9},   {2049, 10},  {3073, 10},
    {4097, 11}, {6145, 11}, {8193, 12}, {12289, 12}, {16385, 13}, {24577, 13},
}};

static constexpr std::array<i32, 19> CL_CODE_ORDER = {{
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
}};

// ============================================================================
// Fixed Huffman Table Builders
// ============================================================================

inline HuffTable buildFixedLitLenTable() {
    std::array<u8, 288> lengths{};
    for (i32 i = 0; i <= 143; ++i)
        lengths[i] = 8;
    for (i32 i = 144; i <= 255; ++i)
        lengths[i] = 9;
    for (i32 i = 256; i <= 279; ++i)
        lengths[i] = 7;
    for (i32 i = 280; i <= 287; ++i)
        lengths[i] = 8;
    HuffTable t;
    t.build(lengths.data(), 288);
    return t;
}

inline HuffTable buildFixedDistTable() {
    std::array<u8, 32> lengths{};
    for (i32 i = 0; i < 32; ++i)
        lengths[i] = 5;
    HuffTable t;
    t.build(lengths.data(), 32);
    return t;
}

// ============================================================================
// Inflater — known-size fast path (no capacity checks in hot loop)
// ============================================================================

struct Inflater {
    BitReader br;
    u8* output;        ///< Pre-allocated output buffer (caller owns).
    size_t outSize;    ///< Total capacity of output buffer.
    size_t outPos = 0; ///< Current write position.

    bool inflateStored() {
        br.alignToByte();
        u32 len = br.readBits(16);
        u32 nlen = br.readBits(16);
        if ((len ^ nlen) != 0xFFFF)
            return false;
        // Rewind bytePos to logical stream position.
        br.bytePos -= static_cast<size_t>(br.bitsAvail / 8);
        br.bitsAvail = 0;
        br.bitBuf = 0;
        if (br.bytePos + len > br.size)
            return false;
        if (outPos + len > outSize)
            return false;
        std::memcpy(output + outPos, br.data + br.bytePos, len);
        outPos += len;
        br.bytePos += len;
        return true;
    }

    bool inflateBlock(const HuffTable& litLen, const HuffTable& dist) {
        u8* const outBuf = output;
        const size_t outCap = outSize;
        size_t wp = outPos;

        u64 bits = br.bitBuf;
        i32 bitsLeft = br.bitsAvail;
        const u8* brData = br.data;
        size_t brPos = br.bytePos;
        const size_t brSize = br.size;

        constexpr u32 FAST_MASK = HUFF_FAST_SIZE - 1;
        const u32* llFast = litLen.fast.data();
        const u32* dFast = dist.fast.data();

        // Safety margins for the fast inner loop.
        // Input:  8 bytes for unconditional 64-bit refill.
        // Output: 258 bytes for max match length without bounds checks.
        const size_t brSafeEnd = brSize >= 8 ? brSize - 8 : 0;
        const size_t wpSafeEnd = outCap >= 258 ? outCap - 258 : 0;

        // ==============================================================
        // FAST LOOP — runs while input/output safety margins hold.
        // Unconditional 8-byte refill + no output bounds checks let the
        // compiler emit tight, branch-prediction-friendly machine code.
        // ==============================================================
        while (brPos <= brSafeEnd && wp <= wpSafeEnd) [[likely]] {
            // 64-bit refill — the outer while-condition guarantees
            // brPos + 8 <= brSize, so we skip that check here.
            // Guard on bitsLeft <= 56 prevents UB from shifting u64 by >= 64.
            if (bitsLeft <= 56) {
                u64 next;
                std::memcpy(&next, brData + brPos, 8);
                bits |= next << bitsLeft;
                i32 consume = (64 - bitsLeft) >> 3;
                brPos += consume;
                bitsLeft += consume * 8;
            }

            // Decode literal/length (single packed table lookup).
            i32 sym;
            {
                u32 entry = llFast[static_cast<u32>(bits) & FAST_MASK];
                u8 codeLen = static_cast<u8>(entry >> 16);
                if (codeLen) [[likely]] {
                    sym = static_cast<i32>(static_cast<u16>(entry));
                    bits >>= codeLen;
                    bitsLeft -= codeLen;
                } else {
                    sym = litLen.decodeSlow(bits, bitsLeft, brData, brPos, brSize);
                }
            }

            if (sym < 256) [[likely]] {
                if (sym < 0) [[unlikely]]
                    return false;
                outBuf[wp++] = static_cast<u8>(sym);
                continue;
            }

            if (sym == 256) [[unlikely]] {
                outPos = wp;
                br.bitBuf = bits;
                br.bitsAvail = bitsLeft;
                br.bytePos = brPos;
                return true;
            }

            // Length/distance back-reference.
            {
                i32 lenIdx = sym - 257;
                if (lenIdx < 0 || lenIdx >= 29) [[unlikely]]
                    return false;
                u32 length = LENGTH_TABLE[lenIdx].base;
                u32 lenExtra = LENGTH_TABLE[lenIdx].extraBits;
                if (lenExtra) {
                    length += static_cast<u32>(bits) & ((1u << lenExtra) - 1);
                    bits >>= lenExtra;
                    bitsLeft -= lenExtra;
                }

                // Distance decode (single packed table lookup).
                i32 distSym;
                {
                    u32 dEntry = dFast[static_cast<u32>(bits) & FAST_MASK];
                    u8 dCodeLen = static_cast<u8>(dEntry >> 16);
                    if (dCodeLen) [[likely]] {
                        distSym = static_cast<i32>(static_cast<u16>(dEntry));
                        bits >>= dCodeLen;
                        bitsLeft -= dCodeLen;
                    } else {
                        distSym = dist.decodeSlow(bits, bitsLeft, brData, brPos, brSize);
                    }
                }
                if (distSym < 0 || distSym >= 30) [[unlikely]]
                    return false;

                u32 distance = DISTANCE_TABLE[distSym].base;
                u32 distExtra = DISTANCE_TABLE[distSym].extraBits;
                if (distExtra) {
                    distance += static_cast<u32>(bits) & ((1u << distExtra) - 1);
                    bits >>= distExtra;
                    bitsLeft -= distExtra;
                }

                if (distance > wp) [[unlikely]]
                    return false;

                u8* dst = outBuf + wp;
                const u8* src = outBuf + wp - distance;
                wp += length;

                if (distance >= length) {
                    std::memcpy(dst, src, length);
                } else if (distance == 1) {
                    std::memset(dst, src[0], length);
                } else if (distance >= 8) {
                    // 8-byte-wide chunk copies for overlapping matches.
                    // Each load reads fully within already-written output
                    // (distance >= 8), so no stale-data hazard.  Compiler
                    // can emit MOV/MOVQ pairs, enabling ILP on the copy.
                    u32 rem = length;
                    while (rem >= 8) {
                        std::memcpy(dst, src, 8);
                        dst += 8;
                        src += 8;
                        rem -= 8;
                    }
                    if (rem)
                        std::memcpy(dst, src, rem);
                } else {
                    // Short overlap (distance 2-7): doubling copy.
                    size_t copied = distance;
                    std::memcpy(dst, src, distance);
                    while (copied < length) {
                        size_t chunk = copied < length - copied ? copied : length - copied;
                        std::memcpy(dst + copied, dst, chunk);
                        copied += chunk;
                    }
                }
            }
        }

        // ==============================================================
        // CAREFUL LOOP — handles remaining data near buffer boundaries.
        // Same decode logic with full bounds checking on input/output.
        // ==============================================================
        for (;;) {
            if (bitsLeft <= 56) {
                if (brPos + 8 <= brSize) {
                    u64 next = 0;
                    std::memcpy(&next, brData + brPos, 8);
                    bits |= next << bitsLeft;
                    i32 consume = (64 - bitsLeft) >> 3;
                    brPos += consume;
                    bitsLeft += consume * 8;
                } else {
                    while (bitsLeft <= 56 && brPos < brSize) {
                        bits |= static_cast<u64>(brData[brPos++]) << bitsLeft;
                        bitsLeft += 8;
                    }
                }
            }

            i32 sym;
            {
                u32 entry = llFast[static_cast<u32>(bits) & FAST_MASK];
                u8 codeLen = static_cast<u8>(entry >> 16);
                if (codeLen) [[likely]] {
                    sym = static_cast<i32>(static_cast<u16>(entry));
                    bits >>= codeLen;
                    bitsLeft -= codeLen;
                } else {
                    sym = litLen.decodeSlow(bits, bitsLeft, brData, brPos, brSize);
                }
            }

            if (sym < 256) [[likely]] {
                if (sym < 0) [[unlikely]]
                    return false;
                if (wp >= outCap) [[unlikely]]
                    return false;
                outBuf[wp++] = static_cast<u8>(sym);
                continue;
            }

            if (sym == 256) {
                outPos = wp;
                br.bitBuf = bits;
                br.bitsAvail = bitsLeft;
                br.bytePos = brPos;
                return true;
            }

            {
                i32 lenIdx = sym - 257;
                if (lenIdx < 0 || lenIdx >= 29) [[unlikely]]
                    return false;
                u32 length = LENGTH_TABLE[lenIdx].base;
                u32 lenExtra = LENGTH_TABLE[lenIdx].extraBits;
                if (lenExtra) {
                    length += static_cast<u32>(bits) & ((1u << lenExtra) - 1);
                    bits >>= lenExtra;
                    bitsLeft -= lenExtra;
                }

                i32 distSym;
                {
                    u32 dEntry = dFast[static_cast<u32>(bits) & FAST_MASK];
                    u8 dCodeLen = static_cast<u8>(dEntry >> 16);
                    if (dCodeLen) [[likely]] {
                        distSym = static_cast<i32>(static_cast<u16>(dEntry));
                        bits >>= dCodeLen;
                        bitsLeft -= dCodeLen;
                    } else {
                        distSym = dist.decodeSlow(bits, bitsLeft, brData, brPos, brSize);
                    }
                }
                if (distSym < 0 || distSym >= 30) [[unlikely]]
                    return false;

                u32 distance = DISTANCE_TABLE[distSym].base;
                u32 distExtra = DISTANCE_TABLE[distSym].extraBits;
                if (distExtra) {
                    distance += static_cast<u32>(bits) & ((1u << distExtra) - 1);
                    bits >>= distExtra;
                    bitsLeft -= distExtra;
                }

                if (distance > wp) [[unlikely]]
                    return false;
                if (wp + length > outCap) [[unlikely]]
                    return false;

                u8* dst = outBuf + wp;
                const u8* src = outBuf + wp - distance;
                wp += length;

                if (distance >= length) {
                    std::memcpy(dst, src, length);
                } else if (distance == 1) {
                    std::memset(dst, src[0], length);
                } else if (distance >= 8) {
                    u32 rem = length;
                    while (rem >= 8) {
                        std::memcpy(dst, src, 8);
                        dst += 8;
                        src += 8;
                        rem -= 8;
                    }
                    if (rem)
                        std::memcpy(dst, src, rem);
                } else {
                    size_t copied = distance;
                    std::memcpy(dst, src, distance);
                    while (copied < length) {
                        size_t chunk = copied < length - copied ? copied : length - copied;
                        std::memcpy(dst + copied, dst, chunk);
                        copied += chunk;
                    }
                }
            }
        }
    }

    bool inflateDynamic() {
        u32 hlit = br.readBits(5) + 257;
        u32 hdist = br.readBits(5) + 1;
        u32 hclen = br.readBits(4) + 4;

        if (hlit > 286 || hdist > 30)
            return false;

        std::array<u8, 19> clCodeLengths{};
        for (u32 i = 0; i < hclen; ++i)
            clCodeLengths[CL_CODE_ORDER[i]] = static_cast<u8>(br.readBits(3));

        HuffTable clTable;
        if (!clTable.build(clCodeLengths.data(), 19))
            return false;

        // Stack-allocate: max hlit + hdist = 286 + 30 = 316 bytes.
        std::array<u8, 316> codeLengths{};
        u32 totalCodes = hlit + hdist;
        u32 idx = 0;
        while (idx < totalCodes) {
            i32 sym = clTable.decode(br);
            if (sym < 0)
                return false;
            if (sym < 16) {
                codeLengths[idx++] = static_cast<u8>(sym);
            } else if (sym == 16) {
                if (idx == 0)
                    return false;
                u32 rep = br.readBits(2) + 3;
                u8 prev = codeLengths[idx - 1];
                for (u32 r = 0; r < rep && idx < totalCodes; ++r)
                    codeLengths[idx++] = prev;
            } else if (sym == 17) {
                u32 rep = br.readBits(3) + 3;
                for (u32 r = 0; r < rep && idx < totalCodes; ++r)
                    codeLengths[idx++] = 0;
            } else if (sym == 18) {
                u32 rep = br.readBits(7) + 11;
                for (u32 r = 0; r < rep && idx < totalCodes; ++r)
                    codeLengths[idx++] = 0;
            } else {
                return false;
            }
        }

        HuffTable litLenTable, distTable;
        if (!litLenTable.build(codeLengths.data(), static_cast<i32>(hlit)))
            return false;
        if (!distTable.build(codeLengths.data() + hlit, static_cast<i32>(hdist)))
            return false;

        return inflateBlock(litLenTable, distTable);
    }

    bool run(const u8* data, size_t size) {
        br.init(data, size, 0);
        bool isFinal = false;
        while (!isFinal) {
            isFinal = br.readBits(1) != 0;
            u32 btype = br.readBits(2);
            if (btype == 0) {
                if (!inflateStored())
                    return false;
            } else if (btype == 1) {
                static const HuffTable fixedLitLen = buildFixedLitLenTable();
                static const HuffTable fixedDist = buildFixedDistTable();
                if (!inflateBlock(fixedLitLen, fixedDist))
                    return false;
            } else if (btype == 2) {
                if (!inflateDynamic())
                    return false;
            } else {
                return false;
            }
        }
        return true;
    }
};

} // namespace inflate_detail

// ============================================================================
// Public API
// ============================================================================

/// Fast zlib-inflate for CASC BLTE frames.
///
/// Skips Adler-32 verification (BLTE already verifies integrity via MD5).
/// When @p expectedSize > 0, pre-allocates exactly that size and eliminates
/// all capacity checks from the hot loop.
///
/// @param src           zlib-wrapped DEFLATE stream (RFC 1950).
/// @param expectedSize  Expected decompressed size (0 = unknown, falls back to heuristic).
/// @return Decompressed bytes, or empty vector on failure.
std::vector<u8> zlibInflateFast(std::span<const u8> src, size_t expectedSize) {
    if (src.size() < 6)
        return {};

    // Parse zlib header (RFC 1950).
    u8 cmf = src[0];
    u8 flg = src[1];
    if (((static_cast<u16>(cmf) << 8) | flg) % 31 != 0)
        return {};
    if ((cmf & 0x0F) != 8)
        return {};
    if (flg & 0x20) // preset dictionary not supported
        return {};

    size_t deflateStart = 2;
    size_t deflateEnd = src.size() - 4; // Last 4 bytes = Adler-32 (skipped).

    // Determine output size.
    size_t outSize = expectedSize > 0 ? expectedSize : src.size() * 4;
    if (outSize < 4096)
        outSize = 4096;

    std::vector<u8> output(outSize);

    inflate_detail::Inflater inflater;
    inflater.output = output.data();
    inflater.outSize = outSize;

    if (!inflater.run(src.data() + deflateStart, deflateEnd - deflateStart))
        return {};

    output.resize(inflater.outPos);
    return output;
}

} // namespace whiteout::storages::common
