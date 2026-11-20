// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "pkware.h"

#include "../../../common/output_reserve.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace whiteout::storages::mpq {

namespace {

// ============================================================================
// Format tables
// ============================================================================

// The stream header: how literals are coded, then log2 of the window over 64.
constexpr u8 kBinaryLiterals = 0; ///< Literals are plain 8-bit bytes.
constexpr u8 kAsciiLiterals = 1;  ///< Literals use the ASCII prefix code.
constexpr u32 kMinDictBits = 4;   ///< 1 KiB window.
constexpr u32 kMaxDictBits = 6;   ///< 4 KiB window.

// A copy of this length ends the stream; the longest real copy is one less.
constexpr u32 kEndOfStream = 519;
constexpr u32 kMaxMatch = kEndOfStream - 1;
// A 2-byte copy keeps 2 low distance bits, so it reaches 256 bytes back.
constexpr u32 kShortMatchDistBits = 2;

// The 16 length codes: each one's bit length, the copy length it starts at,
// and the extra bits that follow it.
constexpr std::array<u8, 16> kLenBits = {3, 2, 3, 3, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7};
constexpr std::array<u16, 16> kLenBase = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
constexpr std::array<u8, 16> kLenExtraBits = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};

// The 64 codes for the high bits of a copy distance.
constexpr std::array<u8, 64> kDistBits = {
    2, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};

// The 256 literal codes of ASCII mode.
constexpr std::array<u8, 256> kAsciiBits = {
    0x0B, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x08, 0x07, 0x0C, 0x0C, 0x07, 0x0C, 0x0C,
    0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
    0x04, 0x0A, 0x08, 0x0C, 0x0A, 0x0C, 0x0A, 0x08, 0x07, 0x07, 0x08, 0x09, 0x07, 0x06, 0x07, 0x08,
    0x07, 0x06, 0x07, 0x07, 0x07, 0x07, 0x08, 0x07, 0x07, 0x08, 0x08, 0x0C, 0x0B, 0x07, 0x09, 0x0B,
    0x0C, 0x06, 0x07, 0x06, 0x06, 0x05, 0x07, 0x08, 0x08, 0x06, 0x0B, 0x09, 0x06, 0x07, 0x06, 0x06,
    0x07, 0x0B, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x09, 0x09, 0x0B, 0x08, 0x0B, 0x09, 0x0C, 0x08,
    0x0C, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x0B, 0x07, 0x05, 0x06, 0x05, 0x05,
    0x06, 0x0A, 0x05, 0x05, 0x05, 0x05, 0x08, 0x07, 0x08, 0x08, 0x0A, 0x0B, 0x0B, 0x0C, 0x0C, 0x0C,
    0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D,
    0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D,
    0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D,
    0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
    0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
    0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
    0x0D, 0x0C, 0x0D, 0x0D, 0x0D, 0x0C, 0x0D, 0x0D, 0x0D, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0C, 0x0D,
    0x0D, 0x0D, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D};

// ============================================================================
// Prefix codes
// ============================================================================

/// A prefix code the format gives only by bit lengths. Each code is the
/// canonical one for its length (shorter first, then by symbol), inverted,
/// and stored low bit first.
template <size_t N, u32 MaxBits>
struct PrefixCode {
    struct Entry {
        u8 symbol = 0;
        u8 bits = 0; ///< 0: no code starts with these bits.
    };
    std::array<u16, N> code{}; ///< As stored in the stream.
    std::array<u8, N> bits{};
    std::array<Entry, size_t{1} << MaxBits> lookup{}; ///< The next MaxBits stream bits -> symbol.

    explicit PrefixCode(const std::array<u8, N>& lengths) {
        u32 canonical = 0;
        for (u32 len = 1; len <= MaxBits; ++len, canonical <<= 1) {
            for (size_t s = 0; s < N; ++s) {
                if (lengths[s] != len)
                    continue;
                u32 const inverted = ~canonical++ & ((1u << len) - 1);
                u32 stored = 0;
                for (u32 b = 0; b < len; ++b)
                    stored |= ((inverted >> b) & 1u) << (len - 1 - b);
                code[s] = static_cast<u16>(stored);
                bits[s] = static_cast<u8>(len);
                for (u32 fill = stored; fill < lookup.size(); fill += 1u << len)
                    lookup[fill] = {static_cast<u8>(s), static_cast<u8>(len)};
            }
        }
    }
};

