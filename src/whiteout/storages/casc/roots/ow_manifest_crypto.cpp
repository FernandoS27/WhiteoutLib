// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file ow_manifest_crypto.cpp
/// @brief Driving the per-build CMF key/IV generators.

#include "ow_manifest_crypto.h"

#include "../../common/sha1.h"
#include "../codec/crypto.h"

#include <array>

namespace whiteout::storages::casc::ow {

namespace {

constexpr size_t kBlockSize = 16;
constexpr size_t kEntrySize = 20;

u32 readLE32(const u8* p) {
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

} // namespace

bool decryptCmfBody(std::span<u8> body, const CmfCryptoHeader& header, std::string_view name) {
    const auto* provider = findCmfProvider(header.buildVersion);
    if (provider == nullptr)
        return false;

    auto const digest =
        storages::common::sha1Hash({reinterpret_cast<const u8*>(name.data()), name.size()});

    std::array<u8, 32> key{};
    if (!provider->key(header, key))
        return false;

    // TACTLib substitutes zeros when the IV schedule faults and carries on,
    // because that costs only the first block rather than the whole manifest.
    std::array<u8, kBlockSize> iv{};
    bool const ivValid = provider->iv(header, digest, iv);

    size_t const whole = (body.size() / kBlockSize) * kBlockSize;
    if (whole == 0)
        return false;
    aes256CbcDecrypt(body.first(whole), std::span<const u8, 32>(key),
                     std::span<const u8, kBlockSize>(iv));

    // The entry table leads the body and its first record is always index 1.
    // TACTLib only warns about this; we treat it as fatal because our provider
    // lookup deliberately falls back to the closest build below, and a guess
    // that cannot be rejected is worse than no manifest at all. Skipped when
    // the IV faulted, since then the first block is known-bad but the rest of
    // the manifest is still sound.
    if (ivValid && header.entryCount > 0) {
        if (body.size() < kEntrySize || readLE32(body.data()) != 1)
            return false;
    }
    return true;
}

} // namespace whiteout::storages::casc::ow
