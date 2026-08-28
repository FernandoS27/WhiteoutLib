// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file ow_resource_graph.cpp
/// @brief Reading Overwatch's resource graph.

#include "ow_resource_graph.h"

#include "ow_asset_types.h"
#include "ow_manifest_crypto.h"

namespace whiteout::storages::casc::ow {

namespace {

constexpr size_t kPackageSize = 30;
constexpr size_t kSkinHeaderSize = 40;
constexpr size_t kSkinEntrySize = 32;

/// Header field offsets. The gaps are real: the header is mostly zeroes.
constexpr size_t kOffBuild = 0x04;
constexpr size_t kOffPackageCount = 0x18;
constexpr size_t kOffPackageBytes = 0x1C;
constexpr size_t kOffSkinCount = 0x20;
constexpr size_t kOffSkinBytes = 0x24;
constexpr size_t kOffTypeBundleIndexBytes = 0x2C;
constexpr size_t kOffPatch = 0x3C;
constexpr size_t kOffGraphBytes = 0x40;
constexpr size_t kOffMagic = 0x50;

u16 readLE16(const u8* p) {
    return u16(u16(p[0]) | (u16(p[1]) << 8));
}

u32 readLE32(const u8* p) {
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

u64 readLE64(const u8* p) {
    return u64(readLE32(p)) | (u64(readLE32(p + 4)) << 32);
}

} // namespace

bool parseTrgHeader(std::span<const u8> data, TrgHeader& out) {
    if (data.size() < kTrgHeaderSize)
        return false;

    const u8* p = data.data();
    TrgHeader h;
    h.buildVersion = readLE32(p + kOffBuild);
    h.packageCount = readLE32(p + kOffPackageCount);
    h.packageBytes = readLE32(p + kOffPackageBytes);
    h.skinCount = readLE32(p + kOffSkinCount);
    h.skinBytes = readLE32(p + kOffSkinBytes);
    h.typeBundleIndexBytes = readLE32(p + kOffTypeBundleIndexBytes);
    h.patch = readLE32(p + kOffPatch) != 0;
    h.graphBytes = readLE32(p + kOffGraphBytes);
    h.magic = readLE32(p + kOffMagic);

    u32 const plain = trgNonEncryptedMagic(h.magic);
    if (plain != 0x0D747267u)
        return false;
    h.encrypted = h.magic != plain;

    out = h;
    return true;
}

std::optional<ResourceGraph> parseResourceGraph(std::span<u8> data, std::string_view name) {
    ResourceGraph graph;
    if (!parseTrgHeader(data, graph.header))
        return std::nullopt;
    TrgHeader const& h = graph.header;

    // Every block has to fit, and the four together have to account for the
    // file. A plaintext graph ends exactly on the last block; an encrypted one
    // is padded up to a block boundary, and a body that was already aligned
    // gets a whole spare block rather than none.
    size_t const blocks = size_t(h.packageBytes) + h.skinBytes + h.graphBytes +
                          size_t(h.typeBundleIndexBytes);
    if (blocks > data.size() - kTrgHeaderSize)
        return std::nullopt;
    size_t const spare = data.size() - kTrgHeaderSize - blocks;
    if (spare > (h.encrypted ? 16u : 0u))
        return std::nullopt;
    if (size_t(h.packageCount) * kPackageSize != h.packageBytes)
        return std::nullopt;

    auto body = data.subspan(kTrgHeaderSize);
    if (h.encrypted) {
        TrgCryptoHeader const crypto{h.buildVersion, i32(h.packageCount), i32(h.skinCount),
                                     trgNonEncryptedMagic(h.magic)};
        if (!decryptTrgBody(body, crypto, name))
            return std::nullopt;
    }

    graph.packages.reserve(h.packageCount);
    for (u32 i = 0; i < h.packageCount; ++i) {
        const u8* p = body.data() + size_t(i) * kPackageSize;
        TrgPackage pkg;
        pkg.guid = readLE64(p);
        pkg.hash = readLE64(p + 8);
        pkg.graphRef = readLE32(p + 0x10);
        pkg.field14 = readLE32(p + 0x14);
        pkg.field18 = readLE32(p + 0x18);
        pkg.field1c = readLE16(p + 0x1C);
        graph.packages.push_back(pkg);
    }

    // The key schedule ignores the name but the IV is the SHA-1 of it, so a
    // caller that passes the wrong spelling gets a perfectly good graph with
    // one corrupt block at the front — the first package record. Catch that
    // here rather than hand back a package nobody can resolve.
    if (h.encrypted && !graph.packages.empty() && assetTypeId(graph.packages[0].guid) == 0)
        return std::nullopt;

    auto const skinBlock = body.subspan(h.packageBytes, h.skinBytes);
    graph.skins.reserve(h.skinCount);
    size_t off = 0;
    for (u32 i = 0; i < h.skinCount; ++i) {
        if (off + kSkinHeaderSize > skinBlock.size())
            return std::nullopt;
        const u8* p = skinBlock.data() + off;

        TrgSkin skin;
        skin.guid = readLE64(p + 8);
        skin.graphRef = readLE32(p + 0x18);
        u16 const count = readLE16(p + 0x24);

        size_t const bytes = kSkinHeaderSize + size_t(count) * kSkinEntrySize;
        if (off + bytes > skinBlock.size())
            return std::nullopt;

        skin.entries.reserve(count);
        for (u16 e = 0; e < count; ++e) {
            const u8* q = p + kSkinHeaderSize + size_t(e) * kSkinEntrySize;
            skin.entries.push_back({readLE64(q), readLE64(q + 8)});
        }
        graph.skins.push_back(std::move(skin));
        off += bytes;
    }
    if (off != skinBlock.size())
        return std::nullopt;

    return graph;
}

} // namespace whiteout::storages::casc::ow
