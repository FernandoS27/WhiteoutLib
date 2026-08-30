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
///   - 10-bit Huffman fast LUT holding fully decoded symbols: literal byte, or
///     match/distance base plus its extra-bit count, so the hot loop never
///     touches the RFC 1951 length and distance tables.
///   - Packed u32 Huffman LUT (cache-line aligned, single access per lookup).
///   - Split fast/careful inflate loops (unconditional refill in hot path).
///   - Two literals decoded per refill (75% of symbols here are literals).
///   - Fixed-width 8-byte back-reference copies (no variable-length memcpy call).
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

inline constexpr i32 HUFF_FAST_BITS = 10;
inline constexpr i32 HUFF_FAST_SIZE = 1 << HUFF_FAST_BITS;
inline constexpr i32 HUFF_MAX_BITS = 15;
inline constexpr i32 MAX_SYMBOLS = 320; // 288 lit/len + 32 dist

// Decoded-symbol layout, shared by the fast LUT and the slow path:
//   [15:0]  value    literal byte, or match length base, or distance base
//   [23:16] codeLen  Huffman code length (0 = not in the fast LUT)
//   [27:24] extra    extra bits to read after the code
//   [31:28] op       kOpLiteral / kOpMatch / kOpEnd / kOpBad
// Resolving length and distance bases here keeps the two RFC 1951 tables and
// their range checks out of the decode loop.
inline constexpr u32 kOpLiteral = 0u << 28;
inline constexpr u32 kOpMatch = 1u << 28;
inline constexpr u32 kOpEnd = 2u << 28;
inline constexpr u32 kOpBad = 3u << 28;

enum class TableKind : u8 { LitLen, Dist };

/// Bit-reversal of each byte. The fast LUT is indexed by the code read
/// LSB-first, so every entry's index is its canonical code reversed.
inline constexpr std::array<u8, 256> kReverseByte = [] {
    std::array<u8, 256> t{};
    for (i32 i = 0; i < 256; ++i) {
        u32 v = static_cast<u32>(i);
        v = ((v & 0x55) << 1) | ((v >> 1) & 0x55);
        v = ((v & 0x33) << 2) | ((v >> 2) & 0x33);
        v = ((v & 0x0F) << 4) | ((v >> 4) & 0x0F);
        t[static_cast<size_t>(i)] = static_cast<u8>(v);
    }
    return t;
}();

inline u32 reverseCode(u32 code, i32 len) {
    u32 const r = (static_cast<u32>(kReverseByte[code & 0xFF]) << 8) |
                  static_cast<u32>(kReverseByte[(code >> 8) & 0xFF]);
    return r >> (16 - len);
}

inline u32 packSymbol(TableKind kind, i32 sym) {
    if (kind == TableKind::Dist) {
        if (sym < 0 || sym >= 30)
            return kOpBad;
        return kOpMatch | (static_cast<u32>(DISTANCE_TABLE[sym].extraBits) << 24) |
               static_cast<u32>(DISTANCE_TABLE[sym].base);
    }
    if (sym < 0)
        return kOpBad;
    if (sym < 256)
        return kOpLiteral | static_cast<u32>(sym);
    if (sym == 256)
        return kOpEnd;
    i32 const idx = sym - 257;
    if (idx >= 29)
        return kOpBad;
    return kOpMatch | (static_cast<u32>(LENGTH_TABLE[idx].extraBits) << 24) |
           static_cast<u32>(LENGTH_TABLE[idx].base);
}

struct HuffTable {
    // Fully decoded entries in the layout above. Code length 0 → slow path.
    // Left uninitialised: build() writes every slot a code reaches and clears
    // only the rest, which for a complete code is none of them.
    alignas(64) std::array<u32, HUFF_FAST_SIZE> fast;

    TableKind kind = TableKind::LitLen;

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
    ///
    /// Refills to a full accumulator rather than to the 15 bits a code needs:
    /// the caller may go on to read length and distance codes plus their extra
    /// bits without refilling again, and the fast loop's budget assumes it.
    u32 decodeSlow(u64& bits, i32& bitsLeft, const u8* brData, size_t& brPos, size_t brSize) const {
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
        u32 c = 0;
        for (i32 len = 1; len <= HUFF_MAX_BITS; ++len) {
            c = (c << 1) | (static_cast<u32>(bits) & 1);
            bits >>= 1;
            bitsLeft -= 1;
            if (c < maxcode[len])
                return packSymbol(kind, lookupSlow(c, len));
        }
        return kOpBad;
    }