struct Codes {
    PrefixCode<16, 7> length{kLenBits};
    PrefixCode<64, 8> distance{kDistBits};
    PrefixCode<256, 13> ascii{kAsciiBits};
};

const Codes& codes() {
    static const Codes kCodes;
    return kCodes;
}

// ============================================================================
// Bit streams (low bit first)
// ============================================================================

class BitWriter {
public:
    explicit BitWriter(std::vector<u8>& out) : m_out(out) {}

    void put(u32 value, u32 count) {
        m_buffer |= static_cast<u64>(value) << m_count;
        m_count += count;
        for (; m_count >= 8; m_count -= 8, m_buffer >>= 8)
            m_out.push_back(static_cast<u8>(m_buffer));
    }

    template <size_t N, u32 MaxBits>
    void put(const PrefixCode<N, MaxBits>& code, u32 symbol) {
        put(code.code[symbol], code.bits[symbol]);
    }

    void finish() {
        if (m_count > 0)
            m_out.push_back(static_cast<u8>(m_buffer));
        m_buffer = 0;
        m_count = 0;
    }

private:
    std::vector<u8>& m_out;
    u64 m_buffer = 0;
    u32 m_count = 0;
};

/// The length code whose range holds @p length.
u32 lengthCode(u32 length) {
    u32 code = 0;
    while (code + 1 < kLenBase.size() && kLenBase[code + 1] <= length)
        ++code;
    return code;
}

} // anonymous namespace

// ============================================================================
// Explode (decompress)
// ============================================================================

size_t pkwareExplodeInto(std::span<const u8> src, std::span<u8> dst) {
    if (src.size() < 2)
        return 0;
    u8 const literalMode = src[0];
    u32 const dictBits = src[1];
    if (literalMode > kAsciiLiterals || dictBits < kMinDictBits || dictBits > kMaxDictBits)
        return 0;

    const Codes& c = codes();
    const u8* in = src.data() + 2;
    const u8* const inEnd = src.data() + src.size();
    u64 bits = 0;  // The stream's next bits, low bit first.
    u32 count = 0; // How many of them came from whole bytes; past the input they are zeros.
    u8* out = dst.data();
    u8* const outBase = out;
    u8* const outEnd = out + dst.size();

    while (out < outEnd) {
        // One refill covers a whole symbol: the longest is a copy, at
        // 1 + 7 + 8 + 8 + 6 = 30 bits. Eight bytes at a time while the input
        // has them; the part of the last byte that does not fit is read again.
        if (inEnd - in >= 8) {
            u64 next = 0;
            std::memcpy(&next, in, 8);
            bits |= next << count;
            u32 const take = (63 - count) >> 3;
            in += take;
            count += take * 8;
        } else {
            while (count <= 56 && in < inEnd) {
                bits |= static_cast<u64>(*in++) << count;
                count += 8;
            }
        }

        u32 used = 1;
        if ((bits & 1) == 0) {
            u32 literal = 0;
            if (literalMode == kAsciiLiterals) {
                auto const entry = c.ascii.lookup[(bits >> 1) & 0x1FFF];
                if (entry.bits == 0)
                    return 0;
                literal = entry.symbol;
                used += entry.bits;
            } else {
                literal = static_cast<u32>(bits >> 1) & 0xFF;
                used += 8;
            }
            if (used > count) // The input ran out inside the symbol.
                return 0;
            bits >>= used;
            count -= used;
            *out++ = static_cast<u8>(literal);
            continue;
        }

        auto const lengthEntry = c.length.lookup[(bits >> used) & 0x7F];
        if (lengthEntry.bits == 0)
            return 0;
        used += lengthEntry.bits;
        u32 const extraBits = kLenExtraBits[lengthEntry.symbol];
        u32 const length = kLenBase[lengthEntry.symbol] + (static_cast<u32>(bits >> used) & ((1u << extraBits) - 1));
        used += extraBits;
        if (length == kEndOfStream) {
            if (used > count)
                return 0;
            break;
        }

        u32 const lowBits = length == 2 ? kShortMatchDistBits : dictBits;
        auto const distanceEntry = c.distance.lookup[(bits >> used) & 0xFF];
        if (distanceEntry.bits == 0)
            return 0;
        used += distanceEntry.bits;
        size_t const distance =
            ((size_t{distanceEntry.symbol} << lowBits) | (static_cast<u32>(bits >> used) & ((1u << lowBits) - 1))) + 1;
        used += lowBits;
        if (used > count)
            return 0;
        bits >>= used;
        count -= used;
        if (distance > static_cast<size_t>(out - outBase))
            return 0;

        size_t const room = static_cast<size_t>(outEnd - out);
        size_t const n = std::min<size_t>(length, room);
        const u8* from = out - distance;
        if (distance >= 8 && room >= n + 8) {
            // Eight bytes a step: what it writes past n, later output writes over.
            for (size_t i = 0; i < n; i += 8)
                std::memcpy(out + i, from + i, 8);
        } else {
            for (size_t i = 0; i < n; ++i)
                out[i] = from[i];
        }
        out += n;
    }
    return static_cast<size_t>(out - outBase);
}

