// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../../common/unicode_path.h"
#include "../../common/hex.h"
#include "crypto.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define WHITEOUT_AES_X86 1
#include <wmmintrin.h>
#if defined(_WIN32)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
// MSVC allows the AES intrinsics unconditionally; GCC and Clang need the
// function that uses them to opt into the instruction set.
#if defined(__GNUC__) || defined(__clang__)
#define WHITEOUT_TARGET_AES __attribute__((target("aes,sse2")))
#else
#define WHITEOUT_TARGET_AES
#endif
#else
#define WHITEOUT_AES_X86 0
#endif

namespace whiteout::storages::casc {

// ============================================================================
// KeyRing
// ============================================================================

void KeyRing::addKey(u64 keyName, std::array<u8, 16> key) {
    m_keys[keyName] = key;
}

void KeyRing::addKey(u64 keyName, const std::string& hexKey) {
    std::array<u8, 16> key{};
    if (storages::common::hexDecode(hexKey, key.data(), 16))
        m_keys[keyName] = key;
}

bool KeyRing::importFromString(const std::string& keyList) {
    bool imported = false;
    std::istringstream ss(keyList);
    std::string line;
    while (std::getline(ss, line)) {
        // Trim leading/trailing whitespace.
        size_t const start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            continue;
        size_t const end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);

        // Skip comments and empty lines.
        if (line.empty() || line[0] == '#')
            continue;

        // Parse: keyNameHex keyValueHex
        size_t const sep = line.find_first_of(" \t");
        if (sep == std::string::npos)
            continue;
        std::string const nameHex = line.substr(0, sep);
        std::string const valueHex = line.substr(line.find_first_not_of(" \t", sep));

        u64 const keyName = storages::common::hexToU64(nameHex);
        std::array<u8, 16> key{};
        if (storages::common::hexDecode(valueHex, key.data(), 16)) {
            m_keys[keyName] = key;
            imported = true;
        }
    }
    return imported;
}

bool KeyRing::importFromFile(const std::string& path) {
    auto file = whiteout::common::open_ifstream(path);
    if (!file.is_open())
        return false;
    std::string const content((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
    return importFromString(content);
}

const std::array<u8, 16>* KeyRing::findKey(u64 keyName) const {
    auto it = m_keys.find(keyName);
    if (it != m_keys.end())
        return &it->second;
    // Record first miss atomically (first-writer-wins).
    u64 expected = 0;
    m_firstMissing.compare_exchange_strong(expected, keyName, std::memory_order_relaxed,
                                           std::memory_order_relaxed);
    return nullptr;
}

std::optional<u64> KeyRing::firstMissingKey() const {
    u64 val = m_firstMissing.load(std::memory_order_relaxed);
    if (val == 0)
        return std::nullopt;
    return val;
}

// ============================================================================
// Salsa20 (20-round, 256-bit key variant)
// ============================================================================

namespace {

inline u32 rotl32(u32 v, int n) {
    return (v << n) | (v >> (32 - n));
}

inline u32 leLoad32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

inline void leStore32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}

void salsa20QuarterRound(u32& a, u32& b, u32& c, u32& d) {
    b ^= rotl32(a + d, 7);
    c ^= rotl32(b + a, 9);
    d ^= rotl32(c + b, 13);
    a ^= rotl32(d + c, 18);
}

void salsa20Block(const u32 input[16], u8 output[64]) {
    u32 x[16];
    std::memcpy(x, input, 64);

    // 20 rounds (10 double rounds).
    for (int i = 0; i < 10; ++i) {
        // Column rounds.
        salsa20QuarterRound(x[0], x[4], x[8], x[12]);
        salsa20QuarterRound(x[5], x[9], x[13], x[1]);
        salsa20QuarterRound(x[10], x[14], x[2], x[6]);
        salsa20QuarterRound(x[15], x[3], x[7], x[11]);
        // Row rounds.
        salsa20QuarterRound(x[0], x[1], x[2], x[3]);
        salsa20QuarterRound(x[5], x[6], x[7], x[4]);
        salsa20QuarterRound(x[10], x[11], x[8], x[9]);
        salsa20QuarterRound(x[15], x[12], x[13], x[14]);
    }

    for (int i = 0; i < 16; ++i)
        leStore32(output + i * 4, x[i] + input[i]);
}

// Salsa20 key-expansion constants. A CASC key is 128-bit, and Salsa20 spells
// that with "tau" and the key repeated across both halves of the state —
// *not* with "sigma", which belongs to the 256-bit variant. Getting this wrong
// produces a perfectly self-consistent keystream that is not Salsa20's, so a
// round-trip test passes and every real file decrypts to noise.
constexpr u32 kTau0 = 0x61707865; // "expa"
constexpr u32 kTau1 = 0x3120646E; // "nd 1"
constexpr u32 kTau2 = 0x79622D36; // "6-by"
constexpr u32 kTau3 = 0x6B206574; // "te k"

} // anonymous namespace

void salsa20Decrypt(std::span<u8> data, std::span<const u8, 16> key, std::span<const u8, 8> iv) {
    if (data.empty())
        return;

    // Salsa20 state layout, 128-bit key (the key occupies both halves):
    //  [tau0]  [key0]  [key1]  [key2]
    //  [key3]  [tau1]  [nonce0][nonce1]
    //  [ctr0]  [ctr1]  [tau2]  [key0]
    //  [key1]  [key2]  [key3]  [tau3]
    u32 state[16];
    state[0] = kTau0;
    state[1] = leLoad32(key.data());
    state[2] = leLoad32(key.data() + 4);
    state[3] = leLoad32(key.data() + 8);
    state[4] = leLoad32(key.data() + 12);
    state[5] = kTau1;
    state[6] = leLoad32(iv.data());
    state[7] = leLoad32(iv.data() + 4);
    state[8] = 0; // Counter low.
    state[9] = 0; // Counter high.
    state[10] = kTau2;
    state[11] = leLoad32(key.data());
    state[12] = leLoad32(key.data() + 4);
    state[13] = leLoad32(key.data() + 8);
    state[14] = leLoad32(key.data() + 12);
    state[15] = kTau3;

    u8 block[64];
    size_t offset = 0;
    while (offset < data.size()) {
        salsa20Block(state, block);

        size_t const chunkSize = std::min<size_t>(64, data.size() - offset);
        for (size_t i = 0; i < chunkSize; ++i)
            data[offset + i] ^= block[i];

        offset += chunkSize;

        // Increment counter (little-endian u64).
        state[8]++;
        if (state[8] == 0)
            state[9]++;
    }
}

// ============================================================================
// ARC4 (RC4)
// ============================================================================

void arc4Transform(std::span<u8> data, std::span<const u8> key) {
    if (data.empty() || key.empty())
        return;

    // KSA (Key Scheduling Algorithm).
    u8 S[256];
    for (int i = 0; i < 256; ++i)
        S[i] = static_cast<u8>(i);

    u8 j = 0;
    for (int i = 0; i < 256; ++i) {
        j = static_cast<u8>(j + S[i] + key[i % key.size()]);
        std::swap(S[i], S[j]);
    }

    // PRGA (Pseudo-Random Generation Algorithm).
    u8 ii = 0;
    j = 0;
    for (size_t k = 0; k < data.size(); ++k) {
        ii = static_cast<u8>(ii + 1);
        j = static_cast<u8>(j + S[ii]);
        std::swap(S[ii], S[j]);
        u8 const keyByte = S[static_cast<u8>(S[ii] + S[j])];
        data[k] ^= keyByte;
    }
}

// ============================================================================
// AES-256-CBC
// ============================================================================

namespace {

constexpr u8 kSBox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};

/// Inverted rather than transcribed: a second 256-byte constant is a second
/// chance to get a byte wrong, and the inverse is defined by the forward table.
constexpr std::array<u8, 256> makeInvSBox() {
    std::array<u8, 256> inv{};
    for (int i = 0; i < 256; ++i)
        inv[kSBox[i]] = u8(i);
    return inv;
}
constexpr std::array<u8, 256> kInvSBox = makeInvSBox();

constexpr u8 xtime(u8 x) {
    return u8((x << 1) ^ ((x >> 7) * 0x1B));
}

constexpr u8 gmul(u8 a, u8 b) {
    u8 r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

/// InvMixColumns multiplies every byte by 9, 11, 13 and 14 in GF(2^8), which
/// with a shift-and-add loop costs more than the whole rest of the round. The
/// products are only 1 KB in total, so they are tabulated at compile time.
constexpr std::array<std::array<u8, 256>, 4> makeInvMixTables() {
    constexpr u8 coeff[4] = {9, 11, 13, 14};
    std::array<std::array<u8, 256>, 4> t{};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 256; ++i)
            t[size_t(c)][size_t(i)] = gmul(u8(i), coeff[c]);
    return t;
}
constexpr std::array<std::array<u8, 256>, 4> kInvMix = makeInvMixTables();
constexpr const std::array<u8, 256>& kMul9 = kInvMix[0];
constexpr const std::array<u8, 256>& kMul11 = kInvMix[1];
constexpr const std::array<u8, 256>& kMul13 = kInvMix[2];
constexpr const std::array<u8, 256>& kMul14 = kInvMix[3];

constexpr size_t kAesRounds = 14;
constexpr size_t kExpandedKeySize = (kAesRounds + 1) * 16;

void expandKey256(const u8* key, u8* rk) {
    std::memcpy(rk, key, 32);
    u8 rcon = 1;
    for (size_t i = 8; i < kExpandedKeySize / 4; ++i) {
        u8 t[4];
        std::memcpy(t, rk + (i - 1) * 4, 4);
        if (i % 8 == 0) {
            u8 const first = t[0];
            t[0] = u8(kSBox[t[1]] ^ rcon);
            t[1] = kSBox[t[2]];
            t[2] = kSBox[t[3]];
            t[3] = kSBox[first];
            rcon = xtime(rcon);
        } else if (i % 8 == 4) {
            for (u8& b : t)
                b = kSBox[b];
        }
        for (int j = 0; j < 4; ++j)
            rk[i * 4 + j] = u8(rk[(i - 8) * 4 + j] ^ t[j]);
    }
}

void addRoundKey(u8* state, const u8* rk) {
    for (int i = 0; i < 16; ++i)
        state[i] ^= rk[i];
}

void invShiftRows(u8* state) {
    u8 t[16];
    std::memcpy(t, state, 16);
    for (int r = 1; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            state[4 * ((c + r) & 3) + r] = t[4 * c + r];
}

void invSubBytes(u8* state) {
    for (int i = 0; i < 16; ++i)
        state[i] = kInvSBox[state[i]];
}

void invMixColumns(u8* state) {
    for (int c = 0; c < 4; ++c) {
        u8* p = state + c * 4;
        u8 const a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = u8(kMul14[a0] ^ kMul11[a1] ^ kMul13[a2] ^ kMul9[a3]);
        p[1] = u8(kMul9[a0] ^ kMul14[a1] ^ kMul11[a2] ^ kMul13[a3]);
        p[2] = u8(kMul13[a0] ^ kMul9[a1] ^ kMul14[a2] ^ kMul11[a3]);
        p[3] = u8(kMul11[a0] ^ kMul13[a1] ^ kMul9[a2] ^ kMul14[a3]);
    }
}

void decryptBlock(const u8* rk, u8* state) {
    addRoundKey(state, rk + kAesRounds * 16);
    for (size_t round = kAesRounds - 1; round > 0; --round) {
        invShiftRows(state);
        invSubBytes(state);
        addRoundKey(state, rk + round * 16);
        invMixColumns(state);
    }
    invShiftRows(state);
    invSubBytes(state);
    addRoundKey(state, rk);
}

#if WHITEOUT_AES_X86

/// True when the CPU implements the AES-NI instruction set.
bool detectAesNi() {
#if defined(_WIN32)
    int regs[4]{};
    __cpuid(regs, 1);
    return (regs[2] & (1 << 25)) != 0;
#else
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d) == 0)
        return false;
    return (c & (1u << 25)) != 0;
#endif
}

/// CBC decryption is only chained in the final XOR — the block decryptions
/// themselves are independent, so four run at once to fill the pipeline.
WHITEOUT_TARGET_AES
void cbcDecryptAesNi(std::span<u8> data, std::span<const u8, 32> key, std::span<const u8, 16> iv) {
    u8 rk[kExpandedKeySize];
    expandKey256(key.data(), rk);

    // AES-NI decrypts with the equivalent inverse cipher: the encryption round
    // keys in reverse, all but the outermost two passed through InvMixColumns.
    __m128i dk[kAesRounds + 1];
    dk[0] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rk + kAesRounds * 16));
    for (size_t i = 1; i < kAesRounds; ++i)
        dk[i] = _mm_aesimc_si128(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(rk + (kAesRounds - i) * 16)));
    dk[kAesRounds] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rk));

    __m128i chain = _mm_loadu_si128(reinterpret_cast<const __m128i*>(iv.data()));
    size_t const blocks = data.size() / 16;
    size_t b = 0;

    for (; b + 4 <= blocks; b += 4) {
        auto* p = reinterpret_cast<__m128i*>(data.data() + b * 16);
        __m128i const c0 = _mm_loadu_si128(p);
        __m128i const c1 = _mm_loadu_si128(p + 1);
        __m128i const c2 = _mm_loadu_si128(p + 2);
        __m128i const c3 = _mm_loadu_si128(p + 3);
        __m128i x0 = _mm_xor_si128(c0, dk[0]);
        __m128i x1 = _mm_xor_si128(c1, dk[0]);
        __m128i x2 = _mm_xor_si128(c2, dk[0]);
        __m128i x3 = _mm_xor_si128(c3, dk[0]);
        for (size_t r = 1; r < kAesRounds; ++r) {
            x0 = _mm_aesdec_si128(x0, dk[r]);
            x1 = _mm_aesdec_si128(x1, dk[r]);
            x2 = _mm_aesdec_si128(x2, dk[r]);
            x3 = _mm_aesdec_si128(x3, dk[r]);
        }
        x0 = _mm_aesdeclast_si128(x0, dk[kAesRounds]);
        x1 = _mm_aesdeclast_si128(x1, dk[kAesRounds]);
        x2 = _mm_aesdeclast_si128(x2, dk[kAesRounds]);
        x3 = _mm_aesdeclast_si128(x3, dk[kAesRounds]);
        _mm_storeu_si128(p, _mm_xor_si128(x0, chain));
        _mm_storeu_si128(p + 1, _mm_xor_si128(x1, c0));
        _mm_storeu_si128(p + 2, _mm_xor_si128(x2, c1));
        _mm_storeu_si128(p + 3, _mm_xor_si128(x3, c2));
        chain = c3;
    }

    for (; b < blocks; ++b) {
        auto* p = reinterpret_cast<__m128i*>(data.data() + b * 16);
        __m128i const c = _mm_loadu_si128(p);
        __m128i x = _mm_xor_si128(c, dk[0]);
        for (size_t r = 1; r < kAesRounds; ++r)
            x = _mm_aesdec_si128(x, dk[r]);
        x = _mm_aesdeclast_si128(x, dk[kAesRounds]);
        _mm_storeu_si128(p, _mm_xor_si128(x, chain));
        chain = c;
    }
}

