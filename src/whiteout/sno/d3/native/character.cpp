// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/sno/d3/native/character.h>

#include <array>
#include <cctype>

namespace whiteout {
namespace sno {
namespace d3 {
namespace native {
namespace {

constexpr std::array<const char*, kPlayerClassCount> kClassNames = {
    "DemonHunter", "Barbarian", "Wizard", "WitchDoctor", "Monk", "Crusader", "Necromancer",
};

// Same ordinals, different spellings -- see the header.
constexpr std::array<const char*, kPlayerClassCount> kPortraitNames = {
    "Demonhunter", "Barbarian", "Wizard", "Witchdoctor", "Monk", "Crusader", "Necromancer",
};

// The stems the shipped Appearances actually use. Crusader and Necromancer are
// the two that do not follow from the class name.
constexpr std::array<const char*, kPlayerClassCount> kAppearanceStems = {
    "DemonHunter", "Barbarian", "Wizard", "WitchDoctor", "Monk", "X1_Crusader", "P6_Necro",
};

constexpr std::array<EquipSlotLook, 4> kEquipSlotLooks = {{
    {1, 2},
    {2, 7},
    {3, 5},
    {7, 9},
}};

bool inRange(PlayerClass cls) {
    const auto i = static_cast<i32>(cls);
    return i >= 0 && static_cast<size_t>(i) < kPlayerClassCount;
}

bool iEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

LookSlot slotFromToken(std::string_view s) {
    if (s == "TRS") return LookSlot::Torso;
    if (s == "LEG") return LookSlot::Legs;
    if (s == "BTS") return LookSlot::Boots;
    if (s == "GLV") return LookSlot::Gloves;
    if (iEquals(s, "Hair")) return LookSlot::Hair;
    return LookSlot::Unknown;
}

ArmourWeight weightFromToken(std::string_view s) {
    if (s == "NKD") return ArmourWeight::Naked;
    if (s == "LIT") return ArmourWeight::Light;
    if (s == "MED") return ArmourWeight::Medium;
    if (s == "HVY") return ArmourWeight::Heavy;
    return ArmourWeight::Unknown;
}

// Split a token on '_' without allocating.
std::vector<std::string_view> splitToken(std::string_view t) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= t.size()) {
        const size_t at = t.find('_', start);
        if (at == std::string_view::npos) {
            parts.push_back(t.substr(start));
            break;
        }
        parts.push_back(t.substr(start, at - start));
        start = at + 1;
    }
    return parts;
}

} // namespace

const char* playerClassName(PlayerClass cls) {
    return inRange(cls) ? kClassNames[static_cast<size_t>(cls)] : "Unknown";
}

const char* playerPortraitClassName(PlayerClass cls) {
    return inRange(cls) ? kPortraitNames[static_cast<size_t>(cls)] : "Unknown";
}

std::string playerAppearanceStem(PlayerClass cls, Gender gender) {
    if (!inRange(cls)) return {};
    return std::string(kAppearanceStems[static_cast<size_t>(cls)]) +
           (gender == Gender::Male ? "_Male" : "_Female");
}

std::span<const EquipSlotLook> equipSlotLookCategories() {
    return {kEquipSlotLooks.data(), kEquipSlotLooks.size()};
}

u32 fallbackLookValue(u32 lookValue) {
    // Transcribed from the bitmask tests in ActorModel_ApplyLook: the binary
    // guards on `lookValue <= 9` and then tests (1 << lookValue) against
    // 0x84 (bits 2,7), 0x110 (bits 4,8) and 0x240 (bits 6,9).
    if (lookValue > 9) return 0;
    const u32 bit = 1u << lookValue;
    if (bit & 0x84u) return 1;
    if (bit & 0x110u) return 3;
    if (bit & 0x240u) return 5;
    return 0;
}

std::optional<u32> tagMapValue(std::span<const TagMapEntry> tagMap, u32 tagId) {
    for (const auto& e : tagMap)
        if (e.dwTagId == tagId) return e.dwValue;
    return std::nullopt;
}

ItemLook itemLook(const Actor& itemActor) {
    ItemLook out;
    out.lookValue = tagMapValue(itemActor.arTagMap, kTagItemLookValue);
    out.lookName = tagMapValue(itemActor.arTagMap, kTagItemLookName);
    return out;
}

size_t findLookIndex(const Appearances& app, std::string_view lookName) {
    for (size_t i = 0; i < app.arLooks.size(); ++i)
        if (app.arLooks[i].szName == lookName) return i;
    return 0; // what Appearance_FindLookIndex does when it finds nothing
}

const SubObjectAppearance* subObjectAppearance(const Appearances& app, const SubObject& sub,
                                               size_t lookIndex) {
    for (const auto& m : app.arMaterials) {
        if (m.szName != sub.szName) continue;
        if (lookIndex >= m.arVariants.size()) return nullptr;
        return &m.arVariants[lookIndex];
    }
    return nullptr;
}

GeosetName parseGeosetName(const SubObject& sub) {
    GeosetName out;
    const std::string& mn = sub.szMaterialName;

    // "<token>Shape_<szName>_<NNN>", or "<token>ShapeDeformed_<szName>_<NNN>"
    // in the Necromancer files.
    size_t at = mn.find("ShapeDeformed_");
    size_t skip = 14;
    if (at == std::string::npos) {
        at = mn.find("Shape_");
        skip = 6;
    }
    if (at == std::string::npos) return out;

    const std::string_view rest(mn.data() + at + skip, mn.size() - at - skip);
    // The remainder must be the sub-object's own name plus a numeric suffix;
    // requiring that is what keeps an unrelated "...Shape_" from matching.
    if (rest.size() <= sub.szName.size()) return out;
    if (rest.compare(0, sub.szName.size(), sub.szName) != 0) return out;
    if (rest[sub.szName.size()] != '_') return out;

    out.parsed = true;
    out.token = std::string_view(mn.data(), at);

    auto parts = splitToken(out.token);
    if (parts.empty()) return out;

    // A trailing "_Cloth" (either spelling) is a sub-piece marker, not part of
    // the slot/weight/variant triple, so take it off first.
    if (parts.size() > 1 && iEquals(parts.back(), "Cloth")) {
        out.cloth = true;
        parts.pop_back();
    }

    size_t i = 0;
    if (i < parts.size() && parts[i] == "N") ++i; // the armour prefix

    // Hair is its own shape: "Hair_HLM1", "Hair_NKD", "N_Hair". Consume the
    // "Hair" part along with the slot, or the helm/weight token after it gets
    // read as a qualifier ("Hair_NKD" -> qualifier "Hair").
    if (i < parts.size() && iEquals(parts[i], "Hair")) {
        out.slot = LookSlot::Hair;
        ++i;
    }

    if (out.slot == LookSlot::Unknown && i < parts.size()) {
        const LookSlot s = slotFromToken(parts[i]);
        if (s != LookSlot::Unknown) {
            out.slot = s;
            ++i;
        }
    }
    // An optional qualifier sits between slot and weight ("N_TRS_CLS_MED_B").
    if (i < parts.size() && weightFromToken(parts[i]) == ArmourWeight::Unknown &&
        i + 1 < parts.size() && weightFromToken(parts[i + 1]) != ArmourWeight::Unknown) {
        out.qualifier = parts[i];
        ++i;
    }
    if (i < parts.size()) {
        const ArmourWeight w = weightFromToken(parts[i]);
        if (w != ArmourWeight::Unknown) {
            out.weight = w;
            ++i;
        }
    }
    // A single-letter tail is the variant.
    if (i < parts.size() && parts[i].size() == 1) {
        const char c = parts[i][0];
        if (c >= 'A' && c <= 'Z') out.variant = c;
    }
    return out;
}

u32 armourWeightBaseLookValue(ArmourWeight weight) {
    switch (weight) {
    case ArmourWeight::Naked: return 0;
    case ArmourWeight::Light: return 1;
    case ArmourWeight::Medium: return 3;
    case ArmourWeight::Heavy: return 5;
    default: return 0;
    }
}

bool usesDyeType(i32 dyeType) {
    return dyeType != kDyeNone;
}

f32 dyeRampU(i32 dyeType) {
    if (dyeType < kDyeFirst) return 0.0f;
    return (static_cast<f32>(dyeType - kDyeFirst) + 0.5f) / static_cast<f32>(kDyeRampRows);
}

std::optional<size_t> tintSlotForKind(u32 tintKind) {
    if (tintKind < kTintKindFirst || tintKind > kTintKindLast) return std::nullopt;
    return static_cast<size_t>(tintKind - kTintKindFirst);
}

} // namespace native
} // namespace d3
} // namespace sno
} // namespace whiteout
