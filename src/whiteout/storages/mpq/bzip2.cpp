// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Minimal single-file BZip2 decompressor. Handles standard bzip2 streams as
// produced by libbzip2 (which is what MPQ archives use). No external deps.

#include "bzip2.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <numeric>

namespace whiteout::storages::mpq {

namespace {

// ============================================================================
// MSB-first bit reader (plain, no byte-stuffing)
// ============================================================================

struct Bz2BitReader {
    const u8* data;
    size_t size;
    size_t bytePos = 0;
    u32 bitBuf = 0;
    i32 bitsAvail = 0;

    Bz2BitReader(const u8* d, size_t s) : data(d), size(s) {}

    void refill() {
        while (bitsAvail <= 24 && bytePos < size) {
            bitBuf |= static_cast<u32>(data[bytePos++]) << (24 - bitsAvail);
            bitsAvail += 8;
        }
    }

    u32 readBits(i32 count) {
        if (count <= 24) {
            refill();
            u32 val = bitBuf >> (32 - count);
            bitBuf <<= count;
            bitsAvail -= count;
            return val;
        }
        // For count > 24, split to avoid undefined behavior from shifting
        // a u32 by 32 or more bits.
        u32 hi = readBits(16);
        u32 lo = readBits(count - 16);
        return (hi << (count - 16)) | lo;
    }

    u8 readByte() {
        return static_cast<u8>(readBits(8));
    }

    bool hasData() const {
        return bytePos < size || bitsAvail > 0;
    }
};

// ============================================================================
// Constants
// ============================================================================

static constexpr u32 kMaxBlockSize = 900000; // Max bzip2 block (level 9).
static constexpr u32 kMaxAlphaSize = 258;    // Max Huffman symbols.
static constexpr u32 kMaxGroups = 6;         // Max coding groups.
static constexpr u32 kGroupSize = 50;        // Symbols per group selector.
static constexpr u32 kMaxSelectors = (2 + kMaxBlockSize / kGroupSize); // Worst case.
static constexpr u32 kMaxCodeLen = 23;                                 // Max Huffman code length.
static constexpr u32 kRunA = 0;
static constexpr u32 kRunB = 1;

// ============================================================================
// Huffman table for bzip2 (canonical codes, MSB-first)
// ============================================================================

struct Bz2HuffTable {
    i32 minLen = 0;
    i32 maxLen = 0;
    // For each code length: base code value and permutation index.
    std::array<u32, kMaxCodeLen + 2> base{};  // base[len] = first code of length len.
    std::array<u32, kMaxCodeLen + 2> limit{}; // limit[len] = last+1 code of length len.
    std::array<u32, kMaxCodeLen + 2>
        permBase{};                        // permBase[len] = cumulative symbol count before len.
    std::array<u32, kMaxAlphaSize> perm{}; // Permutation array.

    bool build(const u8* lengths, u32 nSyms) {
        // Count occurrences of each code length.
        std::array<u32, kMaxCodeLen + 2> count{};
        for (u32 i = 0; i < nSyms; i++) {
            if (lengths[i] > kMaxCodeLen)
                return false;
            count[lengths[i]]++;
        }

        // Find min/max code lengths.
        minLen = 1;
        while (minLen <= static_cast<i32>(kMaxCodeLen) && count[minLen] == 0)
            minLen++;
        maxLen = kMaxCodeLen;
        while (maxLen >= 1 && count[maxLen] == 0)
            maxLen--;
        if (minLen > maxLen)
            return false;

        // Compute base values and limits.
        u32 code = 0;
        u32 permIdx = 0;
        for (i32 len = minLen; len <= maxLen; len++) {
            base[len] = code;
            permBase[len] = permIdx;
            for (u32 i = 0; i < nSyms; i++) {
                if (lengths[i] == static_cast<u8>(len)) {
                    perm[permIdx++] = i;
                }
            }
            code += count[len];
            limit[len] = code;
            code <<= 1;
        }

        return true;
    }

