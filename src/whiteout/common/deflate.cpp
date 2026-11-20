// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file deflate.cpp
/// @brief Minimal zlib-wrapped DEFLATE inflate/deflate implementation.
///
/// Implements RFC 1950 (zlib wrapper) + RFC 1951 (DEFLATE) with:
///   - Inflate: stored, fixed Huffman, and dynamic Huffman blocks.
///   - Deflate: LZ77 with hash chains and lazy matching; each block stored,
///     fixed Huffman or dynamic Huffman, whichever is smallest.
///
/// No external dependencies (no zlib).

#include "deflate.h"

#include "bit_io.h"
#include "checksum.h"
#include "huffman_table.h"
#include "output_reserve.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace whiteout {

using BitReader = LsbBitReader;
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
            if (newCap < needed)
                newCap = needed;
            if (newCap < 4096)
                newCap = 4096;
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
        if (br.bitsAvail < 0)
            return error("Truncated stored block header");
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
            // Bits past the end read as zeros; a stream that needs them is
            // truncated, and would otherwise decode zeros without end.
            if (bitsLeft < 0)
                return error("Truncated DEFLATE stream");

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
                        if (chunk > length - copied)
                            chunk = length - copied;
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
        if (br.bitsAvail < 0)
            return error("Truncated dynamic Huffman header");

        return inflateBlock(litLenTable, distTable);
    }

    bool run(const u8* data, size_t size) {
        br.init(data, size, 0);

        bool isFinal = false;
        while (!isFinal) {
            isFinal = br.readBits(1) != 0;
            u32 const btype = br.readBits(2);
            if (br.bitsAvail < 0)
                return error("Truncated DEFLATE stream");

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
// Deflate Implementation (LZ77 with lazy matching + stored/fixed/dynamic blocks)
// ============================================================================

constexpr u32 kMinMatch = 3;
constexpr u32 kMaxMatch = 258;
constexpr u32 kWindowSize = 32768;
// How hard a match is looked for: zlib's level 6.
constexpr u32 kMaxChain = 128;   ///< Candidates tried per position.
constexpr u32 kGoodLength = 8;   ///< With a match this long in hand, a quarter of them.
constexpr u32 kMaxLazy = 16;     ///< A match shorter than this may lose to one a byte later.
constexpr u32 kNiceLength = 128; ///< A match this long ends the search.
constexpr u32 kTooFar = 4096;    ///< A 3-byte match from further back costs more than it saves.
/// A block ends after this many symbols, so its codes fit what it holds.
constexpr size_t kBlockSymbols = 16384;
constexpr u32 kEndOfBlock = 256;
constexpr u32 kLitLenCodes = 286;
constexpr u32 kDistCodes = 30;
constexpr u32 kCodeLenCodes = 19;
constexpr u32 kMaxCodeBits = 15;
constexpr u32 kMaxCodeLenBits = 7;
/// The match finder keeps positions in 32 bits: a longer input is stored.
constexpr u64 kMaxSearchable = 0xFFFF0000ull;

/// A match length's code (0-28) and a distance's code (0-29), looked up.
struct SymbolCodes {
    std::array<u8, kMaxMatch + 1> length{};
    std::array<u8, 512> distance{}; ///< By distance - 1 up to 256, then by (distance - 1) >> 7.
};
constexpr SymbolCodes buildSymbolCodes() {
    SymbolCodes codes;
    for (u32 code = 0; code < LENGTH_TABLE.size(); ++code)
        for (u32 length = LENGTH_TABLE[code].base; length <= kMaxMatch; ++length)
            codes.length[length] = static_cast<u8>(code);
    for (u32 code = 0; code < DISTANCE_TABLE.size(); ++code) {
        for (u32 d = DISTANCE_TABLE[code].base - 1; d < 256; ++d)
            codes.distance[d] = static_cast<u8>(code);
        for (u32 d = (DISTANCE_TABLE[code].base - 1) >> 7; d < 256; ++d)
            if ((d << 7) + 1 >= DISTANCE_TABLE[code].base)
                codes.distance[256 + d] = static_cast<u8>(code);
    }
    return codes;
}
constexpr SymbolCodes kSymbolCodes = buildSymbolCodes();

inline u32 distanceCode(u32 distance) {
    u32 const d = distance - 1;
    return d < 256 ? kSymbolCodes.distance[d] : kSymbolCodes.distance[256 + (d >> 7)];
}

/// A prefix code as the stream carries it: each symbol's bit length, and its
/// code with the bits reversed, since DEFLATE sends a code high bit first.
template <size_t N>
struct PrefixCode {
    std::array<u8, N> bits{};
    std::array<u16, N> code{};

    /// The canonical codes for the lengths in @p bits (RFC 1951 3.2.2).
    void assignCodes() {
        std::array<u16, kMaxCodeBits + 2> next{};
        std::array<u16, kMaxCodeBits + 1> count{};
        for (u8 const length : bits)
            ++count[length];
        count[0] = 0;
        u32 value = 0;
        for (u32 length = 1; length <= kMaxCodeBits; ++length) {
            value = (value + count[length - 1]) << 1;
            next[length] = static_cast<u16>(value);
        }
        for (size_t symbol = 0; symbol < N; ++symbol) {
            u32 const length = bits[symbol];
            if (length == 0)
                continue;
            u32 canonical = next[length]++;
            u32 reversed = 0;
            for (u32 bit = 0; bit < length; ++bit, canonical >>= 1)
                reversed = (reversed << 1) | (canonical & 1);
            code[symbol] = static_cast<u16>(reversed);
        }
    }
};

/// Bit lengths of a prefix code for @p count symbols used @p frequency
/// times each, none longer than @p maxBits: a Huffman code, its longest
/// codes shortened to the limit when it has any. A symbol never used gets no
/// code. At least two symbols get one, as zlib sees to: some inflaters take
/// no code of a single symbol.
void codeLengths(const u32* frequency, u32 count, u32 maxBits, u8* bits) {
    struct Node {
        u32 weight;
        u16 symbol;
    };
    std::array<Node, kLitLenCodes> leaves;
    u32 used = 0;
    for (u32 symbol = 0; symbol < count; ++symbol) {
        bits[symbol] = 0;
        if (frequency[symbol] != 0)
            leaves[used++] = {frequency[symbol], static_cast<u16>(symbol)};
    }
    for (u32 symbol = 0; used < 2 && symbol < count; ++symbol) {
        if (frequency[symbol] == 0)
            leaves[used++] = {1, static_cast<u16>(symbol)};
    }
    std::sort(leaves.begin(), leaves.begin() + used, [](const Node& a, const Node& b) {
        return a.weight != b.weight ? a.weight < b.weight : a.symbol < b.symbol;
    });

    // Huffman's algorithm over two queues: the leaves, lightest first, and
    // the branches in the order they are made, which is lightest first too.
    // A node's parent is always made after it.
    std::array<u32, kLitLenCodes * 2> weight;
    std::array<u16, kLitLenCodes * 2> parent;
    for (u32 i = 0; i < used; ++i)
        weight[i] = leaves[i].weight;
    u32 leaf = 0;
    u32 branch = used;
    u32 made = used;
    auto lightest = [&] {
        if (leaf < used && (branch >= made || weight[leaf] <= weight[branch]))
            return leaf++;
        return branch++;
    };
    while (made < used * 2 - 1) {
        u32 const a = lightest();
        u32 const b = lightest();
        weight[made] = weight[a] + weight[b];
        parent[a] = parent[b] = static_cast<u16>(made);
        ++made;
    }

    // Each leaf's depth, counted from the root down, and how many codes
    // there are of each length; lengths past the limit are cut to it.
    std::array<u8, kLitLenCodes * 2> depth;
    std::array<u32, kMaxCodeBits + 1> perLength{};
    depth[made - 1] = 0;
    for (u32 node = made - 1; node-- > 0;) {
        depth[node] = static_cast<u8>(std::min<u32>(depth[parent[node]] + 1, 255));
        if (node < used)
            ++perLength[std::min<u32>(depth[node], maxBits)];
    }
    // Cutting them made the code over-full: codes at the limit are paired
    // off under a shorter code that is lengthened by one, until it fits.
    u32 space = 0;
    for (u32 length = 1; length <= maxBits; ++length)
        space += perLength[length] << (maxBits - length);
    while (space > (1u << maxBits)) {
        --perLength[maxBits];
        for (u32 length = maxBits - 1; length > 0; --length) {
            if (perLength[length] != 0) {
                --perLength[length];
                perLength[length + 1] += 2;
                break;
            }
        }
        --space;
    }
    // The rarest symbols take the longest codes.
    u32 next = 0;
    for (u32 length = maxBits; length > 0; --length)
        for (u32 n = perLength[length]; n > 0; --n)
            bits[leaves[next++].symbol] = static_cast<u8>(length);
}

/// The fixed code of RFC 1951 3.2.6.
struct FixedCodes {
    PrefixCode<kLitLenCodes + 2> litLen;
    PrefixCode<kDistCodes + 2> distance;
    FixedCodes() {
        for (u32 symbol = 0; symbol < litLen.bits.size(); ++symbol)
            litLen.bits[symbol] = symbol <= 143 ? 8 : symbol <= 255 ? 9 : symbol <= 279 ? 7 : 8;
        distance.bits.fill(5);
        litLen.assignCodes();
        distance.assignCodes();
    }
};
const FixedCodes& fixedCodes() {
    static const FixedCodes codes;
    return codes;
}

/// What a deflate borrows: the match finder's tables and the symbols of the
/// block being gathered. Kept per thread, so a 4 KiB input costs no
/// allocation and clears only the part of the tables it uses.
struct DeflateWorkspace {
    std::vector<u32> head; ///< By hash: the last position with it, plus one; 0 for none.
    std::vector<u32> prev; ///< By position in the window: the one before it with the same hash, plus one.
    std::vector<u32> symbols;
};

class Deflater {
public:
    /// @p out receives the stream; it must hold bound(input.size()) bytes.
    Deflater(std::span<const u8> input, u8* out, DeflateWorkspace& workspace)
        : m_src(input.data()), m_size(input.size()), m_out(out), m_work(workspace) {}

    /// The most a stream of @p size input bytes can come to, with room for
    /// the bit writer to write a word at a time.
    [[nodiscard]] static size_t bound(size_t size) {
        // No block comes to more than it would stored: 5 bytes to each
        // 65,535 of them, and a block ends every kBlockSymbols symbols.
        return size + 8 * (size / kBlockSymbols + 2) + 16;
    }

    /// Writes the whole stream; returns its end.
    u8* run() {
        if (m_size == 0) {
            // One empty fixed block.
            put(1, 1);
            put(1, 2);
            put(fixedCodes().litLen.code[kEndOfBlock], fixedCodes().litLen.bits[kEndOfBlock]);
            return finish();
        }
        if (m_size > kMaxSearchable) {
            stored(m_size, true);
            return finish();
        }
        prepare();

        size_t pos = 0;
        size_t inserted = 0; // The first position not yet in the hash chains.
        while (pos < m_size) {
            Match match = find(pos, 0, inserted);
            if (match.length == 0) {
                literal(m_src[pos]);
                ++pos;
            } else {
                // A longer match a byte on is worth this byte as a literal.
                while (match.length < kMaxLazy && pos + 1 < m_size) {
                    Match const later = find(pos + 1, match.length, inserted);
                    if (later.length == 0)
                        break;
                    literal(m_src[pos]);
                    ++pos;
                    match = later;
                }
                copy(match);
                for (size_t p = inserted; p < pos + match.length; ++p)
                    insert(p);
                inserted = std::max(inserted, pos + match.length);
                pos += match.length;
            }
            if (m_work.symbols.size() >= kBlockSymbols)
                endBlock(pos, pos == m_size);
        }
        if (!m_lastBlockWritten)
            endBlock(m_size, true);
        return finish();
    }

private:
    struct Match {
        u32 length = 0;
        u32 distance = 0;
    };

    // ---- the match finder

    void prepare() {
        // The hash table is sized to the input: a small one is quick to
        // clear and stays in cache.
        m_hashBits = 8;
        while (m_hashBits < 15 && (size_t{1} << m_hashBits) < m_size)
            ++m_hashBits;
        size_t const window = std::min<size_t>(kWindowSize, std::bit_ceil(m_size));
        m_windowMask = static_cast<u32>(window - 1);
        if (m_work.head.size() < (size_t{1} << m_hashBits))
            m_work.head.resize(size_t{1} << m_hashBits);
        if (m_work.prev.size() < window)
            m_work.prev.resize(window);
        std::memset(m_work.head.data(), 0, (size_t{1} << m_hashBits) * sizeof(u32));
        m_work.symbols.clear();
        m_work.symbols.reserve(kBlockSymbols + kMaxLazy + 2); // A block may run a few symbols over.
    }

    [[nodiscard]] u32 hashAt(size_t pos) const {
        u32 const three = static_cast<u32>(m_src[pos]) | (static_cast<u32>(m_src[pos + 1]) << 8) |
                          (static_cast<u32>(m_src[pos + 2]) << 16);
        return (three * 0x9E3779B1u) >> (32 - m_hashBits);
    }

    /// Puts @p pos at the head of its hash chain; returns what was there.
    u32 insert(size_t pos) {
        if (m_size - pos < kMinMatch)
            return 0;
        u32 const hash = hashAt(pos);
        u32 const before = m_work.head[hash];
        m_work.prev[pos & m_windowMask] = before;
        m_work.head[hash] = static_cast<u32>(pos) + 1;
        return before;
    }

    /// The longest match at @p pos that beats @p held bytes, or none. Enters
    /// @p pos into the chains; @p inserted follows.
    Match find(size_t pos, u32 held, size_t& inserted) {
        u32 candidate = 0;
        if (pos >= inserted) {
            candidate = insert(pos);
            inserted = pos + 1;
        }
        size_t const maxLength = std::min<size_t>(kMaxMatch, m_size - pos);
        if (maxLength < kMinMatch)
            return {};

        const u8* const here = m_src + pos;
        u32 best = std::max(held, kMinMatch - 1);
        if (best >= maxLength)
            return {};
        Match match;
        u32 chain = held >= kGoodLength ? kMaxChain / 4 : kMaxChain;
        while (candidate != 0 && chain-- != 0) {
            size_t const at = candidate - 1;
            size_t const distance = pos - at;
            if (distance > kWindowSize)
                break;
            const u8* const there = m_src + at;
            // The byte that would make it longer, then the first two.
            if (there[best] == here[best] && there[0] == here[0] && there[1] == here[1]) {
                u32 const length = matchLength(here, there, static_cast<u32>(maxLength));
                if (length > best) {
                    best = length;
                    match = {length, static_cast<u32>(distance)};
                    if (length >= kNiceLength || length == maxLength)
                        break;
                }
            }
            u32 const next = m_work.prev[at & m_windowMask];
            if (next >= candidate) // The slot was reused by a later position.
                break;
            candidate = next;
        }
        if (match.length == kMinMatch && match.distance > kTooFar)
            return {};
        return match;
    }

    static u32 matchLength(const u8* a, const u8* b, u32 maxLength) {
        u32 length = 0;
        if constexpr (std::endian::native == std::endian::little) {
            while (length + 8 <= maxLength) {
                u64 x = 0;
                u64 y = 0;
                std::memcpy(&x, a + length, 8);
                std::memcpy(&y, b + length, 8);
                if (x != y)
                    return length + (static_cast<u32>(std::countr_zero(x ^ y)) >> 3);
                length += 8;
            }
        }
        while (length < maxLength && a[length] == b[length])
            ++length;
        return length;
    }

    // ---- the symbols of a block

    void literal(u8 byte) {
        m_work.symbols.push_back(byte);
        ++m_litLenCount[byte];
    }

    void copy(Match match) {
        m_work.symbols.push_back((match.distance << 16) | (match.length - kMinMatch));
        ++m_litLenCount[257 + kSymbolCodes.length[match.length]];
        ++m_distanceCount[distanceCode(match.distance)];
    }

    /// Writes the symbols gathered since the last block, which stand for
    /// the input up to @p end, in whichever of the three block forms is
    /// smallest.
    void endBlock(size_t end, bool last) {
        ++m_litLenCount[kEndOfBlock];
        PrefixCode<kLitLenCodes + 2> litLen;
        PrefixCode<kDistCodes + 2> distance;
        codeLengths(m_litLenCount.data(), kLitLenCodes, kMaxCodeBits, litLen.bits.data());
        codeLengths(m_distanceCount.data(), kDistCodes, kMaxCodeBits, distance.bits.data());

        // The code lengths themselves, run-length coded and then coded.
        u32 litLenUsed = kLitLenCodes;
        while (litLenUsed > 257 && litLen.bits[litLenUsed - 1] == 0)
            --litLenUsed;
        u32 distanceUsed = kDistCodes;
        while (distanceUsed > 1 && distance.bits[distanceUsed - 1] == 0)
            --distanceUsed;
        std::array<u8, kLitLenCodes + kDistCodes> lengths;
        std::copy_n(litLen.bits.begin(), litLenUsed, lengths.begin());
        std::copy_n(distance.bits.begin(), distanceUsed, lengths.begin() + litLenUsed);
        struct Run {
            u8 symbol;
            u8 extra;
        };
        std::array<Run, kLitLenCodes + kDistCodes> runs;
        u32 runCount = 0;
        std::array<u32, kCodeLenCodes> codeLenCount{};
        u32 const total = litLenUsed + distanceUsed;
        for (u32 i = 0; i < total;) {
            u8 const length = lengths[i];
            u32 same = 1;
            while (i + same < total && lengths[i + same] == length)
                ++same;
            i += same;
            if (length == 0) {
                for (; same >= 11; ) {
                    u32 const n = std::min<u32>(same, 138);
                    runs[runCount++] = {18, static_cast<u8>(n - 11)};
                    ++codeLenCount[18];
                    same -= n;
                }
                if (same >= 3) {
                    runs[runCount++] = {17, static_cast<u8>(same - 3)};
                    ++codeLenCount[17];
                    same = 0;
                }
            } else {
                // The length once, then repeats of it three to six at a time.
                runs[runCount++] = {length, 0};
                ++codeLenCount[length];
                --same;
                for (; same >= 3; ) {
                    u32 const n = std::min<u32>(same, 6);
                    runs[runCount++] = {16, static_cast<u8>(n - 3)};
                    ++codeLenCount[16];
                    same -= n;
                }
            }
            for (; same > 0; --same) {
                runs[runCount++] = {length, 0};
                ++codeLenCount[length];
            }
        }
        PrefixCode<kCodeLenCodes> codeLen;
        codeLengths(codeLenCount.data(), kCodeLenCodes, kMaxCodeLenBits, codeLen.bits.data());
        u32 codeLenUsed = kCodeLenCodes;
        while (codeLenUsed > 4 && codeLen.bits[static_cast<size_t>(CL_CODE_ORDER[codeLenUsed - 1])] == 0)
            --codeLenUsed;

        // What each form would cost, in bits.
        static constexpr std::array<u8, kCodeLenCodes> kCodeLenExtra = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                                        0, 0, 0, 0, 0, 0, 2, 3, 7};
        const FixedCodes& fixed = fixedCodes();
        u64 extraBits = 0;
        u64 dynamicBits = 0;
        u64 fixedBits = 0;
        for (u32 symbol = 0; symbol < kLitLenCodes; ++symbol) {
            u64 const count = m_litLenCount[symbol];
            dynamicBits += count * litLen.bits[symbol];
            fixedBits += count * fixed.litLen.bits[symbol];
            if (symbol > kEndOfBlock)
                extraBits += count * LENGTH_TABLE[symbol - 257].extraBits;
        }
        for (u32 symbol = 0; symbol < kDistCodes; ++symbol) {
            u64 const count = m_distanceCount[symbol];
            dynamicBits += count * distance.bits[symbol];
            fixedBits += count * 5;
            extraBits += count * DISTANCE_TABLE[symbol].extraBits;
        }
        dynamicBits += extraBits + 3 + 5 + 5 + 4 + 3 * u64{codeLenUsed};
        for (u32 symbol = 0; symbol < kCodeLenCodes; ++symbol)
            dynamicBits += u64{codeLenCount[symbol]} * (codeLen.bits[symbol] + kCodeLenExtra[symbol]);
        fixedBits += extraBits + 3;
        // Stored: 65,535 bytes to a block, each after 3 header bits, padding
        // to a byte and 4 bytes of length.
        size_t const rawBytes = end - m_blockStart;
        u64 const storedBlocks = std::max<u64>(1, (rawBytes + 65534) / 65535);
        u64 const padding = (8 - (m_bitCount + 3) % 8) % 8;
        u64 const storedBits = u64{rawBytes} * 8 + 35 + padding + (storedBlocks - 1) * 40;

        if (storedBits < dynamicBits && storedBits < fixedBits) {
            stored(end, last);
        } else if (fixedBits <= dynamicBits) {
            put(last ? 1 : 0, 1);
            put(1, 2);
            symbols(fixed.litLen, fixed.distance);
        } else {
            litLen.assignCodes();
            distance.assignCodes();
            codeLen.assignCodes();
            put(last ? 1 : 0, 1);
            put(2, 2);
            put(litLenUsed - 257, 5);
            put(distanceUsed - 1, 5);
            put(codeLenUsed - 4, 4);
            for (u32 i = 0; i < codeLenUsed; ++i)
                put(codeLen.bits[static_cast<size_t>(CL_CODE_ORDER[i])], 3);
            for (u32 i = 0; i < runCount; ++i) {
                put(codeLen.code[runs[i].symbol], codeLen.bits[runs[i].symbol]);
                if (runs[i].symbol >= 16)
                    put(runs[i].extra, kCodeLenExtra[runs[i].symbol]);
            }
            symbols(litLen, distance);
        }

        m_work.symbols.clear();
        m_litLenCount.fill(0);
        m_distanceCount.fill(0);
        m_blockStart = end;
        m_lastBlockWritten = last;
    }

    template <size_t L, size_t D>
    void symbols(const PrefixCode<L>& litLen, const PrefixCode<D>& distance) {
        for (u32 const symbol : m_work.symbols) {
            u32 const far = symbol >> 16;
            if (far == 0) {
                put(litLen.code[symbol], litLen.bits[symbol]);
                continue;
            }
            u32 const length = (symbol & 0xFFFF) + kMinMatch;
            u32 const lengthCode = kSymbolCodes.length[length];
            put(litLen.code[257 + lengthCode], litLen.bits[257 + lengthCode]);
            put(length - LENGTH_TABLE[lengthCode].base, LENGTH_TABLE[lengthCode].extraBits);
            u32 const code = distanceCode(far);
            put(distance.code[code], distance.bits[code]);
            put(far - DISTANCE_TABLE[code].base, DISTANCE_TABLE[code].extraBits);
        }
        put(litLen.code[kEndOfBlock], litLen.bits[kEndOfBlock]);
    }

    /// The input from the block's start to @p end as stored blocks.
    void stored(size_t end, bool last) {
        size_t at = m_blockStart;
        do {
            size_t const length = std::min<size_t>(65535, end - at);
            put(last && at + length == end ? 1 : 0, 1);
            put(0, 2);
            alignToByte();
            put(static_cast<u32>(length), 16);
            put(static_cast<u32>(length) ^ 0xFFFF, 16);
            flushBytes();
            std::memcpy(m_out, m_src + at, length);
            m_out += length;
            at += length;
        } while (at < end);
    }

    // ---- bits, low bit first

    /// @p count (at most 16) bits of @p value.
    void put(u32 value, u32 count) {
        m_bits |= static_cast<u64>(value) << m_bitCount;
        m_bitCount += count;
        if (m_bitCount >= 32) {
            for (u32 i = 0; i < 4; ++i)
                m_out[i] = static_cast<u8>(m_bits >> (8 * i));
            m_out += 4;
            m_bits >>= 32;
            m_bitCount -= 32;
        }
    }
    void alignToByte() {
        m_bitCount = (m_bitCount + 7) & ~7u;
    }
    /// Writes the whole bytes held; the bit count must be a multiple of 8.
    void flushBytes() {
        for (; m_bitCount >= 8; m_bitCount -= 8, m_bits >>= 8)
            *m_out++ = static_cast<u8>(m_bits);
    }
    u8* finish() {
        alignToByte();
        flushBytes();
        return m_out;
    }

    const u8* m_src;
    size_t m_size;
    u8* m_out;
    DeflateWorkspace& m_work;
    u64 m_bits = 0;
    u32 m_bitCount = 0;
    u32 m_hashBits = 8;
    u32 m_windowMask = 0;
    size_t m_blockStart = 0;
    bool m_lastBlockWritten = false;
    std::array<u32, kLitLenCodes + 2> m_litLenCount{};
    std::array<u32, kDistCodes + 2> m_distanceCount{};
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
    // Pre-size to expectedSize when known (and plausible), so the inner loop
    // rarely re-allocates.
    size_t initSize = expectedSize > 0 ? common::outputReserve(expectedSize, data.size()) : data.size() * 4;
    if (initSize < 4096)
        initSize = 4096;
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

bool zlib_compress_append(std::vector<u8>& out, std::span<const u8> data, std::string* /*out_error*/) {
    thread_local DeflateWorkspace workspace;
    size_t const start = out.size();
    out.resize(start + 2 + Deflater::bound(data.size()) + 4);
    u8* const stream = out.data() + start;

    // Zlib header: CMF=0x78 (CM=8/deflate, CINFO=7/32K window), FLG=0x9C
    // (default compression, check bits). (0x78 << 8 | 0x9C) % 31 = 0.
    stream[0] = 0x78;
    stream[1] = 0x9C;

    Deflater deflater(data, stream + 2, workspace);
    u8* end = deflater.run();

    // Adler-32 checksum (big-endian).
    u32 const check = adler32(data);
    *end++ = static_cast<u8>((check >> 24) & 0xFF);
    *end++ = static_cast<u8>((check >> 16) & 0xFF);
    *end++ = static_cast<u8>((check >> 8) & 0xFF);
    *end++ = static_cast<u8>(check & 0xFF);

    out.resize(static_cast<size_t>(end - out.data()));
    return true;
}

std::vector<u8> zlib_compress(std::span<const u8> data, std::string* out_error) {
    std::vector<u8> out;
    if (!zlib_compress_append(out, data, out_error))
        return {};
    return out;
}

} // namespace whiteout