std::vector<u8> pkwareExplode(std::span<const u8> src, size_t expectedSize) {
    return whiteout::common::decodeGrowing(expectedSize, src.size(),
                                           [&](std::span<u8> dst) { return pkwareExplodeInto(src, dst); });
}

// ============================================================================
// Implode (compress) — binary literals, greedy hash-chain matching
// ============================================================================

std::vector<u8> pkwareImplode(std::span<const u8> src) {
    if (src.empty())
        return {};

    // The smallest window that covers the input: its low distance bits are cheaper.
    u32 dictBits = kMinDictBits;
    while (dictBits < kMaxDictBits && (size_t{64} << dictBits) < src.size())
        ++dictBits;
    size_t const window = size_t{64} << dictBits;

    constexpr u32 kHashBits = 12;
    constexpr u32 kMaxChain = 64;
    constexpr u32 kMinMatch = 3; // Matches are found by 3-byte hash.
    auto hashAt = [&](size_t pos) {
        u32 const v = src[pos] | (u32{src[pos + 1]} << 8) | (u32{src[pos + 2]} << 16);
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    std::vector<i32> head(size_t{1} << kHashBits, -1);
    std::vector<i32> prev(src.size(), -1);
    auto insert = [&](size_t pos) {
        if (pos + kMinMatch > src.size())
            return;
        u32 const h = hashAt(pos);
        prev[pos] = head[h];
        head[h] = static_cast<i32>(pos);
    };

    const Codes& c = codes();
    std::vector<u8> out;
    out.reserve(src.size() / 2 + 16);
    out.push_back(kBinaryLiterals);
    out.push_back(static_cast<u8>(dictBits));
    BitWriter bw(out);

    for (size_t pos = 0; pos < src.size();) {
        size_t bestLen = 0;
        size_t bestDist = 0;
        if (pos + kMinMatch <= src.size()) {
            size_t const maxLen = std::min<size_t>(kMaxMatch, src.size() - pos);
            u32 chain = 0;
            for (i32 link = head[hashAt(pos)]; link >= 0 && chain < kMaxChain; link = prev[link], ++chain) {
                size_t const cand = static_cast<size_t>(link);
                if (pos - cand > window)
                    break;
                size_t len = 0;
                while (len < maxLen && src[cand + len] == src[pos + len])
                    ++len;
                if (len > bestLen) {
                    bestLen = len;
                    bestDist = pos - cand;
                    if (len == maxLen)
                        break;
                }
            }
        }

        if (bestLen < kMinMatch) {
            bw.put(0, 1);
            bw.put(src[pos], 8);
            insert(pos++);
            continue;
        }

        u32 const code = lengthCode(static_cast<u32>(bestLen));
        bw.put(1, 1);
        bw.put(c.length, code);
        bw.put(static_cast<u32>(bestLen) - kLenBase[code], kLenExtraBits[code]);
        size_t const d = bestDist - 1;
        bw.put(c.distance, static_cast<u32>(d >> dictBits));
        bw.put(static_cast<u32>(d) & ((1u << dictBits) - 1), dictBits);
        for (size_t end = pos + bestLen; pos < end;)
            insert(pos++);
    }

    u32 const endCode = lengthCode(kEndOfStream);
    bw.put(1, 1);
    bw.put(c.length, endCode);
    bw.put(kEndOfStream - kLenBase[endCode], kLenExtraBits[endCode]);
    bw.finish();
    return out;
}

} // namespace whiteout::storages::mpq