#endif // WHITEOUT_AES_X86

} // namespace

void aes256CbcDecryptPortable(std::span<u8> data, std::span<const u8, 32> key,
                              std::span<const u8, 16> iv) {
    u8 rk[kExpandedKeySize];
    expandKey256(key.data(), rk);

    u8 chain[16];
    std::memcpy(chain, iv.data(), 16);

    size_t const blocks = data.size() / 16;
    for (size_t b = 0; b < blocks; ++b) {
        u8* p = data.data() + b * 16;
        u8 next[16];
        std::memcpy(next, p, 16);
        decryptBlock(rk, p);
        for (int i = 0; i < 16; ++i)
            p[i] ^= chain[i];
        std::memcpy(chain, next, 16);
    }
}

AesBackend aesBackend() noexcept {
#if WHITEOUT_AES_X86
    static bool const accelerated = detectAesNi();
    if (accelerated)
        return AesBackend::AesNi;
#endif
    return AesBackend::Portable;
}

void aes256CbcDecrypt(std::span<u8> data, std::span<const u8, 32> key, std::span<const u8, 16> iv) {
#if WHITEOUT_AES_X86
    if (aesBackend() == AesBackend::AesNi) {
        cbcDecryptAesNi(data, key, iv);
        return;
    }
#endif
    aes256CbcDecryptPortable(data, key, iv);
}

} // namespace whiteout::storages::casc
