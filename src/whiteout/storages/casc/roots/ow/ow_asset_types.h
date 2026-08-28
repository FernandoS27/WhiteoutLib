// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file ow_asset_types.h
/// @brief Names for Overwatch's asset types, so its GUIDs read as filenames.
///
/// Overwatch stores no filenames. Every asset is a bare 64-bit GUID, and the
/// only structure in one is a type. The client keeps the names; the data does
/// not, so these come from the client itself rather than from guessing at file
/// contents -- see tools/ow_gen_asset_types.py, which generates this.
///
/// The type is not the leading hex digits of the GUID. The client bit-reverses
/// the top 16 bits and adds one, so the `0D00` an asset prints with is type
/// 0x00C, MODEL. Reversing is what keeps the ids small enough to index a table.
///
/// Source: Overwatch.exe (prometheus-2_24_0_3-152850, decrypted)
///   type id -> descriptor index  0x143A12420, 403 x u32, typeId -> descriptor index
///   descriptor registrar         0x141FFAF10, descriptor +0 = name, +8 = descriptor index
///   GUID -> type id              0x142011C90: bitreverse12(guid >> 48 & 0xFFF) + 1
///
/// 240 of 403 type ids are registered; the rest are unused by this
/// build and keep their type id as the extension.
///
/// Internal header -- not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace whiteout::storages::casc::ow {

/// Largest type id this build of the client will accept.
inline constexpr u32 kMaxAssetTypeId = 0x192;

/// The raw 12-bit field a GUID prints with, before the client unscrambles it.
constexpr u32 assetTypeField(u64 guid) {
    return u32((guid >> 48) & 0xFFF);
}

/// The client's asset type id, or 0 for a GUID it would reject.
constexpr u32 assetTypeId(u64 guid) {
    u32 reversed = 0;
    u32 field = assetTypeField(guid);
    for (int i = 0; i < 12; ++i, field >>= 1)
        reversed = (reversed << 1) | (field & 1);
    u32 const id = reversed + 1;
    return id > kMaxAssetTypeId ? 0 : id;
}

