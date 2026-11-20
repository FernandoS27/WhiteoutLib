// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file crypto.h
/// @brief MPQ encryption/decryption primitives.
///
/// Internal header — not part of the public include path.

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include <whiteout/common_types.h>

namespace whiteout::storages::mpq {

/// Hash type indices used by hashString().
enum class HashType : u32 {
    TableOffset = 0, ///< Hash → table offset (index into hash table).
    NameA = 1,       ///< Hash → name verification A.
    NameB = 2,       ///< Hash → name verification B.
    FileKey = 3,     ///< Hash → file encryption key.
    TableKey = 4,    ///< Hash → table encryption key.
};

/// Get the global 1280-entry MPQ encryption table (lazy-initialized).
/// The table is shared and immutable after first construction.
const u32* getEncryptionTable();

/// Storm's case folding: ASCII letters to upper case, whatever the C locale
/// says, and '/' to '\\' unless the slashes are kept.
struct NameFold {
    std::array<u8, 256> folded{};    ///< Upper case, '/' as '\\'.
    std::array<u8, 256> slashKept{}; ///< Upper case, '/' as itself.
};
constexpr NameFold buildNameFold() {
    NameFold fold;
    for (u32 c = 0; c < 256; ++c) {
        u8 const upper = static_cast<u8>(c >= 'a' && c <= 'z' ? c - 32 : c);
        fold.slashKept[c] = upper;
        fold.folded[c] = upper == '/' ? u8{'\\'} : upper;
    }
    return fold;
}
inline constexpr NameFold kNameFold = buildNameFold();

/// Compute an MPQ hash of a filename.
/// The filename is normalized (uppercase + '/' → '\\') before hashing.
/// @param filename     Path to hash.
/// @param hashType     One of the HashType enum values (0–4).
/// @param keepSlashes  Hash '/' as itself, as Storm and StormLib do.
/// @return The 32-bit hash value.
u32 hashString(std::string_view filename, HashType hashType, bool keepSlashes = false);

/// What a hash table knows a name by.
struct NameHash {
    u32 offset = 0;        ///< HashType::TableOffset: where its probe starts.
    u32 a = 0;             ///< HashType::NameA.
    u32 b = 0;             ///< HashType::NameB.
    bool hasSlash = false; ///< The name holds a '/', so it has a second spelling.

    /// The two check hashes as one key.
    [[nodiscard]] u64 key() const {
        return (static_cast<u64>(a) << 32) | b;
    }
};

/// The three hashes of hashString() for a table lookup, in one pass.
[[nodiscard]] NameHash hashName(std::string_view filename, bool keepSlashes = false);

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

/// decryptBlock() over the whole words of a byte buffer, in place; a tail
/// of under four bytes is left as it is.
void decryptBytes(std::span<u8> bytes, u32 key);

/// encryptBlock() over the whole words of a byte buffer, in place; a tail
/// of under four bytes is left as it is.
void encryptBytes(std::span<u8> bytes, u32 key);

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
