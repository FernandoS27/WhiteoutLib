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

#include "../../common/bit_io.h"
#include "../../common/huffman_table.h"
#include "../../common/checksum.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace whiteout::textures::png {

// Aliases for the common types to keep the rest of the file unchanged.
using BitReader = ::whiteout::LsbBitReader;
using BitWriter = ::whiteout::LsbBitWriter;
using HuffTable = ::whiteout::LsbHuffmanTable;

namespace {

// Adler-32 lives in src/whiteout/common/checksum.h.
using ::whiteout::adler32;

// ============================================================================
// Huffman Table Builders for DEFLATE Fixed Codes
// ============================================================================

HuffTable buildFixedLitLenTable() {
    std::array<u8, 288> lengths{};
    for (i32 i = 0; i <= 143; ++i) lengths[i] = 8;
    for (i32 i = 144; i <= 255; ++i) lengths[i] = 9;
    for (i32 i = 256; i <= 279; ++i) lengths[i] = 7;
    for (i32 i = 280; i <= 287; ++i) lengths[i] = 8;
    HuffTable t;
    t.build(lengths.data(), 288);
    return t;
}

HuffTable buildFixedDistTable() {
    std::array<u8, 32> lengths{};
    for (i32 i = 0; i < 32; ++i) lengths[i] = 5;
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
    {3, 0},   {4, 0},   {5, 0},   {6, 0},   {7, 0},   {8, 0},   {9, 0},
    {10, 0},  {11, 1},  {13, 1},  {15, 1},  {17, 1},  {19, 2},  {23, 2},
    {27, 2},  {31, 2},  {35, 3},  {43, 3},  {51, 3},  {59, 3},  {67, 4},
    {83, 4},  {99, 4},  {115, 4}, {131, 5}, {163, 5}, {195, 5}, {227, 5},
    {258, 0},
}};

// Distance codes 0-29.
static constexpr std::array<LenDistEntry, 30> DISTANCE_TABLE = {{
    {1, 0},     {2, 0},     {3, 0},     {4, 0},     {5, 1},     {7, 1},
    {9, 2},     {13, 2},    {17, 3},    {25, 3},    {33, 4},    {49, 4},
    {65, 5},    {97, 5},    {129, 6},   {193, 6},   {257, 7},   {385, 7},
    {513, 8},   {769, 8},   {1025, 9},  {1537, 9},  {2049, 10}, {3073, 10},
    {4097, 11}, {6145, 11}, {8193, 12}, {12289, 12},{16385, 13},{24577, 13},
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
    std::string* errorOut;

    bool error(const char* msg) {
        if (errorOut) *errorOut = msg;
        return false;
    }

    bool inflateStored() {
        br.alignToByte();
        if (br.bytePos + 4 > br.size) return error("Truncated stored block header");
        // Read from the byte stream directly (bit buffer was aligned).
        // But we may have bytes still in the bit buffer after alignment.
        // After alignToByte, bitsAvail is a multiple of 8 that was already consumed.
        // Actually we need to read LEN/NLEN from the bitstream post-alignment.
        u32 len = br.readBits(16);
        u32 nlen = br.readBits(16);
        if ((len ^ nlen) != 0xFFFF) return error("Stored block LEN/NLEN mismatch");
        // Now read `len` raw bytes. Since we aligned, remaining bits are gone.
        // But readBits consumed them from the buffer, so now bytePos is advanced.
        // We need to pull bytes. After reading 32 bits from an aligned stream,
        // bytePos has been advanced by refill. We should just read from the bit
        // reader byte-by-byte.
        for (u32 i = 0; i < len; ++i) {
            if (br.bytePos >= br.size && br.bitsAvail < 8)
                return error("Truncated stored block data");
            output.push_back(static_cast<u8>(br.readBits(8)));
        }
        return true;
    }

    bool inflateBlock(const HuffTable& litLen, const HuffTable& dist) {
        for (;;) {
            i32 sym = litLen.decode(br);
            if (sym < 0) return error("Invalid literal/length code");
            if (sym < 256) {
                output.push_back(static_cast<u8>(sym));
            } else if (sym == 256) {
                return true; // End of block.
            } else {
                // Length code.
                i32 lenIdx = sym - 257;
                if (lenIdx < 0 || lenIdx >= 29) return error("Invalid length code");
                u32 length = LENGTH_TABLE[lenIdx].base;
                if (LENGTH_TABLE[lenIdx].extraBits > 0) {
                    length += br.readBits(LENGTH_TABLE[lenIdx].extraBits);
                }

                // Distance code.
                i32 distSym = dist.decode(br);
                if (distSym < 0 || distSym >= 30) return error("Invalid distance code");
                u32 distance = DISTANCE_TABLE[distSym].base;
                if (DISTANCE_TABLE[distSym].extraBits > 0) {
                    distance += br.readBits(DISTANCE_TABLE[distSym].extraBits);
                }

                if (distance > output.size()) return error("Distance exceeds output buffer");

                // Copy from back-reference.
                size_t srcPos = output.size() - distance;
                for (u32 i = 0; i < length; ++i) {
                    output.push_back(output[srcPos + i]);
                }
            }
        }
    }

    bool inflateDynamic() {
        u32 hlit = br.readBits(5) + 257;
        u32 hdist = br.readBits(5) + 1;
        u32 hclen = br.readBits(4) + 4;

        if (hlit > 286 || hdist > 30) return error("Invalid dynamic Huffman header");

        // Read code length code lengths.
        std::array<u8, 19> clCodeLengths{};
        for (u32 i = 0; i < hclen; ++i) {
            clCodeLengths[CL_CODE_ORDER[i]] = static_cast<u8>(br.readBits(3));
        }

        HuffTable clTable;
        if (!clTable.build(clCodeLengths.data(), 19))
            return error("Failed to build code length Huffman table");

        // Decode literal/length + distance code lengths.
        u32 totalCodes = hlit + hdist;
        std::vector<u8> codeLengths(totalCodes, 0);
        u32 idx = 0;
        while (idx < totalCodes) {
            i32 sym = clTable.decode(br);
            if (sym < 0) return error("Invalid code length symbol");

            if (sym < 16) {
                codeLengths[idx++] = static_cast<u8>(sym);
            } else if (sym == 16) {
                if (idx == 0) return error("Repeat code with no previous length");
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
                return error("Unknown code length symbol");
            }
        }

        HuffTable litLenTable, distTable;
        if (!litLenTable.build(codeLengths.data(), static_cast<i32>(hlit)))
            return error("Failed to build dynamic literal/length table");

        // Build distance table. If hdist == 1 and the only length is 0, there
        // are no distance codes (literal-only block). Build a dummy table.
        if (!distTable.build(codeLengths.data() + hlit, static_cast<i32>(hdist)))
            return error("Failed to build dynamic distance table");

        return inflateBlock(litLenTable, distTable);
    }

    bool run(const u8* data, size_t size) {
        br.init(data, size, 0);

        bool isFinal = false;
        while (!isFinal) {
            isFinal = br.readBits(1) != 0;
            u32 btype = br.readBits(2);

            if (btype == 0) {
                if (!inflateStored()) return false;
            } else if (btype == 1) {
                static const HuffTable fixedLitLen = buildFixedLitLenTable();
                static const HuffTable fixedDist = buildFixedDistTable();
                if (!inflateBlock(fixedLitLen, fixedDist)) return false;
            } else if (btype == 2) {
                if (!inflateDynamic()) return false;
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
        // 0b00110000 .. 0b10111111 → 8 bits, codes 0x30-0xBF
        bw.writeBits(reverseBits(static_cast<u32>(sym + 0x30), 8), 8);
    } else if (sym <= 255) {
        // 0b110010000 .. 0b111111111 → 9 bits, codes 0x190-0x1FF
        bw.writeBits(reverseBits(static_cast<u32>(sym - 144 + 0x190), 9), 9);
    } else if (sym <= 279) {
        // 0b0000000 .. 0b0010111 → 7 bits, codes 0x00-0x17
        bw.writeBits(reverseBits(static_cast<u32>(sym - 256), 7), 7);
    } else if (sym <= 287) {
        // 0b11000000 .. 0b11000111 → 8 bits, codes 0xC0-0xC7
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
        if (length >= LENGTH_TABLE[i].base) return i;
    }
    return 0;
}

// Find the distance code (0-29) for a given distance (1-32768).
i32 distanceToCode(u32 dist) {
    for (i32 i = 29; i >= 0; --i) {
        if (dist >= DISTANCE_TABLE[i].base) return i;
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

        // Hash table: head[hash] = most recent position with that hash.
        // prev[pos & (WINDOW_SIZE-1)] = previous position in chain.
        std::vector<i32> head(HASH_SIZE, -1);
        std::vector<i32> prev(WINDOW_SIZE, -1);

        auto hashFunc = [&](size_t pos) -> u32 {
            if (pos + 2 >= srcLen) return 0;
            return ((static_cast<u32>(src[pos]) << 10) ^
                    (static_cast<u32>(src[pos + 1]) << 5) ^
                    static_cast<u32>(src[pos + 2])) &
                   HASH_MASK;
        };

        size_t pos = 0;
        while (pos < srcLen) {
            u32 bestLen = 0;
            u32 bestDist = 0;

            if (pos + MIN_MATCH <= srcLen) {
                u32 h = hashFunc(pos);
                i32 chainPos = head[h];
                u32 chainCount = 0;

                while (chainPos >= 0 && chainCount < MAX_CHAIN) {
                    u32 dist = static_cast<u32>(pos - static_cast<size_t>(chainPos));
                    if (dist > WINDOW_SIZE) break;

                    // Compare.
                    u32 maxLen = static_cast<u32>(std::min(
                        static_cast<size_t>(MAX_MATCH), srcLen - pos));
                    u32 len = 0;
                    while (len < maxLen && src[pos + len] == src[chainPos + len]) {
                        ++len;
                    }
                    if (len >= MIN_MATCH && len > bestLen) {
                        bestLen = len;
                        bestDist = dist;
                        if (len == MAX_MATCH) break;
                    }

                    chainPos = prev[static_cast<size_t>(chainPos) & (WINDOW_SIZE - 1)];
                    ++chainCount;
                }

                // Insert current position into hash chain.
                prev[pos & (WINDOW_SIZE - 1)] = head[h];
                head[h] = static_cast<i32>(pos);
            }

            if (bestLen >= MIN_MATCH) {
                // Emit length/distance pair.
                i32 lenCode = lengthToCode(bestLen);
                writeFixedLitLen(bw, 257 + lenCode);
                if (LENGTH_TABLE[lenCode].extraBits > 0) {
                    bw.writeBits(bestLen - LENGTH_TABLE[lenCode].base,
                                 LENGTH_TABLE[lenCode].extraBits);
                }

                i32 distCode = distanceToCode(bestDist);
                writeFixedDist(bw, distCode);
                if (DISTANCE_TABLE[distCode].extraBits > 0) {
                    bw.writeBits(bestDist - DISTANCE_TABLE[distCode].base,
                                 DISTANCE_TABLE[distCode].extraBits);
                }

                // Insert skipped positions into the hash chain.
                for (u32 i = 1; i < bestLen; ++i) {
                    size_t p = pos + i;
                    if (p + MIN_MATCH <= srcLen) {
                        u32 h = hashFunc(p);
                        prev[p & (WINDOW_SIZE - 1)] = head[h];
                        head[h] = static_cast<i32>(p);
                    }
                }
                pos += bestLen;
            } else {
                // Emit literal.
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

std::vector<u8> zlib_decompress(std::span<const u8> data, std::string* out_error) {
    if (data.size() < 6) {
        if (out_error) *out_error = "Zlib data too short";
        return {};
    }

    // Parse zlib header (RFC 1950).
    u8 cmf = data[0];
    u8 flg = data[1];
    if (((static_cast<u16>(cmf) << 8) | flg) % 31 != 0) {
        if (out_error) *out_error = "Invalid zlib header checksum";
        return {};
    }
    u8 cm = cmf & 0x0F;
    if (cm != 8) {
        if (out_error) *out_error = "Unsupported zlib compression method";
        return {};
    }
    bool hasDict = (flg & 0x20) != 0;
    if (hasDict) {
        if (out_error) *out_error = "Preset dictionaries not supported";
        return {};
    }

    // DEFLATE data starts at byte 2.
    size_t deflateStart = 2;
    size_t deflateEnd = data.size() - 4; // Last 4 bytes are Adler-32.

    Inflater inflater;
    inflater.errorOut = out_error;
    inflater.output.reserve(data.size() * 4); // Rough estimate.

    if (!inflater.run(data.data() + deflateStart, deflateEnd - deflateStart)) {
        return {};
    }

    // Verify Adler-32.
    u32 stored = (static_cast<u32>(data[data.size() - 4]) << 24) |
                 (static_cast<u32>(data[data.size() - 3]) << 16) |
                 (static_cast<u32>(data[data.size() - 2]) << 8) |
                 static_cast<u32>(data[data.size() - 1]);
    u32 computed = adler32(inflater.output);
    if (stored != computed) {
        if (out_error) *out_error = "Adler-32 checksum mismatch";
        return {};
    }

    return std::move(inflater.output);
}

std::vector<u8> zlib_compress(std::span<const u8> data, std::string* out_error) {
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
    u32 check = adler32(data);
    result.push_back(static_cast<u8>((check >> 24) & 0xFF));
    result.push_back(static_cast<u8>((check >> 16) & 0xFF));
    result.push_back(static_cast<u8>((check >> 8) & 0xFF));
    result.push_back(static_cast<u8>(check & 0xFF));

    return result;
}

} // namespace whiteout::textures::png