    u32 decode(Bz2BitReader& br) const {
        i32 len = minLen;
        u32 code = br.readBits(len);

        while (len <= maxLen) {
            if (code < limit[len]) {
                return perm[permBase[len] + (code - base[len])];
            }
            code = (code << 1) | br.readBits(1);
            len++;
        }
        return 0; // Error — shouldn't reach here on valid data.
    }
};

// ============================================================================
// BZip2 block decompressor
// ============================================================================

struct Bz2Decoder {
    Bz2BitReader& br;
    std::vector<u8>& output;
    size_t maxOutput;

    Bz2Decoder(Bz2BitReader& reader, std::vector<u8>& out, size_t maxOut)
        : br(reader), output(out), maxOutput(maxOut) {}

    bool decodeBlock() {
        // Read block header magic: 0x314159265359 (48 bits).
        u32 hi = br.readBits(24);
        u32 lo = br.readBits(24);
        if (hi == 0x177245 && lo == 0x385090) {
            // End-of-stream marker.
            return false;
        }
        if (hi != 0x314159 || lo != 0x265359) {
            return false; // Invalid block magic.
        }

        // Block CRC (32 bits) — we skip verification for simplicity.
        u32 blockCrc = br.readBits(32);

        // Randomized flag (1 bit) — must be 0 in modern bzip2.
        u32 blockRandomised = br.readBits(1);

        // Original pointer for BWT undo.
        u32 origPtr = br.readBits(24);

        // Symbol map — which of 16 groups of 16 symbols are used.
        u32 usedGroups = br.readBits(16);
        std::array<bool, 256> inUse{};
        u32 nInUse = 0;

        for (u32 g = 0; g < 16; g++) {
            if (usedGroups & (1u << (15 - g))) {
                u32 groupBits = br.readBits(16);
                for (u32 b = 0; b < 16; b++) {
                    if (groupBits & (1u << (15 - b))) {
                        inUse[g * 16 + b] = true;
                        nInUse++;
                    }
                }
            }
        }
        if (nInUse == 0)
            return false;

        u32 alphaSize = nInUse + 2; // +2 for RUNA and RUNB.

        // Number of Huffman coding groups.
        u32 nGroups = br.readBits(3);
        if (nGroups < 2 || nGroups > kMaxGroups)
            return false;

        // Selector list via MTF encoding.
        u32 nSelectors = br.readBits(15);
        if (nSelectors == 0 || nSelectors > kMaxSelectors)
            return false;

        // Decode selectors using unary coding + MTF.
        std::vector<u8> selectorMtf(nSelectors);
        u8 selectorMtfBuf[kMaxGroups];
        for (u32 i = 0; i < nGroups; i++)
            selectorMtfBuf[i] = static_cast<u8>(i);

        for (u32 i = 0; i < nSelectors; i++) {
            u32 idx = 0;
            while (br.readBits(1)) {
                idx++;
                if (idx >= nGroups)
                    return false;
            }
            // MTF undo.
            u8 val = selectorMtfBuf[idx];
            for (u32 j = idx; j > 0; j--)
                selectorMtfBuf[j] = selectorMtfBuf[j - 1];
            selectorMtfBuf[0] = val;
            selectorMtf[i] = val;
        }

        // Decode Huffman tables for each group.
        std::array<Bz2HuffTable, kMaxGroups> tables;
        for (u32 g = 0; g < nGroups; g++) {
            u8 lengths[kMaxAlphaSize];
            i32 curr = static_cast<i32>(br.readBits(5));

            for (u32 s = 0; s < alphaSize; s++) {
                for (;;) {
                    if (curr < 1 || curr > 20)
                        return false;
                    u32 bit = br.readBits(1);
                    if (bit == 0)
                        break;
                    u32 bit2 = br.readBits(1);
                    curr += bit2 ? -1 : 1;
                }
                lengths[s] = static_cast<u8>(curr);
            }

            if (!tables[g].build(lengths, alphaSize))
                return false;
        }

        // Decode symbols (MTF + RLE2).
        // Build the "in use" symbol list for MTF.
        u8 mtfSymbols[256];
        u32 mtfCount = 0;
        for (u32 i = 0; i < 256; i++) {
            if (inUse[i])
                mtfSymbols[mtfCount++] = static_cast<u8>(i);
        }

        // The decoded block data before BWT undo.
        std::vector<u8> block;
        block.reserve(kMaxBlockSize);

        u32 groupPos = 0;
        u32 groupNo = 0;
        const Bz2HuffTable* currentTable = &tables[selectorMtf[0]];

        for (;;) {
            if (groupPos == kGroupSize) {
                groupNo++;
                if (groupNo >= nSelectors)
                    return false;
                currentTable = &tables[selectorMtf[groupNo]];
                groupPos = 0;
            }
            groupPos++;

            u32 sym = currentTable->decode(br);

            if (sym == nInUse + 1)
                break; // EOB symbol.
            if (sym > nInUse + 1)
                return false;

            if (sym == kRunA || sym == kRunB) {
                // RUNA/RUNB zero-run decoding.
                u32 runLen = 0;
                u32 runPower = 1;
                do {
                    runLen += (sym == kRunA) ? runPower : 2 * runPower;
                    runPower <<= 1;

                    if (groupPos == kGroupSize) {
                        groupNo++;
                        if (groupNo >= nSelectors)
                            return false;
                        currentTable = &tables[selectorMtf[groupNo]];
                        groupPos = 0;
                    }
                    groupPos++;
                    sym = currentTable->decode(br);
                } while (sym == kRunA || sym == kRunB);

                // The byte to repeat is the one at mtfSymbols[0].
                u8 repeatByte = mtfSymbols[0];
                if (block.size() + runLen > kMaxBlockSize)
                    return false;
                block.insert(block.end(), runLen, repeatByte);

                // Don't continue — sym already has the next symbol.
                // Fall through to process it.
            }

            if (sym == nInUse + 1)
                break;                               // EOB from inner loop.
            if (sym >= kRunA + 1 && sym <= nInUse) { // sym is an MTF index + 1.
                // MTF decode: sym-1 is the MTF position.
                u32 mtfIdx = sym - 1;
                if (mtfIdx >= mtfCount)
                    return false;
                u8 byte = mtfSymbols[mtfIdx];
                // Move to front.
                for (u32 j = mtfIdx; j > 0; j--)
                    mtfSymbols[j] = mtfSymbols[j - 1];
                mtfSymbols[0] = byte;
                block.push_back(byte);
            }

            if (block.size() > kMaxBlockSize)
                return false;
        }

        u32 blockSize = static_cast<u32>(block.size());
        if (origPtr >= blockSize)
            return false;

        // Inverse BWT Transform.
        // Step 1: Count character frequencies.
        std::array<u32, 256> charCount{};
        for (u8 b : block)
            charCount[b]++;

        // Step 2: Compute cumulative counts.
        std::array<u32, 256> cumCount{};
        u32 sum = 0;
        for (u32 i = 0; i < 256; i++) {
            cumCount[i] = sum;
            sum += charCount[i];
        }

        // Step 3: Build the transformation vector T.
        std::vector<u32> T(blockSize);
        for (u32 i = 0; i < blockSize; i++) {
            u8 ch = block[i];
            T[cumCount[ch]] = i;
            cumCount[ch]++;
        }

        // Step 4: Follow the transformation vector to produce output.
        u32 pos = T[origPtr];
        size_t outStart = output.size();
        for (u32 i = 0; i < blockSize; i++) {
            u8 ch = block[pos];
            output.push_back(ch);
            pos = T[pos];
            if (output.size() >= maxOutput)
                break;
        }

        // Step 5: Undo the bzip2 post-BWT RLE (runs of 4+ identical bytes).
        // The output so far has the RLE still encoded. We need to decode it.
        // Actually in bzip2, the RLE is applied BEFORE the BWT, so after inverse
        // BWT we get RLE-encoded data that needs to be decoded.
        //
        // RLE format: if 4 consecutive identical bytes appear, the next byte N
        // means "repeat that byte N more times" (0-255 additional copies).
        size_t rleStart = outStart;
        size_t rleEnd = output.size();
        std::vector<u8> decoded;
        decoded.reserve(rleEnd - rleStart);

        size_t rp = rleStart;
        while (rp < rleEnd) {
            u8 ch = output[rp++];
            decoded.push_back(ch);
            u32 runCount = 1;

            while (rp < rleEnd && output[rp] == ch && runCount < 4) {
                decoded.push_back(ch);
                runCount++;
                rp++;
            }

            if (runCount == 4 && rp < rleEnd) {
                u32 extra = output[rp++];
                for (u32 j = 0; j < extra; j++) {
                    decoded.push_back(ch);
                }
            }
        }

        // Replace the BWT output with the RLE-decoded output.
        output.resize(outStart);
        output.insert(output.end(), decoded.begin(), decoded.end());

        // Handle the blockRandomised flag (legacy, rarely used).
        if (blockRandomised) {
            // Randomised blocks are extremely rare in practice. We don't support them.
            return false;
        }

        return true;
    }

    bool decode() {
        // Stream header: 'B', 'Z', 'h', then block size digit '1'-'9'.
        u8 b0 = br.readByte();
        u8 b1 = br.readByte();
        u8 b2 = br.readByte();
        if (b0 != 'B' || b1 != 'Z' || b2 != 'h')
            return false;

        u8 blockSizeChar = br.readByte();
        if (blockSizeChar < '1' || blockSizeChar > '9')
            return false;

        // Decompress blocks until end-of-stream.
        while (output.size() < maxOutput) {
            if (!decodeBlock())
                break;
        }

        return !output.empty();
    }
};

} // anonymous namespace

std::vector<u8> bzip2Decompress(std::span<const u8> src, size_t expectedSize) {
    if (src.size() < 10)
        return {}; // Minimum: header + end-of-stream block.

    Bz2BitReader br(src.data(), src.size());
    std::vector<u8> out;
    out.reserve(expectedSize);

    Bz2Decoder decoder(br, out, expectedSize);
    if (!decoder.decode())
        return {};

    return out;
}

// ============================================================================
// BZip2 Compressor
// ============================================================================

namespace {

// MSB-first bit writer.
struct Bz2BitWriter {
    std::vector<u8>& out;
    u32 bitBuf = 0;
    i32 bitsUsed = 0;