    bool build(const u8* codeLengths, i32 count, TableKind k = TableKind::LitLen) {
        kind = k;
        std::array<i32, HUFF_MAX_BITS + 1> blCount{};
        for (i32 i = 0; i < count; ++i) {
            if (codeLengths[i] > HUFF_MAX_BITS)
                return false;
            blCount[codeLengths[i]]++;
        }
        blCount[0] = 0;

        // Reject an over-subscribed tree. The LUT is no longer wiped between
        // builds, so a malformed length set would otherwise decode against
        // whatever the previous block left in the slots it fails to cover.
        i32 left = 1;
        for (i32 bits = 1; bits <= HUFF_MAX_BITS; ++bits) {
            left = (left << 1) - blCount[bits];
            if (left < 0)
                return false;
        }

        std::array<i32, HUFF_MAX_BITS + 1> nextCode{};
        std::array<i32, HUFF_MAX_BITS + 1> firstCode{};
        i32 code = 0;
        for (i32 bits = 1; bits <= HUFF_MAX_BITS; ++bits) {
            code = (code + blCount[bits - 1]) << 1;
            nextCode[bits] = code;
            firstCode[bits] = code;
        }

        symbols.fill(0xFFFF);

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
            if (len == 0)
                continue;
            i32 const c = nextCode[len]++;
            if (len > HUFF_FAST_BITS)
                continue;
            u32 const packed = packSymbol(kind, i) | (static_cast<u32>(len) << 16);
            for (u32 fill = reverseCode(static_cast<u32>(c), len); fill < HUFF_FAST_SIZE;
                 fill += (1u << len))
                fast[fill] = packed;
        }

