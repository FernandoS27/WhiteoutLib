// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../../common/unicode_path.h"
#include "../../common/hex.h"
#include "crypto.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

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

// Salsa20 "sigma" and "tau" constants for key expansion.
// sigma = "expand 32-byte k"
constexpr u32 kSigma0 = 0x61707865; // "expa"
constexpr u32 kSigma1 = 0x3320646E; // "nd 3"
constexpr u32 kSigma2 = 0x79622D32; // "2-by"
constexpr u32 kSigma3 = 0x6B206574; // "te k"

} // anonymous namespace

void salsa20Decrypt(std::span<u8> data, std::span<const u8, 16> key, std::span<const u8, 8> iv) {
    if (data.empty())
        return;

    // For 128-bit CASC keys, expand to 256-bit by repeating: key||key.
    // Salsa20 state layout (256-bit key):
    //  [sigma0] [key0]   [key1]   [key2]
    //  [key3]   [sigma1] [nonce0] [nonce1]
    //  [ctr0]   [ctr1]   [sigma2] [key4]
    //  [key5]   [key6]   [key7]   [sigma3]
    u32 state[16];
    state[0] = kSigma0;
    state[1] = leLoad32(key.data());
    state[2] = leLoad32(key.data() + 4);
    state[3] = leLoad32(key.data() + 8);
    state[4] = leLoad32(key.data() + 12);
    state[5] = kSigma1;
    state[6] = leLoad32(iv.data());
    state[7] = leLoad32(iv.data() + 4);
    state[8] = 0; // Counter low.
    state[9] = 0; // Counter high.
    state[10] = kSigma2;
    // Repeat key for 256-bit expansion.
    state[11] = leLoad32(key.data());
    state[12] = leLoad32(key.data() + 4);
    state[13] = leLoad32(key.data() + 8);
    state[14] = leLoad32(key.data() + 12);
    state[15] = kSigma3;

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

} // namespace whiteout::storages::casc
