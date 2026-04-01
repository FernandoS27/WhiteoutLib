// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "root.h"
#include "wow_root.h"
#include "d3_root.h"
#include "tvfs_root.h"
#include "mndx_root.h"
#include "../../common/byte_order.h"

#include <cstring>

namespace whiteout::storages::casc {

using storages::common::readLE32;

std::unique_ptr<RootManifest> RootManifest::parse(std::span<const u8> data) {
    if (data.size() < 4) return nullptr;

    u32 magic = readLE32(data.data());

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

    // Fallback: try headerless WoW root (legacy, build 18125+).
    auto wowLegacy = WowRoot::parse(data);
    if (wowLegacy) return wowLegacy;

    return nullptr;
}

} // namespace whiteout::storages::casc
