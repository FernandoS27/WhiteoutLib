// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "root.h"
#include "wow_root.h"
#include "d3_root.h"
#include "tvfs_root.h"
#include "mndx_root.h"
#include "ow_root.h"
#include "s1_root.h"
#include "generic_root.h"
#include "../../common/byte_order.h"

#include <cstring>

namespace whiteout::storages::casc {

using storages::common::readLE32;

std::unique_ptr<RootManifest> RootManifest::parse(std::span<const u8> data) {
    if (data.size() < 4) return nullptr;

    u32 const magic = readLE32(data.data());

    // TVFS root (WC3 Reforged).
    if (magic == RootSignature::kTVFS)
        return TvfsRoot::parse(data);

    // WoW root v2/v3 (build 30080+).
    if (magic == RootSignature::kMFST)
        return WowRoot::parse(data);

    // D3 root — identified by root or subdirectory signature.
    if (magic == RootSignature::kD3Root || magic == RootSignature::kD3Dir)
        return D3Root::parse(data);

    // MNDX root (StarCraft II, Heroes of the Storm).
    if (magic == RootSignature::kMNDX)
        return MndxRoot::parse(data);

    // Overwatch text root (starts with '#').
    if (data[0] == '#')
        return OwRoot::parse(data);

    // S1 / Agent text root (pipe-delimited path|ckey lines).
    if (S1Root::looksLikeS1Root(data))
        return S1Root::parse(data);

    // Fallback: try headerless WoW root (legacy, build 18125+).
    auto wowLegacy = WowRoot::parse(data);
    if (wowLegacy) return wowLegacy;

    // Last resort: generic (empty) root for products with opaque root data.
    return GenericRoot::create();
}

} // namespace whiteout::storages::casc