    Bz2BitWriter(std::vector<u8>& o) : out(o) {}

    void writeBits(i32 count, u32 val) {
        while (count > 0) {
            i32 space = 8 - bitsUsed;
            i32 bits = (count < space) ? count : space;
            // Take the top `bits` from val.
            u32 chunk = (val >> (count - bits)) & ((1u << bits) - 1);
            bitBuf = (bitBuf << bits) | chunk;
            bitsUsed += bits;
            count -= bits;
            if (bitsUsed == 8) {
                out.push_back(static_cast<u8>(bitBuf));
                bitBuf = 0;
                bitsUsed = 0;
            }
        }
    }

    void writeByte(u8 val) {
        writeBits(8, val);
    }

    void flush() {
        if (bitsUsed > 0) {
            out.push_back(static_cast<u8>(bitBuf << (8 - bitsUsed)));
            bitBuf = 0;
            bitsUsed = 0;
        }
    }
};

// BZip2 CRC-32 table.
static constexpr auto kBz2Crc32Table = [] {
    std::array<u32, 256> table{};
    for (u32 i = 0; i < 256; i++) {
        u32 c = i << 24;
        for (int j = 0; j < 8; j++) {
            c = (c & 0x80000000) ? (c << 1) ^ 0x04C11DB7 : (c << 1);
        }
        table[i] = c;
    }
    return table;
}();

u32 bz2Crc32(const u8* data, size_t len) {
    u32 crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ kBz2Crc32Table[((crc >> 24) ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

// BWT forward transform using suffix array (simple doubling algorithm).
// Returns the index of the original string in the sorted rotation list.
u32 bwtForward(const u8* input, u32 len, u8* output) {
    // Build suffix array via std::sort with circular comparison.
    std::vector<u32> sa(len);
    std::iota(sa.begin(), sa.end(), 0u);

    std::sort(sa.begin(), sa.end(), [&](u32 a, u32 b) {
        for (u32 i = 0; i < len; i++) {
            u8 ca = input[(a + i) % len];
            u8 cb = input[(b + i) % len];
            if (ca != cb)
                return ca < cb;
        }
        return false;
    });

    u32 origPtr = 0;
    for (u32 i = 0; i < len; i++) {
        if (sa[i] == 0)
            origPtr = i;
        output[i] = input[(sa[i] + len - 1) % len];
    }
    return origPtr;
}

// BZip2 initial RLE: collapse runs of 4+ identical bytes into 4 copies + repeat count.
std::vector<u8> bz2RleEncode(const u8* data, size_t len) {
    std::vector<u8> out;
    out.reserve(len);
    size_t pos = 0;
    while (pos < len) {
        u8 ch = data[pos];
        size_t run = 1;
        while (pos + run < len && data[pos + run] == ch && run < 259)
            run++;

        if (run >= 4) {
            // Emit 4 copies + repeat count.
            out.push_back(ch);
            out.push_back(ch);
            out.push_back(ch);
            out.push_back(ch);
            u32 extra = static_cast<u32>(run - 4);
            if (extra > 255)
                extra = 255;
            out.push_back(static_cast<u8>(extra));
            pos += 4 + extra;
        } else {
            for (size_t i = 0; i < run; i++)
                out.push_back(ch);
            pos += run;
        }
    }
    return out;
}

// MTF (Move-To-Front) transform + RUNA/RUNB encoding.
// Returns the encoded symbols where sym 0 = RUNA, sym 1 = RUNB,
// sym N (N>=2) = MTF position N-1 for that byte.
struct MtfResult {
    std::vector<u32> symbols;
    std::array<bool, 256> inUse{};
    u32 nInUse = 0;
    u32 eob = 0; // End-of-block symbol index.
};

MtfResult mtfEncode(const u8* data, u32 len) {
    MtfResult result;

    // Determine which bytes are in use.
    for (u32 i = 0; i < len; i++)
        result.inUse[data[i]] = true;

    // Build the MTF list from in-use symbols.
    u8 mtf[256];
    u32 nInUse = 0;
    for (u32 i = 0; i < 256; i++) {
        if (result.inUse[i])
            mtf[nInUse++] = static_cast<u8>(i);
    }
    result.nInUse = nInUse;
    result.eob = nInUse + 1; // EOB symbol.

    result.symbols.reserve(len + len / 4);

    u32 zeroRunLen = 0;

    auto flushZeroRun = [&]() {
        // Encode zero-run using RUNA/RUNB (bijective base-2 numeration).
        // The decoder computes: runLen += (sym == RUNA) ? power : 2*power; power <<= 1;
        // So we decompose zeroRunLen as sum of d_i * 2^i where d_i ∈ {1,2}.
        u32 n = zeroRunLen;
        while (n > 0) {
            if (n % 2 == 1) {
                result.symbols.push_back(0); // RUNA (value 1)
                n = (n - 1) / 2;
            } else {
                result.symbols.push_back(1); // RUNB (value 2)
                n = (n - 2) / 2;
            }
        }
        zeroRunLen = 0;
    };

    for (u32 i = 0; i < len; i++) {
        u8 ch = data[i];
        // Find position in MTF list.
        u32 pos = 0;
        while (mtf[pos] != ch)
            pos++;

        // Move to front.
        for (u32 j = pos; j > 0; j--)
            mtf[j] = mtf[j - 1];
        mtf[0] = ch;

        if (pos == 0) {
            zeroRunLen++;
        } else {
            if (zeroRunLen > 0)
                flushZeroRun();
            result.symbols.push_back(pos + 1); // MTF position + 1 (since 0,1 are RUNA/RUNB).
        }
    }
    if (zeroRunLen > 0)
        flushZeroRun();
    result.symbols.push_back(result.eob);

    return result;
}

// Assign Huffman code lengths from frequency table using a min-heap approach.
// Produces valid code lengths satisfying the Kraft inequality.
// All symbols in [0, nSyms) are assigned a length in [1, maxLen].
void assignCodeLengths(const u32* freqs, u32 nSyms, u8* lengths, i32 maxLen = 20) {
    if (nSyms == 0)
        return;
    if (nSyms == 1) {
        lengths[0] = 1;
        return;
    }

    // All symbols participate (zero-freq symbols get freq=1 to ensure valid Huffman).
    struct Node {
        u64 freq;
        i32 left, right; // -1 = leaf
        u32 sym;         // valid only for leaves
    };

    std::vector<Node> nodes;
    nodes.reserve(nSyms * 2);

    for (u32 i = 0; i < nSyms; i++) {
        nodes.push_back({freqs[i] > 0 ? static_cast<u64>(freqs[i]) : 1u, -1, -1, i});
    }

    // Min-heap comparator.
    auto cmp = [](std::pair<u64, i32>& a, std::pair<u64, i32>& b) {
        return a.first > b.first || (a.first == b.first && a.second > b.second);
    };

    std::vector<std::pair<u64, i32>> heap;
    for (i32 i = 0; i < static_cast<i32>(nodes.size()); i++) {
        heap.push_back({nodes[i].freq, i});
    }
    std::make_heap(heap.begin(), heap.end(), cmp);

    while (heap.size() > 1) {
        std::pop_heap(heap.begin(), heap.end(), cmp);
        auto [f1, i1] = heap.back();
        heap.pop_back();
        std::pop_heap(heap.begin(), heap.end(), cmp);
        auto [f2, i2] = heap.back();
        heap.pop_back();

        i32 newIdx = static_cast<i32>(nodes.size());
        nodes.push_back({f1 + f2, i1, i2, 0});
        heap.push_back({f1 + f2, newIdx});
        std::push_heap(heap.begin(), heap.end(), cmp);
    }

    // Compute depths via DFS.
    std::vector<std::pair<i32, i32>> stack; // (nodeIdx, depth)
    stack.push_back({heap[0].second, 0});
    while (!stack.empty()) {
        auto [idx, depth] = stack.back();
        stack.pop_back();
        if (nodes[idx].left == -1) {
            // Leaf node.
            i32 d = depth;
            if (d < 1)
                d = 1;
            if (d > maxLen)
                d = maxLen;
            lengths[nodes[idx].sym] = static_cast<u8>(d);
        } else {
            stack.push_back({nodes[idx].left, depth + 1});
            stack.push_back({nodes[idx].right, depth + 1});
        }
    }
}

// Build canonical Huffman codes from lengths (for encoding).
void buildCanonicalCodes(const u8* lengths, u32 nSyms, u32* codes) {
    // Find min/max lengths.
    i32 minLen = 25, maxLen2 = 0;
    for (u32 i = 0; i < nSyms; i++) {
        if (lengths[i] > 0) {
            if (lengths[i] < minLen)
                minLen = lengths[i];
            if (lengths[i] > maxLen2)
                maxLen2 = lengths[i];
        }
    }

    // Sort symbols by (length, symbol) for canonical ordering.
    std::vector<std::pair<u8, u32>> syms;
    for (u32 i = 0; i < nSyms; i++) {
        if (lengths[i] > 0)
            syms.push_back({lengths[i], i});
    }
    std::sort(syms.begin(), syms.end());

    u32 code = 0;
    i32 prevLen = syms.empty() ? 0 : syms[0].first;
    for (auto& [len, sym] : syms) {
        code <<= (len - prevLen);
        codes[sym] = code;
        code++;
        prevLen = len;
    }
}

void encodeBlock(Bz2BitWriter& bw, const u8* data, u32 len, u32 blockCrc) {
    // Block magic: 0x314159265359
    bw.writeBits(24, 0x314159);
    bw.writeBits(24, 0x265359);

    // Block CRC.
    bw.writeBits(32, blockCrc);

    // Randomized = 0.
    bw.writeBits(1, 0);

    // Apply initial RLE.
    auto rled = bz2RleEncode(data, len);

    // Forward BWT.
    std::vector<u8> bwtOut(rled.size());
    u32 origPtr = bwtForward(rled.data(), static_cast<u32>(rled.size()), bwtOut.data());

    bw.writeBits(24, origPtr);

    // MTF + RUNA/RUNB encode.
    auto mtf = mtfEncode(bwtOut.data(), static_cast<u32>(bwtOut.size()));

    // Write symbol map.
    u32 groupMask = 0;
    for (u32 g = 0; g < 16; g++) {
        for (u32 b = 0; b < 16; b++) {
            if (mtf.inUse[g * 16 + b]) {
                groupMask |= (1u << (15 - g));
                break;
            }
        }
    }
    bw.writeBits(16, groupMask);
    for (u32 g = 0; g < 16; g++) {
        if (groupMask & (1u << (15 - g))) {
            u32 bits = 0;
            for (u32 b = 0; b < 16; b++) {
                if (mtf.inUse[g * 16 + b])
                    bits |= (1u << (15 - b));
            }
            bw.writeBits(16, bits);
        }
    }

    u32 alphaSize = mtf.nInUse + 2;
    u32 nSymbols = static_cast<u32>(mtf.symbols.size());

    // Use a single Huffman group for simplicity.
    u32 nGroups = 2; // BZip2 requires at least 2.
    u32 nSelectors = (nSymbols + kGroupSize - 1) / kGroupSize;

    bw.writeBits(3, nGroups);
    bw.writeBits(15, nSelectors);

    // Selectors: all point to group 0.
    for (u32 i = 0; i < nSelectors; i++) {
        bw.writeBits(1, 0); // Unary 0 = group 0 (MTF position 0).
    }

    // Compute frequency table.
    std::vector<u32> freq(alphaSize, 0);
    for (u32 sym : mtf.symbols) {
        if (sym < alphaSize)
            freq[sym]++;
    }

    // Assign code lengths.
    std::vector<u8> codeLens(alphaSize, 0);
    assignCodeLengths(freq.data(), alphaSize, codeLens.data());

    // Build canonical codes.
    std::vector<u32> codes(alphaSize, 0);
    buildCanonicalCodes(codeLens.data(), alphaSize, codes.data());

    // Write Huffman tables — group 0 is the real table, group 1 is a dummy copy.
    for (u32 g = 0; g < nGroups; g++) {
        i32 curr = codeLens[0];
        bw.writeBits(5, static_cast<u32>(curr));
        for (u32 s = 0; s < alphaSize; s++) {
            i32 target = codeLens[s];
            while (curr < target) {
                bw.writeBits(2, 0x2); // 1,0 = increment
                curr++;
            }
            while (curr > target) {
                bw.writeBits(2, 0x3); // 1,1 = decrement
                curr--;
            }
            bw.writeBits(1, 0); // 0 = done for this symbol.
        }
    }

    // Encode symbols.
    for (u32 sym : mtf.symbols) {
        if (sym < alphaSize && codeLens[sym] > 0) {
            bw.writeBits(codeLens[sym], codes[sym]);
        }
    }
}

} // anonymous namespace

std::vector<u8> bzip2Compress(std::span<const u8> src) {
    if (src.empty())
        return {};

    std::vector<u8> out;
    out.reserve(src.size() + 64);

    Bz2BitWriter bw(out);

    // Stream header: 'B', 'Z', 'h', block size digit.
    bw.writeByte('B');
    bw.writeByte('Z');
    bw.writeByte('h');
    bw.writeByte('9'); // Block size 9 (900k) — standard for MPQ.

    // Compute CRC of original data.
    u32 blockCrc = bz2Crc32(src.data(), src.size());

    // Encode all data as a single block (BZip2 max block = 900k).
    // For MPQ sectors, data is always < 900k.
    encodeBlock(bw, src.data(), static_cast<u32>(src.size()), blockCrc);

    // End-of-stream magic: 0x177245385090.
    bw.writeBits(24, 0x177245);
    bw.writeBits(24, 0x385090);

    // Combined CRC (just the one block CRC).
    bw.writeBits(32, blockCrc);

    bw.flush();

    return out;
}

} // namespace whiteout::storages::mpq
