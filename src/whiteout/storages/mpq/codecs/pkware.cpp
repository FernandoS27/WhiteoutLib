// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "pkware.h"

#include "../../../common/output_reserve.h"

#include <algorithm>
#include <array>

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

class BitReader {
public:
    explicit BitReader(std::span<const u8> src) : m_src(src) {}

    /// The next @p count (<= 16) bits, zeros past the end of the input.
    u32 peek(u32 count) {
        refill();
        return static_cast<u32>(m_buffer) & ((1u << count) - 1);
    }

    /// Consumes @p count bits; false when the input runs out first.
    bool skip(u32 count) {
        refill();
        if (count > m_count)
            return false;
        m_buffer >>= count;
        m_count -= count;
        return true;
    }

    bool read(u32 count, u32& value) {
        value = peek(count);
        return skip(count);
    }

    template <size_t N, u32 MaxBits>
    bool decode(const PrefixCode<N, MaxBits>& code, u32& symbol) {
        auto const entry = code.lookup[peek(MaxBits)];
        symbol = entry.symbol;
        return entry.bits != 0 && skip(entry.bits);
    }

private:
    void refill() {
        while (m_count <= 56 && m_pos < m_src.size()) {
            m_buffer |= static_cast<u64>(m_src[m_pos++]) << m_count;
            m_count += 8;
        }
    }

    std::span<const u8> m_src;
    size_t m_pos = 0;
    u64 m_buffer = 0;
    u32 m_count = 0;
};

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

std::vector<u8> pkwareExplode(std::span<const u8> src, size_t expectedSize) {
    if (src.size() < 2)
        return {};
    u8 const literalMode = src[0];
    u32 const dictBits = src[1];
    if (literalMode > kAsciiLiterals || dictBits < kMinDictBits || dictBits > kMaxDictBits)
        return {};

    const Codes& c = codes();
    BitReader br(src.subspan(2));
    std::vector<u8> out;
    out.reserve(whiteout::common::outputReserve(expectedSize, src.size()));

    while (out.size() < expectedSize) {
        u32 isCopy = 0;
        if (!br.read(1, isCopy))
            return {};
        if (!isCopy) {
            u32 literal = 0;
            bool const ok = literalMode == kAsciiLiterals ? br.decode(c.ascii, literal) : br.read(8, literal);
            if (!ok)
                return {};
            out.push_back(static_cast<u8>(literal));
            continue;
        }

        u32 code = 0;
        u32 extra = 0;
        if (!br.decode(c.length, code) || !br.read(kLenExtraBits[code], extra))
            return {};
        u32 const length = kLenBase[code] + extra;
        if (length == kEndOfStream)
            break;

        u32 const lowBits = length == 2 ? kShortMatchDistBits : dictBits;
        u32 high = 0;
        u32 low = 0;
        if (!br.decode(c.distance, high) || !br.read(lowBits, low))
            return {};
        size_t const distance = ((size_t{high} << lowBits) | low) + 1;
        if (distance > out.size())
            return {};

        size_t const from = out.size() - distance;
        size_t const count = std::min<size_t>(length, expectedSize - out.size());
        for (size_t i = 0; i < count; ++i) {
            u8 const byte = out[from + i];
            out.push_back(byte);
        }
    }
    return out;
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
