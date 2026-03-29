// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "crypto.h"

#include <array>
#include <cctype>

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
            u32 temp1 = (seed & 0xFFFF) << 16;

            seed = (seed * 125 + 3) % 0x2AAAAB;
            u32 temp2 = seed & 0xFFFF;

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

u32 hashString(const std::string& filename, u32 hashType) {
    const auto& table = encryptionTable();
    u32 seed1 = 0x7FED7FED;
    u32 seed2 = 0xEEEEEEEE;

    for (char ch : filename) {
        // Normalize: uppercase + '/' → '\\'
        unsigned char uc = static_cast<unsigned char>(ch);
        if (uc == '/') uc = '\\';
        uc = static_cast<unsigned char>(std::toupper(uc));

        seed1 = table[hashType * 256 + uc] ^ (seed1 + seed2);
        seed2 = static_cast<u32>(uc) + seed1 + seed2 + (seed2 << 5) + 3;
    }
    return seed1;
}

// ============================================================================
// Block Encrypt / Decrypt
// ============================================================================

void decryptBlock(u32* data, size_t count, u32 key) {
    const auto& table = encryptionTable();
    u32 seed = 0xEEEEEEEE;

    for (size_t i = 0; i < count; ++i) {
        seed += table[0x400 + (key & 0xFF)];
        u32 ch = data[i] ^ (key + seed);
        key = ((~key << 21) + 0x11111111) | (key >> 11);
        seed = ch + seed + (seed << 5) + 3;
        data[i] = ch;
    }
}

void encryptBlock(u32* data, size_t count, u32 key) {
    const auto& table = encryptionTable();
    u32 seed = 0xEEEEEEEE;

    for (size_t i = 0; i < count; ++i) {
        seed += table[0x400 + (key & 0xFF)];
        u32 ch = data[i];
        data[i] = ch ^ (key + seed);
        key = ((~key << 21) + 0x11111111) | (key >> 11);
        seed = ch + seed + (seed << 5) + 3;
    }
}

// ============================================================================
// File Key Detection
// ============================================================================

u32 detectFileKey(const u32* encryptedBlock, u32 sectorSize, u32 fileSize) {
    // The first entry of the sector offset table for a compressed file is the
    // size of the sector offset table itself. For a file with N sectors the
    // table has (N+1) entries × 4 bytes each.
    // N = ceil(fileSize / sectorSize), so table size = (N + 1) * 4.
    u32 numSectors = (fileSize + sectorSize - 1) / sectorSize;
    u32 tableSize = (numSectors + 1) * 4;

    const auto& table = encryptionTable();

    // Try all possible encryption keys that would produce the known plaintext
    // at position 0. The decryption formula for the first u32 is:
    //   seed = 0xEEEEEEEE + table[0x400 + (key & 0xFF)]
    //   plaintext = encrypted[0] ^ (key + seed)
    // So: key + seed = encrypted[0] ^ tableSize
    //     key = (encrypted[0] ^ tableSize) - seed
    // We iterate over the 256 possible (key & 0xFF) values.
    u32 encrypted0 = encryptedBlock[0];
    u32 encrypted1 = encryptedBlock[1];

    for (u32 keyByte = 0; keyByte < 256; ++keyByte) {
        u32 seed = 0xEEEEEEEE + table[0x400 + keyByte];
        u32 candidateKey = (encrypted0 ^ tableSize) - seed;

        if ((candidateKey & 0xFF) != keyByte) continue;

        // Verify with the second u32. The second entry of the sector offset
        // table should be <= sectorSize + tableSize (compressed sector can't
        // be larger than uncompressed).
        u32 key2 = candidateKey;
        u32 seed2 = seed;

        // Advance key/seed by one step (after decrypting first value).
        seed2 = tableSize + seed2 + (seed2 << 5) + 3;
        key2 = ((~key2 << 21) + 0x11111111) | (key2 >> 11);
        seed2 += table[0x400 + (key2 & 0xFF)];

        u32 decrypted1 = encrypted1 ^ (key2 + seed2);

        // Sector offset[1] should be reasonable: > tableSize and
        // <= tableSize + sectorSize (or slightly more due to compression expansion).
        if (decrypted1 > tableSize && decrypted1 <= tableSize + sectorSize) {
            return candidateKey;
        }
    }

    return 0; // Detection failed.
}

} // namespace whiteout::storages::mpq
