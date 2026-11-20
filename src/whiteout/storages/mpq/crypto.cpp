// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "crypto.h"

#include <array>
#include <cstring>

namespace whiteout::storages::mpq {

// ============================================================================
// Encryption Table (1280 entries, 5 × 256)
// ============================================================================

namespace {

std::array<u32, 1280> buildEncryptionTable() {
    std::array<u32, 1280> table{};
    u32 seed = 0x00100001;

    for (u32 index1 = 0; index1 < 256; ++index1) {
        u32 index2 = index1;
        for (u32 i = 0; i < 5; ++i) {
            seed = (seed * 125 + 3) % 0x2AAAAB;
            u32 const temp1 = (seed & 0xFFFF) << 16;

            seed = (seed * 125 + 3) % 0x2AAAAB;
            u32 const temp2 = seed & 0xFFFF;

            table[index2] = temp1 | temp2;
            index2 += 256;
        }
    }
    return table;
}

const std::array<u32, 1280>& encryptionTable() {
    static const auto table = buildEncryptionTable();
    return table;
}

} // anonymous namespace

const u32* getEncryptionTable() {
    return encryptionTable().data();
}

// ============================================================================
// Hash String
// ============================================================================

u32 hashString(std::string_view filename, HashType hashType, bool keepSlashes) {
    const u32* table = encryptionTable().data() + static_cast<u32>(hashType) * 256;
    const auto& fold = keepSlashes ? kNameFold.slashKept : kNameFold.folded;
    u32 seed1 = 0x7FED7FED;
    u32 seed2 = 0xEEEEEEEE;

    for (char const ch : filename) {
        u32 const uc = fold[static_cast<u8>(ch)];
        seed1 = table[uc] ^ (seed1 + seed2);
        seed2 = uc + seed1 + seed2 + (seed2 << 5) + 3;
    }
    return seed1;
}

NameHash hashName(std::string_view filename, bool keepSlashes) {
    const u32* table = encryptionTable().data();
    const auto& fold = keepSlashes ? kNameFold.slashKept : kNameFold.folded;
    // The three hashes share nothing but the character: three chains a pass.
    u32 offset1 = 0x7FED7FED, offset2 = 0xEEEEEEEE;
    u32 a1 = 0x7FED7FED, a2 = 0xEEEEEEEE;
    u32 b1 = 0x7FED7FED, b2 = 0xEEEEEEEE;
    u32 slashes = 0;

    for (char const ch : filename) {
        slashes |= static_cast<u32>(ch == '/');
        u32 const uc = fold[static_cast<u8>(ch)];
        offset1 = table[uc] ^ (offset1 + offset2);
        offset2 = uc + offset1 + offset2 + (offset2 << 5) + 3;
        a1 = table[0x100 + uc] ^ (a1 + a2);
        a2 = uc + a1 + a2 + (a2 << 5) + 3;
        b1 = table[0x200 + uc] ^ (b1 + b2);
        b2 = uc + b1 + b2 + (b2 << 5) + 3;
    }
    return {offset1, a1, b1, slashes != 0};
}

// ============================================================================
// Block Encrypt / Decrypt
// ============================================================================

namespace {

/// The cipher's state between words: each word's key and seed follow from
/// the plaintext of the one before.
class Cipher {
public:
    explicit Cipher(u32 key) : m_key(key) {}

    u32 decrypt(u32 word) {
        m_seed += m_table[0x400 + (m_key & 0xFF)];
        u32 const plain = word ^ (m_key + m_seed);
        advance(plain);
        return plain;
    }
    u32 encrypt(u32 plain) {
        m_seed += m_table[0x400 + (m_key & 0xFF)];
        u32 const word = plain ^ (m_key + m_seed);
        advance(plain);
        return word;
    }

private:
    void advance(u32 plain) {
        m_key = ((~m_key << 21) + 0x11111111) | (m_key >> 11);
        m_seed = plain + m_seed + (m_seed << 5) + 3;
    }

    const std::array<u32, 1280>& m_table = encryptionTable();
    u32 m_key;
    u32 m_seed = 0xEEEEEEEE;
};

/// Each whole word of @p bytes through @p step, by memcpy: a byte buffer is
/// not a u32 array.
template <class Step>
void eachWord(std::span<u8> bytes, Step step) {
    for (size_t at = 0; at + 4 <= bytes.size(); at += 4) {
        u32 word = 0;
        std::memcpy(&word, bytes.data() + at, 4);
        word = step(word);
        std::memcpy(bytes.data() + at, &word, 4);
    }
}

} // anonymous namespace

void decryptBlock(u32* data, size_t count, u32 key) {
    Cipher cipher(key);
    for (size_t i = 0; i < count; ++i)
        data[i] = cipher.decrypt(data[i]);
}

void encryptBlock(u32* data, size_t count, u32 key) {
    Cipher cipher(key);
    for (size_t i = 0; i < count; ++i)
        data[i] = cipher.encrypt(data[i]);
}

void decryptBytes(std::span<u8> bytes, u32 key) {
    Cipher cipher(key);
    eachWord(bytes, [&](u32 word) { return cipher.decrypt(word); });
}

void encryptBytes(std::span<u8> bytes, u32 key) {
    Cipher cipher(key);
    eachWord(bytes, [&](u32 word) { return cipher.encrypt(word); });
}

// ============================================================================
// File Key Detection
// ============================================================================

u32 detectFileKey(const u32* encryptedBlock, u32 sectorSize, u32 fileSize) {
    // The first entry of the sector offset table for a compressed file is the
    // size of the sector offset table itself. For a file with N sectors the
    // table has (N+1) entries × 4 bytes each.
    // N = ceil(fileSize / sectorSize), so table size = (N + 1) * 4.
    u32 const numSectors = (fileSize + sectorSize - 1) / sectorSize;
    u32 const tableSize = (numSectors + 1) * 4;

    const auto& table = encryptionTable();

    // Try all possible encryption keys that would produce the known plaintext
    // at position 0. The decryption formula for the first u32 is:
    //   seed = 0xEEEEEEEE + table[0x400 + (key & 0xFF)]
    //   plaintext = encrypted[0] ^ (key + seed)
    // So: key + seed = encrypted[0] ^ tableSize
    //     key = (encrypted[0] ^ tableSize) - seed
    // We iterate over the 256 possible (key & 0xFF) values.
    u32 const encrypted0 = encryptedBlock[0];
    u32 const encrypted1 = encryptedBlock[1];

    for (u32 keyByte = 0; keyByte < 256; ++keyByte) {
        u32 const seed = 0xEEEEEEEE + table[0x400 + keyByte];
        u32 const candidateKey = (encrypted0 ^ tableSize) - seed;

        if ((candidateKey & 0xFF) != keyByte)
            continue;

        // Verify with the second u32. The second entry of the sector offset
        // table should be <= sectorSize + tableSize (compressed sector can't
        // be larger than uncompressed).
        u32 key2 = candidateKey;
        u32 seed2 = seed;

        // Advance key/seed by one step (after decrypting first value).
        seed2 = tableSize + seed2 + (seed2 << 5) + 3;
        key2 = ((~key2 << 21) + 0x11111111) | (key2 >> 11);
        seed2 += table[0x400 + (key2 & 0xFF)];

        u32 const decrypted1 = encrypted1 ^ (key2 + seed2);

        // Sector offset[1] should be reasonable: > tableSize and
        // <= tableSize + sectorSize (or slightly more due to compression expansion).
        if (decrypted1 > tableSize && decrypted1 <= tableSize + sectorSize) {
            return candidateKey;
        }
    }

    return 0; // Detection failed.
}

} // namespace whiteout::storages::mpq
