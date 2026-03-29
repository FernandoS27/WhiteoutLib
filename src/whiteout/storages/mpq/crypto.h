// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file crypto.h
/// @brief MPQ encryption/decryption primitives.
///
/// Internal header — not part of the public include path.

#pragma once

#include <cstddef>
#include <string>

#include <whiteout/common_types.h>

namespace whiteout::storages::mpq {

/// Hash type indices used by hashString().
struct HashType {
    static constexpr u32 TableOffset = 0; ///< Hash → table offset (index into hash table).
    static constexpr u32 NameA = 1;       ///< Hash → name verification A.
    static constexpr u32 NameB = 2;       ///< Hash → name verification B.
    static constexpr u32 FileKey = 3;     ///< Hash → file encryption key.
    static constexpr u32 TableKey = 4;    ///< Hash → table encryption key.
};

/// Get the global 1280-entry MPQ encryption table (lazy-initialized).
/// The table is shared and immutable after first construction.
const u32* getEncryptionTable();

/// Compute an MPQ hash of a filename.
/// The filename is normalized (uppercase + '/' → '\\') before hashing.
/// @param filename  Path to hash.
/// @param hashType  One of the HashType constants (0–4).
/// @return The 32-bit hash value.
u32 hashString(const std::string& filename, u32 hashType);

/// Decrypt a block of u32 values in-place.
/// @param data   Pointer to the encrypted u32 array.
/// @param count  Number of u32 elements to decrypt.
/// @param key    Decryption key.
void decryptBlock(u32* data, size_t count, u32 key);

/// Encrypt a block of u32 values in-place.
/// @param data   Pointer to the plaintext u32 array.
/// @param count  Number of u32 elements to encrypt.
/// @param key    Encryption key.
void encryptBlock(u32* data, size_t count, u32 key);

/// Attempt to detect the file encryption key from known plaintext.
/// Uses the sector offset table's first entry (which equals the table size
/// in bytes for compressed files).
/// @param encryptedBlock  Pointer to at least 2 encrypted u32 values
///                        (first two entries of the sector offset table).
/// @param sectorSize      Known sector size (512 << sectorSizeShift).
/// @param fileSize        Uncompressed file size from the block table entry.
/// @return The detected key, or 0 if detection failed.
u32 detectFileKey(const u32* encryptedBlock, u32 sectorSize, u32 fileSize);

} // namespace whiteout::storages::mpq
