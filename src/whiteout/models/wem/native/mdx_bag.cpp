// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/native/mdx_bag.h>

#include <whiteout/models/mdx/structures.h>

#include <algorithm>
#include <bit>
#include <string>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// A geoset's static colour and alpha
// ============================================================================

GeosetTint GeosetTintOf(const MeshSection& section) {
    const auto value = [&](const char* key) -> const NativeBag::Entry* {
        return section.native.find(key);
    };
    const auto real = [](const NativeBag::Entry* entry) {
        return std::bit_cast<f32>(static_cast<u32>(entry->value));
    };
    GeosetTint tint;
    const NativeBag::Entry* r = value(kGeosetColorR);
    const NativeBag::Entry* g = value(kGeosetColorG);
    const NativeBag::Entry* b = value(kGeosetColorB);
    if (r != nullptr && g != nullptr && b != nullptr) {
        tint.color = Vector3f{real(r), real(g), real(b)};
    }
    if (const NativeBag::Entry* a = value(kGeosetAlpha)) {
        tint.alpha = real(a);
    }
    tint.hidden = hasFlag(section.flags, SectionFlags::Hidden);
    return tint;
}

void SetGeosetTint(MeshSection& section, const GeosetTint& tint) {
    const bool hidden = tint.hidden || tint.alpha <= 0.0f;
    const f32 alpha = tint.alpha > 0.0f ? tint.alpha : GeosetTintOf(section).alpha;
    const auto bits = [](f32 v) { return static_cast<i64>(std::bit_cast<u32>(v)); };

    // Written back where the import puts them -- after `selectionFlags` -- so
    // that a bag edited back to its imported value compares equal to it: the
    // bag keeps insertion order and two orders are two bags.
    std::vector<NativeBag::Entry>& entries = section.native.entries;
    std::size_t at = entries.size();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string& name = entries[i].name;
        if (name == kGeosetColorR || name == kGeosetColorG || name == kGeosetColorB ||
            name == kGeosetAlpha) {
            at = std::min(at, i);
        }
    }
    std::erase_if(entries, [](const NativeBag::Entry& entry) {
        return entry.name == kGeosetColorR || entry.name == kGeosetColorG ||
               entry.name == kGeosetColorB || entry.name == kGeosetAlpha;
    });
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == "selectionFlags") {
            at = i + 1;
        }
    }
    at = std::min(at, entries.size());

    std::vector<NativeBag::Entry> added;
    const Vector3f& c = tint.color;
    if (c.x != 1.0f || c.y != 1.0f || c.z != 1.0f) {
        added.push_back({kGeosetColorR, bits(c.x), {}});
        added.push_back({kGeosetColorG, bits(c.y), {}});
        added.push_back({kGeosetColorB, bits(c.z), {}});
    }
    if (alpha != 1.0f) {
        added.push_back({kGeosetAlpha, bits(alpha), {}});
    }
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(at), added.begin(), added.end());

    const u32 without = static_cast<u32>(section.flags) & ~static_cast<u32>(SectionFlags::Hidden);
    section.flags = static_cast<SectionFlags>(without);
    if (hidden) {
        section.flags |= SectionFlags::Hidden;
    }
}

GeosetFlags GeosetFlagsOf(const MeshSection& section) {
    GeosetFlags flags;
    if (const NativeBag::Entry* selection = section.native.find("selectionFlags")) {
        flags.selection = static_cast<u32>(selection->value);
    }
    if (const NativeBag::Entry* word = section.native.find(kGeosetAnimFlags)) {
        flags.animation = static_cast<u32>(word->value) & ~GeosetFlags::kDropShadow;
    }
    if (hasFlag(section.flags, SectionFlags::ProjectedShadow)) {
        flags.animation |= GeosetFlags::kDropShadow;
    }
    return flags;
}

void SetGeosetFlags(MeshSection& section, const GeosetFlags& flags) {
    section.native.set("selectionFlags", static_cast<i64>(flags.selection));
    const u32 rest = flags.animation & ~GeosetFlags::kDropShadow;
    std::erase_if(section.native.entries,
                  [](const NativeBag::Entry& entry) { return entry.name == kGeosetAnimFlags; });
    if (rest != static_cast<u32>(mdx::GeosetAnimation::Flag::Color)) {
        section.native.set(kGeosetAnimFlags, static_cast<i64>(rest));
    }
    const u32 without =
        static_cast<u32>(section.flags) & ~static_cast<u32>(SectionFlags::ProjectedShadow);
    section.flags = static_cast<SectionFlags>(without);
    if ((flags.animation & GeosetFlags::kDropShadow) != 0) {
        section.flags |= SectionFlags::ProjectedShadow;
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout
