// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file ow_manifest_crypto.h
/// @brief Overwatch CMF/TRG manifest key and IV generators.
///
/// Blizzard regenerates the key schedule for these manifests every build, so
/// there is one generator pair per build rather than one algorithm. The pairs
/// themselves live in the generated ow_manifest_providers.cpp; this is the
/// lookup over them and the slice of header each one reads.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace whiteout::storages::casc::ow {

/// SHA-1 of the manifest name, which is what the IV generators index into.
inline constexpr size_t kDigestSize = 20;

/// The only CMF header fields any generator reads. Passing these rather than
/// the parsed header keeps the generated file independent of ow_root.h.
struct CmfCryptoHeader {
    u32 buildVersion = 0;
    i32 dataCount = 0;
    i32 entryCount = 0;
    u32 nonEncryptedMagic = 0;
};

/// As CmfCryptoHeader, for the resource graph.
struct TrgCryptoHeader {
    u32 buildVersion = 0;
    i32 packageCount = 0;
    i32 skinCount = 0;
    u32 nonEncryptedMagic = 0;
};

/// A handful of generators read the magic, and they want the plaintext form
/// even when the manifest is encrypted — which stores it byte-reversed, with
/// the layout version moved into the low byte.
constexpr u32 cmfNonEncryptedMagic(u32 magic) {
    u32 const version = ((magic >> 8) == 0x636D66) ? (magic & 0xFF) : (magic >> 24);
    return 0x00666D63u | (version << 24);
}

/// TRG reverses the whole word rather than repacking it.
constexpr u32 trgNonEncryptedMagic(u32 magic) {
    if ((magic >> 8) != 0x677274)
        return magic;
    return (magic >> 24) | ((magic >> 8) & 0xFF00u) | ((magic << 8) & 0xFF0000u) | (magic << 24);
}

/// One build's generator pair. Both return false when the schedule would have
/// indexed out of range or divided by zero — the cases where the C# original
/// throws.
template <typename Header>
struct ManifestProvider {
    u32 build;
    bool (*key)(const Header&, std::span<u8>);
    bool (*iv)(const Header&, std::span<const u8>, std::span<u8>);
};

/// The provider for @p build, or the closest one below it. Null only when
/// @p build predates every provider we carry.
const ManifestProvider<CmfCryptoHeader>* findCmfProvider(u32 build);
const ManifestProvider<TrgCryptoHeader>* findTrgProvider(u32 build);

/// Decrypt a CMF body in place.
///
/// @param body   Everything after the header; whole 16-byte blocks are
///               decrypted and any trailing partial block is left alone.
/// @param header The plaintext header — only the body is ever encrypted.
/// @param name   Manifest file name as the root lists it, with extension and
///               without any directory. Its SHA-1 is the IV material, so the
///               exact spelling matters.
/// @return false if no provider covers the build or the result fails the
///         plausibility check, in which case @p body is left unusable.
bool decryptCmfBody(std::span<u8> body, const CmfCryptoHeader& header, std::string_view name);

/// Decrypt a resource graph body in place, as decryptCmfBody does for a CMF.
///
/// There is no cheap plausibility check here: the graph's own block sizes have
/// to add up before anything can be believed, so parseResourceGraph does the
/// rejecting. This returns false only when no provider covers the build or a
/// key schedule faults outright.
bool decryptTrgBody(std::span<u8> body, const TrgCryptoHeader& header, std::string_view name);

} // namespace whiteout::storages::casc::ow
