// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file deflate.cpp
/// @brief Minimal zlib-wrapped DEFLATE inflate/deflate implementation.
///
/// Implements RFC 1950 (zlib wrapper) + RFC 1951 (DEFLATE) with:
///   - Inflate: stored, fixed Huffman, and dynamic Huffman blocks.
///   - Deflate: LZ77 with hash-chain matching + fixed Huffman encoding.
///
/// No external dependencies (no zlib).

#include "deflate.h"

#include "bit_io.h"
#include "checksum.h"
#include "huffman_table.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace whiteout {

using BitReader = LsbBitReader;
using BitWriter = LsbBitWriter;
using HuffTable = LsbHuffmanTable;

namespace {

// ============================================================================
// Huffman Table Builders for DEFLATE Fixed Codes
// ============================================================================

HuffTable buildFixedLitLenTable() {
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

HuffTable buildFixedDistTable() {
    std::array<u8, 32> lengths{};
    for (i32 i = 0; i < 32; ++i)
        lengths[i] = 5;
    HuffTable t;
    t.build(lengths.data(), 32);
    return t;
}

// ============================================================================
// DEFLATE Length/Distance Tables (RFC 1951 Section 3.2.5)
// ============================================================================

struct LenDistEntry {
    u16 base;
    u8 extraBits;
};

// Length codes 257-285.
static constexpr std::array<LenDistEntry, 29> LENGTH_TABLE = {{
    {3, 0},  {4, 0},  {5, 0},  {6, 0},   {7, 0},   {8, 0},   {9, 0},   {10, 0},  {11, 1},  {13, 1},
    {15, 1}, {17, 1}, {19, 2}, {23, 2},  {27, 2},  {31, 2},  {35, 3},  {43, 3},  {51, 3},  {59, 3},
    {67, 4}, {83, 4}, {99, 4}, {115, 4}, {131, 5}, {163, 5}, {195, 5}, {227, 5}, {258, 0},
}};

// Distance codes 0-29.
static constexpr std::array<LenDistEntry, 30> DISTANCE_TABLE = {{
    {1, 0},     {2, 0},     {3, 0},     {4, 0},      {5, 1},      {7, 1},
    {9, 2},     {13, 2},    {17, 3},    {25, 3},     {33, 4},     {49, 4},
    {65, 5},    {97, 5},    {129, 6},   {193, 6},    {257, 7},    {385, 7},
    {513, 8},   {769, 8},   {1025, 9},  {1537, 9},   {2049, 10},  {3073, 10},
    {4097, 11}, {6145, 11}, {8193, 12}, {12289, 12}, {16385, 13}, {24577, 13},
}};

// Code length code order (RFC 1951 Section 3.2.7).
static constexpr std::array<i32, 19> CL_CODE_ORDER = {{
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
}};

// ============================================================================
// Inflate Implementation
// ============================================================================

struct Inflater {
    BitReader br;
    std::vector<u8> output;
    size_t outPos = 0; ///< Current write position within output.
    std::string* errorOut;

    bool error(const char* msg) const {
        if (errorOut)
            *errorOut = msg;
        return false;
    }

    /// Ensure at least @p n bytes are available at outPos.
    void ensureSpace(size_t n) {
        size_t const needed = outPos + n;
        if (needed > output.size()) {
            // Grow by at least 2x to amortize reallocations.
            size_t newCap = output.size() * 2;
            if (newCap < needed) newCap = needed;
            if (newCap < 4096) newCap = 4096;
            output.resize(newCap);
        }
    }

    bool inflateStored() {
        br.alignToByte();
        // After alignToByte, the bit buffer may still hold pre-fetched whole
        // bytes (from the 64-bit bulk refill). readBits() consumes from the
        // buffer, but bytePos has already been advanced past those bytes.
        // We read LEN/NLEN through the bit reader, then rewind bytePos to
        // account for any remaining buffered bytes before the bulk memcpy.
        u32 const len = br.readBits(16);
        u32 const nlen = br.readBits(16);
        if ((len ^ nlen) != 0xFFFF)
            return error("Stored block LEN/NLEN mismatch");
        // Rewind bytePos to the logical stream position (after LEN/NLEN).
        br.bytePos -= static_cast<size_t>(br.bitsAvail / 8);
        br.bitsAvail = 0;
        br.bitBuf = 0;
        // Bulk copy stored block data.
        if (br.bytePos + len > br.size)
            return error("Truncated stored block data");
        ensureSpace(len);
        std::memcpy(output.data() + outPos, br.data + br.bytePos, len);
        outPos += len;
        br.bytePos += len;
        return true;
    }

    bool inflateBlock(const HuffTable& litLen, const HuffTable& dist) {
        // Cache output state locally.
        u8* outBuf = output.data();
        size_t outCap = output.size();
        size_t wp = outPos;

        // Cache ALL bit-reader state in locals to eliminate member-access overhead.
        u64 bits = br.bitBuf;
        i32 bitsLeft = br.bitsAvail;
        const u8* brData = br.data;
        size_t brPos = br.bytePos;
        size_t const brSize = br.size;

        // Raw table pointers for fast Huffman lookup (avoids std::array bounds).
        constexpr u32 FAST_MASK = HuffTable::FAST_SIZE - 1;
        const u16* llSym = litLen.fastSymbol.data();
        const u8* llLen = litLen.fastLen.data();
        const u16* dSym = dist.fastSymbol.data();
        const u8* dLen = dist.fastLen.data();

        for (;;) {
            // ---- Single refill per iteration ----
            // After refill, bitsLeft >= 56. Worst-case consumption per
            // iteration is 48 bits (15 litLen + 5 lenExtra + 15 dist + 13 distExtra),
            // so one refill always suffices for the entire iteration.
            if (bitsLeft <= 56) {
                if (brPos + 8 <= brSize) {
                    u64 next = 0;
                    std::memcpy(&next, brData + brPos, 8);
                    bits |= next << bitsLeft;
                    i32 const consume = (64 - bitsLeft) >> 3;
                    brPos += consume;
                    bitsLeft += consume * 8;
                } else {
                    while (bitsLeft <= 56 && brPos < brSize) {
                        bits |= static_cast<u64>(brData[brPos++]) << bitsLeft;
                        bitsLeft += 8;
                    }
                }
            }

            // ---- Decode literal/length symbol (inlined fast path) ----
            i32 sym;
            {
                u32 const peek = static_cast<u32>(bits) & FAST_MASK;
                u8 const codeLen = llLen[peek];
                if (codeLen) {
                    sym = llSym[peek];
                    bits >>= codeLen;
                    bitsLeft -= codeLen;
                } else {
                    // Slow path (~1% of codes): sync state, call full decode.
                    br.bitBuf = bits;
                    br.bitsAvail = bitsLeft;
                    br.bytePos = brPos;
                    sym = litLen.decode(br);
                    bits = br.bitBuf;
                    bitsLeft = br.bitsAvail;
                    brPos = br.bytePos;
                }
            }

            // ---- Literal ----
            if (sym < 256) {
                if (sym < 0)
                    return error("Invalid literal/length code");
                if (wp >= outCap) {
                    outPos = wp;
                    ensureSpace(4096);
                    outBuf = output.data();
                    outCap = output.size();
                }
                outBuf[wp++] = static_cast<u8>(sym);
                continue;
            }

            // ---- End of block ----
            if (sym == 256) {
                outPos = wp;
                br.bitBuf = bits;
                br.bitsAvail = bitsLeft;
                br.bytePos = brPos;
                return true;
            }

            // ---- Length/Distance back-reference ----
            {
                i32 const lenIdx = sym - 257;
                if (lenIdx < 0 || lenIdx >= 29)
                    return error("Invalid length code");
                u32 length = LENGTH_TABLE[lenIdx].base;
                u32 const lenExtra = LENGTH_TABLE[lenIdx].extraBits;
                if (lenExtra) {
                    length += static_cast<u32>(bits) & ((1u << lenExtra) - 1);
                    bits >>= lenExtra;
                    bitsLeft -= lenExtra;
                }

                // Distance decode (inlined fast path, no refill needed —
                // we still have at least 56 - 20 = 36 bits, enough for any
                // distance code (max 15) + extra (max 13) = 28 bits).
                i32 distSym;
                {
                    u32 const dPeek = static_cast<u32>(bits) & FAST_MASK;
                    u8 const dCodeLen = dLen[dPeek];
                    if (dCodeLen) {
                        distSym = dSym[dPeek];
                        bits >>= dCodeLen;
                        bitsLeft -= dCodeLen;
                    } else {
                        br.bitBuf = bits;
                        br.bitsAvail = bitsLeft;
                        br.bytePos = brPos;
                        distSym = dist.decode(br);
                        bits = br.bitBuf;
                        bitsLeft = br.bitsAvail;
                        brPos = br.bytePos;
                    }
                }
                if (distSym < 0 || distSym >= 30)
                    return error("Invalid distance code");

                u32 distance = DISTANCE_TABLE[distSym].base;
                u32 const distExtra = DISTANCE_TABLE[distSym].extraBits;
                if (distExtra) {
                    distance += static_cast<u32>(bits) & ((1u << distExtra) - 1);
                    bits >>= distExtra;
                    bitsLeft -= distExtra;
                }

                if (distance > wp)
                    return error("Distance exceeds output buffer");

                outPos = wp;
                ensureSpace(length);
                outBuf = output.data();
                outCap = output.size();
                u8* dst = outBuf + wp;
                const u8* src = outBuf + wp - distance;
                if (distance >= length) {
                    std::memcpy(dst, src, length);
                } else if (distance == 1) {
                    std::memset(dst, src[0], length);
                } else {
                    size_t copied = distance;
                    std::memcpy(dst, src, distance);
                    while (copied < length) {
                        size_t chunk = copied;
                        if (chunk > length - copied) chunk = length - copied;
                        std::memcpy(dst + copied, dst, chunk);
                        copied += chunk;
                    }
                }
                wp += length;
            }
        }
    }

    bool inflateDynamic() {
        u32 const hlit = br.readBits(5) + 257;
        u32 const hdist = br.readBits(5) + 1;
        u32 const hclen = br.readBits(4) + 4;

        if (hlit > 286 || hdist > 30)
            return error("Invalid dynamic Huffman header");

        // Read code length code lengths.
        std::array<u8, 19> clCodeLengths{};
        for (u32 i = 0; i < hclen; ++i) {
            clCodeLengths[CL_CODE_ORDER[i]] = static_cast<u8>(br.readBits(3));
        }

        HuffTable clTable;
        if (!clTable.build(clCodeLengths.data(), 19))
            return error("Failed to build code length Huffman table");

        // Decode literal/length + distance code lengths.
        u32 const totalCodes = hlit + hdist;
        std::vector<u8> codeLengths(totalCodes, 0);
        u32 idx = 0;
        while (idx < totalCodes) {
            i32 const sym = clTable.decode(br);
            if (sym < 0)
                return error("Invalid code length symbol");

            if (sym < 16) {
                codeLengths[idx++] = static_cast<u8>(sym);
            } else if (sym == 16) {
                if (idx == 0)
                    return error("Repeat code with no previous length");
                u32 const rep = br.readBits(2) + 3;
                u8 const prev = codeLengths[idx - 1];
                for (u32 r = 0; r < rep && idx < totalCodes; ++r)
                    codeLengths[idx++] = prev;
            } else if (sym == 17) {
                u32 const rep = br.readBits(3) + 3;
                for (u32 r = 0; r < rep && idx < totalCodes; ++r)
                    codeLengths[idx++] = 0;
            } else if (sym == 18) {
                u32 const rep = br.readBits(7) + 11;
                for (u32 r = 0; r < rep && idx < totalCodes; ++r)
                    codeLengths[idx++] = 0;
            } else {
                return error("Unknown code length symbol");
            }
        }

        HuffTable litLenTable, distTable;
        if (!litLenTable.build(codeLengths.data(), static_cast<i32>(hlit)))
            return error("Failed to build dynamic literal/length table");

        if (!distTable.build(codeLengths.data() + hlit, static_cast<i32>(hdist)))
            return error("Failed to build dynamic distance table");

        return inflateBlock(litLenTable, distTable);
    }

    bool run(const u8* data, size_t size) {
        br.init(data, size, 0);

        bool isFinal = false;
        while (!isFinal) {
            isFinal = br.readBits(1) != 0;
            u32 const btype = br.readBits(2);

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
                return error("Invalid DEFLATE block type 3");
            }
        }
        return true;
    }
};

// ============================================================================
// Deflate Implementation (Fixed Huffman + LZ77 Hash Chain)
// ============================================================================

// Reverse `count` bits of `value` (DEFLATE codes are sent LSB-first).
u32 reverseBits(u32 value, i32 count) {
    u32 result = 0;
    for (i32 i = 0; i < count; ++i) {
        result = (result << 1) | (value & 1);
        value >>= 1;
    }
    return result;
}

// Encode a literal/length symbol using fixed Huffman codes (RFC 1951 §3.2.6).
void writeFixedLitLen(BitWriter& bw, i32 sym) {
    if (sym <= 143) {
        bw.writeBits(reverseBits(static_cast<u32>(sym + 0x30), 8), 8);
    } else if (sym <= 255) {
        bw.writeBits(reverseBits(static_cast<u32>(sym - 144 + 0x190), 9), 9);
    } else if (sym <= 279) {
        bw.writeBits(reverseBits(static_cast<u32>(sym - 256), 7), 7);
    } else if (sym <= 287) {
        bw.writeBits(reverseBits(static_cast<u32>(sym - 280 + 0xC0), 8), 8);
    }
}

// Encode a distance code using fixed Huffman (5-bit codes 0-29).
void writeFixedDist(BitWriter& bw, i32 code) {
    bw.writeBits(reverseBits(static_cast<u32>(code), 5), 5);
}

// Find the length code index (0-28) for a given match length (3-258).
i32 lengthToCode(u32 length) {
    for (i32 i = 28; i >= 0; --i) {
        if (length >= LENGTH_TABLE[i].base)
            return i;
    }
    return 0;
}

// Find the distance code (0-29) for a given distance (1-32768).
i32 distanceToCode(u32 dist) {
    for (i32 i = 29; i >= 0; --i) {
        if (dist >= DISTANCE_TABLE[i].base)
            return i;
    }
    return 0;
}

// LZ77 hash chain parameters.
static constexpr u32 HASH_BITS = 15;
static constexpr u32 HASH_SIZE = 1u << HASH_BITS;
static constexpr u32 HASH_MASK = HASH_SIZE - 1;
static constexpr u32 MAX_MATCH = 258;
static constexpr u32 MIN_MATCH = 3;
static constexpr u32 WINDOW_SIZE = 32768;
static constexpr u32 MAX_CHAIN = 128; // Max hash chain length to search.

struct Deflater {
    std::vector<u8> output;
    BitWriter bw;