        // Codes at most HUFF_FAST_BITS long fill the canonical prefix range
        // [0, covered) and nothing else, so only the tail needs clearing —
        // and a code that fits entirely in the LUT needs none at all.
        i32 covered = 0;
        for (i32 bits = 1; bits <= HUFF_FAST_BITS; ++bits)
            covered += blCount[bits] << (HUFF_FAST_BITS - bits);
        for (i32 prefix = covered; prefix < HUFF_FAST_SIZE; ++prefix)
            fast[reverseCode(static_cast<u32>(prefix), HUFF_FAST_BITS)] = 0;
        return true;
    }

    /// Decode one code-length-alphabet symbol. Its 19 symbols all pack as
    /// literals, so the value field is the symbol itself.
    i32 decode(BitReader& br) const {
        br.refill();
        u32 entry = fast[static_cast<u32>(br.bitBuf) & (HUFF_FAST_SIZE - 1)];
        u8 codeLen = static_cast<u8>(entry >> 16);
        if (codeLen != 0) [[likely]] {
            br.bitBuf >>= codeLen;
            br.bitsAvail -= codeLen;
        } else {
            entry = decodeSlow(br.bitBuf, br.bitsAvail, br.data, br.bytePos, br.size);
        }
        if ((entry & 0xF0000000u) != kOpLiteral)
            return -1;
        return static_cast<i32>(static_cast<u16>(entry));
    }
};

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
    t.build(lengths.data(), 32, TableKind::Dist);
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
        // 258 = longest match, +8 for the chunked copy's overwrite past its end.
        constexpr size_t kOutMargin = 258 + 8;
        const size_t wpSafeEnd = outCap >= kOutMargin ? outCap - kOutMargin : 0;

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
            u32 entry = llFast[static_cast<u32>(bits) & FAST_MASK];
            {
                u32 codeLen = (entry >> 16) & 0xFF;
                if (codeLen) [[likely]] {
                    bits >>= codeLen;
                    bitsLeft -= codeLen;
                } else {
                    entry = litLen.decodeSlow(bits, bitsLeft, brData, brPos, brSize);
                }
            }

            if (entry < kOpMatch) [[likely]] {
                outBuf[wp++] = static_cast<u8>(entry);

                // A second literal off the same refill. The accumulator held at
                // least 57 bits on entry and a fast-path code is at most 11, so
                // whatever this decodes — literal or a whole match with its
                // extra bits — still has the bits it needs.
                entry = llFast[static_cast<u32>(bits) & FAST_MASK];
                u32 codeLen = (entry >> 16) & 0xFF;
                if (codeLen) [[likely]] {
                    bits >>= codeLen;
                    bitsLeft -= codeLen;
                } else {
                    entry = litLen.decodeSlow(bits, bitsLeft, brData, brPos, brSize);
                }
                if (entry < kOpMatch) [[likely]] {
                    outBuf[wp++] = static_cast<u8>(entry);
                    continue;
                }
            }

            if (entry >= kOpEnd) [[unlikely]] {
                if (entry >= kOpBad)
                    return false;
                outPos = wp;
                br.bitBuf = bits;
                br.bitsAvail = bitsLeft;
                br.bytePos = brPos;
                return true;
            }

            // Length/distance back-reference.
            {
                u32 length = static_cast<u16>(entry);
                u32 lenExtra = (entry >> 24) & 0xF;
                if (lenExtra) {
                    length += static_cast<u32>(bits) & ((1u << lenExtra) - 1);
                    bits >>= lenExtra;
                    bitsLeft -= lenExtra;
                }

                u32 dEntry = dFast[static_cast<u32>(bits) & FAST_MASK];
                u32 dCodeLen = (dEntry >> 16) & 0xFF;
                if (dCodeLen) [[likely]] {
                    bits >>= dCodeLen;
                    bitsLeft -= dCodeLen;
                } else {
                    dEntry = dist.decodeSlow(bits, bitsLeft, brData, brPos, brSize);
                }
                if (dEntry >= kOpEnd) [[unlikely]]
                    return false;

                u32 distance = static_cast<u16>(dEntry);
                u32 distExtra = (dEntry >> 24) & 0xF;
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

                if (distance >= 8) [[likely]] {
                    // Fixed-width chunks: a variable-length memcpy is a call,
                    // and the average match here is 5 bytes. Reading 8 stays
                    // inside already-written output because distance >= 8, and
                    // writing up to 7 bytes past the match is absorbed by the
                    // output margin the loop condition maintains.
                    u8* const end = dst + length;
                    do {
                        std::memcpy(dst, src, 8);
                        dst += 8;
                        src += 8;
                    } while (dst < end);
                } else if (distance == 1) {
                    std::memset(dst, src[0], length);
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

            u32 entry = llFast[static_cast<u32>(bits) & FAST_MASK];
            {
                u32 codeLen = (entry >> 16) & 0xFF;
                if (codeLen) [[likely]] {
                    bits >>= codeLen;
                    bitsLeft -= codeLen;
                } else {
                    entry = litLen.decodeSlow(bits, bitsLeft, brData, brPos, brSize);
                }
            }

            if (entry < kOpMatch) [[likely]] {
                if (wp >= outCap) [[unlikely]]
                    return false;
                outBuf[wp++] = static_cast<u8>(entry);
                continue;
            }

            if (entry >= kOpEnd) {
                if (entry >= kOpBad)
                    return false;
                outPos = wp;
                br.bitBuf = bits;
                br.bitsAvail = bitsLeft;
                br.bytePos = brPos;
                return true;
            }

            {
                u32 length = static_cast<u16>(entry);
                u32 lenExtra = (entry >> 24) & 0xF;
                if (lenExtra) {
                    length += static_cast<u32>(bits) & ((1u << lenExtra) - 1);
                    bits >>= lenExtra;
                    bitsLeft -= lenExtra;
                }

                u32 dEntry = dFast[static_cast<u32>(bits) & FAST_MASK];
                u32 dCodeLen = (dEntry >> 16) & 0xFF;
                if (dCodeLen) [[likely]] {
                    bits >>= dCodeLen;
                    bitsLeft -= dCodeLen;
                } else {
                    dEntry = dist.decodeSlow(bits, bitsLeft, brData, brPos, brSize);
                }
                if (dEntry >= kOpEnd) [[unlikely]]
                    return false;

                u32 distance = static_cast<u16>(dEntry);
                u32 distExtra = (dEntry >> 24) & 0xF;
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
        if (!distTable.build(codeLengths.data() + hlit, static_cast<i32>(hdist), TableKind::Dist))
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
std::optional<size_t> zlibInflateFastInto(std::span<u8> dst, std::span<const u8> src) {
    if (src.size() < 6)
        return std::nullopt;

    // Parse zlib header (RFC 1950).
    u8 cmf = src[0];
    u8 flg = src[1];
    if (((static_cast<u16>(cmf) << 8) | flg) % 31 != 0)
        return std::nullopt;
    if ((cmf & 0x0F) != 8)
        return std::nullopt;
    if (flg & 0x20) // preset dictionary not supported
        return std::nullopt;

    size_t const deflateStart = 2;
    size_t const deflateEnd = src.size() - 4; // Last 4 bytes = Adler-32 (skipped).

    inflate_detail::Inflater inflater;
    inflater.output = dst.data();
    inflater.outSize = dst.size();

    if (!inflater.run(src.data() + deflateStart, deflateEnd - deflateStart))
        return std::nullopt;
    return inflater.outPos;
}

std::vector<u8> zlibInflateFast(std::span<const u8> src, size_t expectedSize) {
    size_t outSize = expectedSize > 0 ? expectedSize : src.size() * 4;
    if (outSize < 4096)
        outSize = 4096;

    std::vector<u8> output(outSize);
    auto written = zlibInflateFastInto(output, src);
    if (!written)
        return {};
    output.resize(*written);
    return output;
}

} // namespace whiteout::storages::common