/// Sorted by type id so lookup is a binary search.
inline constexpr std::array<std::pair<u32, std::string_view>, 240> kAssetTypeNames{{
    {0x001, "stu"},
    {0x002, "mapdata"},
    {0x003, "stu"},
    {0x004, "txtr"},
    {0x006, "anim"},
    {0x007, "skeleton"},
    {0x008, "material"},
    {0x00C, "model"},
    {0x00D, "effect"},
    {0x00E, "lightingdata"},
    {0x010, "displaytext"},
    {0x015, "stu"},
    {0x017, "stu"},
    {0x018, "stu"},
    {0x01A, "stu"},
    {0x01B, "stu"},
    {0x01F, "stu"},
    {0x020, "stu"},
    {0x021, "stu"},
    {0x024, "stu"},
    {0x027, "cursor"},
    {0x02B, "stu"},
    {0x02C, "stu"},
    {0x02D, "stu"},
    {0x02E, "stu"},
    {0x02F, "stu"},
    {0x030, "stu"},
    {0x031, "stu"},
    {0x032, "stu"},
    {0x033, "stu"},
    {0x039, "stu"},
    {0x03A, "stu"},
    {0x03B, "stu"},
    {0x03F, "soundwemfile"},
    {0x040, "shader_reflection_data"},
    {0x043, "soundbank"},
    {0x045, "stu"},
    {0x049, "stu"},
    {0x04A, "effect"},
    {0x04C, "stu"},
    {0x04D, "txtr_payload"},
    {0x04E, "stu"},
    {0x050, "font"},
    {0x051, "stu"},
    {0x053, "stu"},
    {0x054, "stu"},
    {0x055, "stu"},
    {0x058, "stu"},
    {0x05B, "stu"},
    {0x05C, "stu"},
    {0x05D, "stu"},
    {0x05F, "stu"},
    {0x062, "stu"},
    {0x063, "catalog"},
    {0x065, "material"},
    {0x066, "stu"},
    {0x067, "stu"},
    {0x068, "stu"},
    {0x069, "stu"},
    {0x06F, "stu"},
    {0x070, "stu"},
    {0x071, "voicetext"},
    {0x072, "stu"},
    {0x075, "stu"},
    {0x077, "stu"},
    {0x078, "stu"},
    {0x079, "stu"},
    {0x07A, "stu"},
    {0x07C, "displaytext"},
    {0x07F, "stu"},
    {0x081, "stu"},
    {0x083, "stu"},
    {0x084, "stu"},
    {0x085, "shader_group"},
    {0x087, "shader_code"},
    {0x088, "shader_group"},
    {0x089, "stu"},
    {0x08E, "effect"},
    {0x08F, "effect"},
    {0x090, "stu"},
    {0x091, "stu"},
    {0x095, "stu"},
    {0x096, "stu"},
    {0x097, "stu"},
    {0x098, "stu"},
    {0x09B, "stu"},
    {0x09C, "binary_package_data"},
    {0x09D, "stu"},
    {0x09E, "stu"},
    {0x09F, "stu"},
    {0x0A0, "stu"},
    {0x0A2, "stu"},
    {0x0A3, "stu"},
    {0x0A5, "stu"},
    {0x0A6, "stu"},
    {0x0A8, "stu"},
    {0x0A9, "displaytext"},
    {0x0AA, "stu"},
    {0x0AB, "stu"},
    {0x0AC, "stu"},
    {0x0AD, "stu"},
    {0x0AE, "stu"},
    {0x0B2, "voicewemfile"},
    {0x0B3, "material_data"},
    {0x0B5, "stu"},
    {0x0B6, "binary_package_data"},
    {0x0B9, "stu"},
    {0x0BB, "soundwemfile"},
    {0x0BC, "mapchunk"},
    {0x0BD, "lightingmanifest"},
    {0x0BE, "lightingchunk"},
    {0x0BF, "stu"},
    {0x0C0, "stu"},
    {0x0C1, "stu"},
    {0x0C2, "stu"},
    {0x0C3, "stu"},
    {0x0C5, "stu"},
    {0x0C6, "stu"},
    {0x0C7, "stu"},
    {0x0C8, "stu"},
    {0x0C9, "stu"},
    {0x0CA, "stu"},
    {0x0CB, "mapshadowdata"},
    {0x0CC, "stu"},
    {0x0CD, "stu"},
    {0x0CE, "stu"},
    {0x0CF, "stu"},
    {0x0D0, "stu"},
    {0x0D3, "stu"},
    {0x0D4, "sequence"},
    {0x0D5, "stu"},
    {0x0D6, "stu"},
    {0x0D7, "stu"},
    {0x0D8, "stu"},
    {0x0D9, "stu"},
    {0x0DA, "stu"},
    {0x0DB, "stu"},
    {0x0DC, "stu"},
    {0x0DF, "stu"},
    {0x0EA, "stu"},
    {0x0EB, "stu"},
    {0x0EC, "stu"},
    {0x0EE, "stu"},
    {0x0F1, "txtr"},
    {0x0F6, "stu"},
    {0x0F7, "stu"},
    {0x0F8, "stu"},
    {0x0F9, "stu"},
    {0x103, "stu"},
    {0x104, "stu"},
    {0x105, "stu"},
    {0x10B, "stu"},
    {0x10C, "shader_root_signature"},
    {0x10D, "stu"},
    {0x10E, "stu"},
    {0x10F, "stu"},
    {0x112, "fracturable"},
    {0x114, "stu"},
    {0x116, "stu"},
    {0x117, "stu"},
    {0x118, "model"},
    {0x119, "stu"},
    {0x11A, "stu"},
    {0x11C, "stu"},
    {0x11D, "stu"},
    {0x11E, "stu"},
    {0x11F, "stu"},
    {0x120, "stu"},
    {0x122, "catalog"},
    {0x123, "stu"},
    {0x124, "stu"},
    {0x126, "stu"},
    {0x127, "material"},
    {0x129, "stu"},
    {0x12A, "stu"},
    {0x12B, "effect"},
    {0x12C, "stu"},
    {0x12E, "stu"},
    {0x130, "stu"},
    {0x132, "stu"},
    {0x133, "stu"},
    {0x134, "stu"},
    {0x136, "stu"},
    {0x139, "stu"},
    {0x13A, "stu"},
    {0x13C, "stu"},
    {0x13D, "stu"},
    {0x143, "stu"},
    {0x144, "stu"},
    {0x145, "stu"},
    {0x147, "stu"},
    {0x148, "stu"},
    {0x14A, "stu"},
    {0x14B, "stu"},
    {0x14C, "stu"},
    {0x14E, "stu"},
    {0x14F, "stu"},
    {0x151, "stu"},
    {0x153, "stu"},
    {0x154, "stu"},
    {0x157, "stu"},
    {0x158, "stu"},
    {0x159, "stu"},
    {0x15B, "stu"},
    {0x15D, "stu"},
    {0x161, "modelvertexdata"},
    {0x162, "stu"},
    {0x163, "stu"},
    {0x165, "stu"},
    {0x166, "stu"},
    {0x167, "stu"},
    {0x169, "stu"},
    {0x16A, "stu"},
    {0x16B, "stu"},
    {0x170, "binary_package_data"},
    {0x173, "stu"},
    {0x175, "stu"},
    {0x176, "stu"},
    {0x177, "stu"},
    {0x17A, "stu"},
    {0x17B, "stu"},
    {0x17C, "stu"},
    {0x17D, "stu"},
    {0x17F, "stu"},
    {0x181, "pso_group_collection"},
    {0x182, "stu"},
    {0x183, "stu"},
    {0x184, "stu"},
    {0x185, "stu"},
    {0x186, "stu"},
    {0x187, "stu"},
    {0x188, "stu"},
    {0x189, "stu"},
    {0x18A, "stu"},
    {0x18C, "stu"},
    {0x18D, "stu"},
    {0x18E, "stu"},
    {0x18F, "stu"},
    {0x190, "stu"},
    {0x191, "stu"},
}};

/// Name for @p typeId, or empty when this build does not register it.
constexpr std::string_view assetTypeName(u32 typeId) {
    const auto it = std::lower_bound(
        kAssetTypeNames.begin(), kAssetTypeNames.end(), typeId,
        [](const std::pair<u32, std::string_view>& e, u32 t) { return e.first < t; });
    if (it == kAssetTypeNames.end() || it->first != typeId)
        return {};
    return it->second;
}

} // namespace whiteout::storages::casc::ow