    void run(std::span<const u8> input) {
        bw.init(&output);

        // Single fixed-Huffman block (BFINAL=1, BTYPE=01).
        bw.writeBits(1, 1); // BFINAL
        bw.writeBits(1, 2); // BTYPE = fixed Huffman

        if (input.empty()) {
            writeFixedLitLen(bw, 256); // End of block.
            bw.flush();
            return;
        }

        // LZ77 with hash chain.
        const u8* src = input.data();
        const size_t srcLen = input.size();

        std::vector<i32> head(HASH_SIZE, -1);
        std::vector<i32> prev(WINDOW_SIZE, -1);

        auto hashFunc = [&](size_t pos) -> u32 {
            if (pos + 2 >= srcLen)
                return 0;
            return ((static_cast<u32>(src[pos]) << 10) ^ (static_cast<u32>(src[pos + 1]) << 5) ^
                    static_cast<u32>(src[pos + 2])) &
                   HASH_MASK;
        };

        size_t pos = 0;
        while (pos < srcLen) {
            u32 bestLen = 0;
            u32 bestDist = 0;

            if (pos + MIN_MATCH <= srcLen) {
                u32 const h = hashFunc(pos);
                i32 chainPos = head[h];
                u32 chainCount = 0;

                while (chainPos >= 0 && chainCount < MAX_CHAIN) {
                    u32 const dist = static_cast<u32>(pos - static_cast<size_t>(chainPos));
                    if (dist > WINDOW_SIZE)
                        break;

                    u32 const maxLen =
                        static_cast<u32>(std::min(static_cast<size_t>(MAX_MATCH), srcLen - pos));
                    // Fast prefix check: skip chain entries that don't match first 4 bytes.
                    if (maxLen >= 4 && std::memcmp(src + pos, src + chainPos, 4) != 0) {
                        chainPos = prev[static_cast<size_t>(chainPos) & (WINDOW_SIZE - 1)];
                        ++chainCount;
                        continue;
                    }
                    u32 len = 0;
                    // Compare in 8-byte chunks where possible.
                    while (len + 8 <= maxLen) {
                        u64 a, b;
                        std::memcpy(&a, src + pos + len, 8);
                        std::memcpy(&b, src + chainPos + len, 8);
                        if (a != b)
                            break;
                        len += 8;
                    }
                    // Finish remaining bytes.
                    while (len < maxLen && src[pos + len] == src[chainPos + len]) {
                        ++len;
                    }
                    if (len >= MIN_MATCH && len > bestLen) {
                        bestLen = len;
                        bestDist = dist;
                        if (len == MAX_MATCH)
                            break;
                    }

                    chainPos = prev[static_cast<size_t>(chainPos) & (WINDOW_SIZE - 1)];
                    ++chainCount;
                }

                prev[pos & (WINDOW_SIZE - 1)] = head[h];
                head[h] = static_cast<i32>(pos);
            }

            if (bestLen >= MIN_MATCH) {
                i32 const lenCode = lengthToCode(bestLen);
                writeFixedLitLen(bw, 257 + lenCode);
                if (LENGTH_TABLE[lenCode].extraBits > 0) {
                    bw.writeBits(bestLen - LENGTH_TABLE[lenCode].base,
                                 LENGTH_TABLE[lenCode].extraBits);
                }

                i32 const distCode = distanceToCode(bestDist);
                writeFixedDist(bw, distCode);
                if (DISTANCE_TABLE[distCode].extraBits > 0) {
                    bw.writeBits(bestDist - DISTANCE_TABLE[distCode].base,
                                 DISTANCE_TABLE[distCode].extraBits);
                }

                for (u32 i = 1; i < bestLen; ++i) {
                    size_t const p = pos + i;
                    if (p + MIN_MATCH <= srcLen) {
                        u32 const h = hashFunc(p);
                        prev[p & (WINDOW_SIZE - 1)] = head[h];
                        head[h] = static_cast<i32>(p);
                    }
                }
                pos += bestLen;
            } else {
                writeFixedLitLen(bw, src[pos]);
                ++pos;
            }
        }

        writeFixedLitLen(bw, 256); // End of block.
        bw.flush();
    }
};

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

std::vector<u8> zlib_decompress(std::span<const u8> data, std::string* out_error,
                                size_t expectedSize) {
    if (data.size() < 6) {
        if (out_error)
            *out_error = "Zlib data too short";
        return {};
    }

    // Parse zlib header (RFC 1950).
    u8 const cmf = data[0];
    u8 const flg = data[1];
    if (((static_cast<u16>(cmf) << 8) | flg) % 31 != 0) {
        if (out_error)
            *out_error = "Invalid zlib header checksum";
        return {};
    }
    u8 const cm = cmf & 0x0F;
    if (cm != 8) {
        if (out_error)
            *out_error = "Unsupported zlib compression method";
        return {};
    }
    bool const hasDict = (flg & 0x20) != 0;
    if (hasDict) {
        if (out_error)
            *out_error = "Preset dictionaries not supported";
        return {};
    }

    // DEFLATE data starts at byte 2.
    size_t const deflateStart = 2;
    size_t const deflateEnd = data.size() - 4; // Last 4 bytes are Adler-32.

    Inflater inflater;
    inflater.errorOut = out_error;
    // Pre-size to expectedSize when known, so the inner loop rarely re-allocates.
    size_t initSize = expectedSize > 0 ? expectedSize : data.size() * 4;
    if (initSize < 4096) initSize = 4096;
    inflater.output.resize(initSize);

    if (!inflater.run(data.data() + deflateStart, deflateEnd - deflateStart)) {
        return {};
    }

    // Trim to actual decompressed size.
    inflater.output.resize(inflater.outPos);

    // Verify Adler-32.
    u32 const stored = (static_cast<u32>(data[data.size() - 4]) << 24) |
                       (static_cast<u32>(data[data.size() - 3]) << 16) |
                       (static_cast<u32>(data[data.size() - 2]) << 8) |
                       static_cast<u32>(data[data.size() - 1]);
    u32 const computed = adler32(inflater.output);
    if (stored != computed) {
        if (out_error)
            *out_error = "Adler-32 checksum mismatch";
        return {};
    }

    return std::move(inflater.output);
}

std::vector<u8> zlib_compress(std::span<const u8> data, std::string* /*out_error*/) {
    Deflater deflater;
    deflater.run(data);

    // Build zlib wrapper.
    std::vector<u8> result;
    result.reserve(deflater.output.size() + 6);

    // Zlib header: CMF=0x78 (CM=8/deflate, CINFO=7/32K window), FLG=0x01 (check bits).
    // (0x78 << 8 | 0x01) = 30721, 30721 % 31 = 0  ✓
    result.push_back(0x78);
    result.push_back(0x01);

    // DEFLATE data.
    result.insert(result.end(), deflater.output.begin(), deflater.output.end());

    // Adler-32 checksum (big-endian).
    u32 const check = adler32(data);
    result.push_back(static_cast<u8>((check >> 24) & 0xFF));
    result.push_back(static_cast<u8>((check >> 16) & 0xFF));
    result.push_back(static_cast<u8>((check >> 8) & 0xFF));
    result.push_back(static_cast<u8>(check & 0xFF));

    return result;
}

} // namespace whiteout
