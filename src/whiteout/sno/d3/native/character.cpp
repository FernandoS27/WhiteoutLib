// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/sno/d3/native/character.h>

#include <array>
#include <cctype>
#include <string>

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

// g_LookCategoryNames (retail 0x14776F0), by EEquipmentSlot ordinal.
const char* lookCategoryNameOrNull(u32 c) {
    switch (c) {
    case 2: return "TRS";
    case 3: return "RH1";
    case 4: return "LH1";
    case 5: return "GLV";
    case 7: return "BTS";
    case 9: return "LEG";
    default: return nullptr;
    }
}

// g_LookValueNames (retail 0x14777E0), indexed by look value.
constexpr std::array<const char*, 19> kLookValueNames = {
    "NKD",       "LIT_A",     "LIT_B",     "MED_A",     "MED_B",
    "HVY_A",     "HVY_B",     "LIT_C",     "MED_C",     "HVY_C",
    "CLS_LIT_A", "CLS_LIT_B", "CLS_LIT_C", "CLS_MED_A", "CLS_MED_B",
    "CLS_MED_C", "CLS_HVY_A", "CLS_HVY_B", "CLS_HVY_C",
};

// g_HairStyleNames (retail 0x144CC50).
constexpr std::array<const char*, 4> kHairStyleNames = {"NKD", "HLM1", "HLM2", "BALD"};

// The five sheath hardpoints Hardpoint_IsSheath (retail 0xAB9840) tests.
constexpr std::array<std::string_view, 5> kSheathHardpoints = {
    "HP_sheath_left_Back", "HP_sheath_right_Back", "HP_sheath_left_Hip",
    "HP_sheath_right_Hip", "HP_sheath_Shield",
};

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

std::optional<std::pair<PlayerClass, Gender>> playerFromAppearanceStem(std::string_view stem) {
    for (i32 c = 0; c < static_cast<i32>(kPlayerClassCount); ++c) {
        for (const Gender g : {Gender::Male, Gender::Female}) {
            const std::string want = playerAppearanceStem(static_cast<PlayerClass>(c), g);
            if (stem.size() < want.size()) continue;
            if (iEquals(stem.substr(0, want.size()), want))
                return std::make_pair(static_cast<PlayerClass>(c), g);
        }
    }
    return std::nullopt;
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

std::optional<u32> lookCategoryForVisualSlot(EVisualSlot slot) {
    for (const auto& p : kEquipSlotLooks)
        if (p.visualSlot == static_cast<u32>(slot)) return p.lookCategory;
    return std::nullopt;
}

const char* lookCategoryName(u32 lookCategory) {
    return lookCategoryNameOrNull(lookCategory);
}

const char* lookValueName(u32 lookValue) {
    // LookValue_GetName (retail 0xC04050) answers "NKD" for anything it does
    // not know, which is what makes a bad value render naked, not invisible.
    return lookValue < kLookValueNames.size() ? kLookValueNames[lookValue] : "NKD";
}

std::optional<u32> lookValueForGeoset(ArmourWeight weight, char variant, bool clsFamily) {
    if (weight == ArmourWeight::Unknown) return std::nullopt;
    const u32 v = (variant == 0 || variant == 'A') ? 0 : variant == 'B' ? 1 : variant == 'C' ? 2 : 3;
    if (v == 3) return std::nullopt;
    if (clsFamily) {
        // 10..18 = CLS_{LIT,MED,HVY}_{A,B,C}; there is no CLS_NKD.
        if (weight == ArmourWeight::Naked) return std::nullopt;
        const u32 w = weight == ArmourWeight::Light ? 0 : weight == ArmourWeight::Medium ? 1 : 2;
        return 10 + 3 * w + v;
    }
    if (weight == ArmourWeight::Naked) return v == 0 ? std::optional<u32>(0) : std::nullopt;
    const u32 base = armourWeightBaseLookValue(weight);   // LIT 1 / MED 3 / HVY 5
    if (v == 0) return base;                              // A
    if (v == 1) return base + 1;                          // B: 2 / 4 / 6
    return weight == ArmourWeight::Light ? 7u : weight == ArmourWeight::Medium ? 8u : 9u; // C
}

bool matchesLookCategory(std::string_view subObjectName, u32 lookCategory) {
    const char* cat = lookCategoryNameOrNull(lookCategory);
    if (!cat) return false;   // no name, no pattern -- ApplyLook returns early
    return subObjectName.find(cat) != std::string_view::npos;
}

bool matchesLook(std::string_view subObjectName, u32 lookCategory, u32 lookValue) {
    const char* cat = lookCategoryNameOrNull(lookCategory);
    if (!cat) return false;
    std::string pattern;
    pattern.reserve(16);
    pattern.append(cat).append("_").append(lookValueName(lookValue));
    return subObjectName.find(pattern) != std::string_view::npos;
}

size_t findLookIndexByHash(const Appearances& app, u32 lookNameHash) {
    for (size_t i = 0; i < app.arLooks.size(); ++i)
        if (lookNameHash33(app.arLooks[i].szName) == lookNameHash) return i;
    return 0; // Appearance_FindLookIndexById (retail 0x575730): first look on a miss
}

const char* hairStyleName(HairStyle style) {
    const auto i = static_cast<size_t>(style);
    return i < kHairStyleNames.size() ? kHairStyleNames[i] : "NKD";
}

u32 perClassArtTag(PlayerClass cls, Gender gender, bool secondary) {
    // The switch in ActorModel_GetItemAttachModelSno (retail 0x750BD0),
    // verbatim -- note DemonHunter is NOT at the base and Barbarian is the
    // default case.
    u32 off;
    switch (cls) {
    case PlayerClass::DemonHunter: off = 0x8; break;
    case PlayerClass::Wizard:      off = 0x2; break;
    case PlayerClass::WitchDoctor: off = 0x4; break;
    case PlayerClass::Monk:        off = 0x6; break;
    case PlayerClass::Crusader:    off = 0xA; break;
    case PlayerClass::Necromancer: off = 0xC; break;
    case PlayerClass::Barbarian:
    default:                       off = 0x0; break;
    }
    return (secondary ? 0x17200u : 0x17000u) + off + (gender == Gender::Female ? 1u : 0u);
}

ItemTypeTraits itemTypeTraits(u32 gbidItemType) {
    struct Row {
        const char* type;
        ItemTypeTraits t;
    };
    // Every name here cracks against a shipped gbidItemType value
    // (Corpus/D3/GameBalance). Flag provenance is in the header comment; the
    // hip/hidden sheath rows are in-game behaviour, marked as inference.
    static const Row kRows[] = {
        {"Shield", {.shield = true, .offhandOnly = true}},
        {"CrusaderShield", {.shield = true, .offhandOnly = true}},
        {"Mojo", {.offhandOnly = true}},
        {"Orb", {.offhandOnly = true}},
        {"Quiver", {.offhandOnly = true}},
        {"Helm", {.helm = true}},
        {"GenericHelm", {.helm = true}},
        {"VoodooMask", {.helm = true}},
        {"WizardHat", {.helm = true}},
        {"SpiritStone", {.helm = true}},
        {"Shoulders", {.shoulder = true}},
        {"Dagger", {.sheathAtHip = true}},          // inference: hip holster
        {"HandXBow", {.sheathAtHip = true}},        // inference: hip holster
        {"FistWeapon", {.sheathHidden = true}},     // inference: vanishes sheathed
    };
    static const auto kByGbid = [] {
        // The class-suffixed armour spellings (ItemTypeNames.stl ships 44:
        // {Belt,Boots,ChestArmor,Gloves,Helm,Legs,Shoulders,SpiritStone} x
        // class) hash as their own types, so the two families that ATTACH
        // need their traits under every spelling -- a Helm_Monk item whose
        // traits read empty resolves no hardpoint and silently never draws.
        // The other six families are pure look-armour and correctly
        // trait-less.
        std::array<std::pair<u32, ItemTypeTraits>, std::size(kRows) + 2 * kPlayerClassCount + 1>
            out{};
        size_t n = 0;
        for (size_t i = 0; i < std::size(kRows); ++i)
            out[n++] = {gbidHash(kRows[i].type), kRows[i].t};
        for (size_t c = 0; c < kPlayerClassCount; ++c) {
            out[n++] = {gbidHash(std::string("Helm_") + kClassNames[c]),
                        ItemTypeTraits{.helm = true}};
            out[n++] = {gbidHash(std::string("Shoulders_") + kClassNames[c]),
                        ItemTypeTraits{.shoulder = true}};
        }
        out[n++] = {gbidHash("SpiritStone_Monk"), ItemTypeTraits{.helm = true}};
        return out;
    }();
    for (const auto& [gbid, traits] : kByGbid)
        if (gbid == gbidItemType) return traits;
    return {};
}

std::optional<PlayerClass> playerClassFromTypeName(std::string_view typeName) {
    // The game's own encoding first: the `<Slot>_<Class>` armour spellings.
    if (const auto us = typeName.rfind('_'); us != std::string_view::npos) {
        const auto suffix = typeName.substr(us + 1);
        for (size_t i = 0; i < kPlayerClassCount; ++i)
            if (iEquals(suffix, kClassNames[i])) return static_cast<PlayerClass>(i);
    }
    // The class-locked base families -- the equip rules the server-side
    // ItemTypes flags used to carry. Only types the game locks outright;
    // shared families (Bow, Staff, Shield, Belt) stay out and read as neutral.
    struct Row {
        const char* type;
        PlayerClass cls;
    };
    static constexpr Row kRows[] = {
        {"MightyWeapon1H", PlayerClass::Barbarian},
        {"MightyWeapon2H", PlayerClass::Barbarian},
        {"VoodooMask", PlayerClass::WitchDoctor},
        {"Mojo", PlayerClass::WitchDoctor},
        {"CeremonialDagger", PlayerClass::WitchDoctor},
        {"WizardHat", PlayerClass::Wizard},
        {"Orb", PlayerClass::Wizard},
        {"Wand", PlayerClass::Wizard},
        {"FistWeapon", PlayerClass::Monk},
        {"CombatStaff", PlayerClass::Monk},
        {"HandXBow", PlayerClass::DemonHunter},
        {"Quiver", PlayerClass::DemonHunter},
        {"Cloak", PlayerClass::DemonHunter},
        {"Flail", PlayerClass::Crusader},
        {"Flail1H", PlayerClass::Crusader},
        {"Flail2H", PlayerClass::Crusader},
        {"CrusaderShield", PlayerClass::Crusader},
        {"Scythe", PlayerClass::Necromancer},
        {"Scythe1H", PlayerClass::Necromancer},
        {"Scythe2H", PlayerClass::Necromancer},
        {"NecromancerOffhand", PlayerClass::Necromancer},
    };
    for (const auto& row : kRows)
        if (iEquals(typeName, row.type)) return row.cls;
    return std::nullopt;
}

std::optional<PlayerClass> playerClassForSetKey(std::string_view setKey) {
    struct Row {
        const char* key;
        PlayerClass cls;
    };
    // The six all-generic-typed class-locked sets, by ItemSets.stl key; the
    // display names beside them are what a player knows them as.
    static constexpr Row kRows[] = {
        {"Ninja_Set_x1", PlayerClass::DemonHunter},      // The Shadow's Mantle
        {"Monkey_King_Set_x1", PlayerClass::Monk},       // Monkey King's Garb
        {"Dot_Set_x1", PlayerClass::WitchDoctor},        // Raiment of the Jade Harvester
        {"Earthquake_Set_x1", PlayerClass::Barbarian},   // Might of the Earth
        {"Arcane_Wraps_Set_x1", PlayerClass::Wizard},    // Vyr's Amazing Arcana
        {"Thorns_Set_x1", PlayerClass::Crusader},        // Thorns of the Invoker
    };
    for (const auto& row : kRows)
        if (iEquals(setKey, row.key)) return row.cls;
    return std::nullopt;
}

std::optional<std::string_view> attachHardpointName(u32 holdType, EVisualSlot slot,
                                                    const ItemTypeTraits& traits,
                                                    bool sheathed) {
    if (traits.neverDraw) return std::nullopt;

    const auto s = static_cast<i32>(slot);
    if (sheathed && !traits.ignoreSheathe) {
        if (traits.shield) return std::string_view("HP_sheath_Shield");
        if (traits.sheathHidden) return std::string_view("");
        if (s == 4)
            return std::string_view(traits.sheathAtHip ? "HP_sheath_right_Hip"
                                                       : "HP_sheath_right_Back");
        if (s == 5)
            return std::string_view(traits.sheathAtHip ? "HP_sheath_left_Hip"
                                                       : "HP_sheath_left_Back");
        // Helm and shoulders fall through: sheathing only moves weapons.
    }

    // The drawn switch on the hold type (tag 0x10081), then the flag
    // fall-through, exactly as 0xAB9060 orders them.
    switch (holdType) {
    case 0x0: case 0x1: case 0x2: case 0x5: case 0x8: case 0x11:
        if (s == 4) return std::string_view("HP_rightWeapon");
        if (s == 5) return std::string_view("HP_leftWeapon");
        break;
    case 0x3: case 0x4: case 0x7: case 0x10: case 0x19:
        return std::string_view("HP_rightWeapon");
    case 0x6:
        return std::string_view("HP_leftWeapon");
    case 0xF:
        return std::string_view(s == 5 ? "HP_leftFist" : "HP_rightFist");
    default:
        break;
    }
    if (traits.shield) return std::string_view("HP_shield");
    if (traits.offhandOnly) return std::string_view("HP_leftWeapon");
    if (traits.helm && s == 0) return std::string_view("HP_helm");
    if (traits.shoulder) return std::string_view("HP_left_shoulderPad");
    if (s == 4) return std::string_view("HP_rightWeapon");
    if (s == 5) return std::string_view("HP_shield");
    return std::string_view("");
}

bool isSheathHardpoint(std::string_view hardpointName) {
    for (const auto hp : kSheathHardpoints)
        if (iEquals(hardpointName, hp)) return true;
    return false;
}

EquipVisual resolveEquip(const Actor& itemActor, const ItemTypeTraits& traits,
                         EVisualSlot slot, PlayerClass cls, Gender gender, bool sheathed) {
    EquipVisual out;
    const auto s = static_cast<i32>(slot);

    if (lookCategoryForVisualSlot(slot).has_value()) {
        // Armour: the two look tags, exactly ActorModel_ApplyItemLookForSlot
        // (retail 0x750DE0). Absent tags stay absent -- the caller supplies
        // the default look (value 0, look "A"), not this function.
        const ItemLook look = itemLook(itemActor);
        out.lookValue = look.lookValue;
        out.lookNameHash = look.lookName;
        return out;
    }
    if (s != 0 && s != 4 && s != 5 && s != 6) return out; // cosmetics: out of scope

    const u32 hold = tagMapValue(itemActor.arTagMap, kTagItemHoldType).value_or(0);
    const auto hp = attachHardpointName(hold, slot, traits, sheathed);
    if (!hp) return out;   // flag-47 types: never drawn
    out.hardpoint = *hp;
    out.drawn = !out.hardpoint.empty();

    // Model choice, ActorModel_GetItemAttachModelSno (retail 0x750BD0): the
    // item's own Actor unless tag 0x17020 switches on the per-class pairs.
    out.attachActorSno = itemActor.dwSnoId;
    const bool perClass = tagMapValue(itemActor.arTagMap, kTagUsePerClassArt).value_or(0) != 0;
    if (perClass) {
        const auto primary = tagMapValue(itemActor.arTagMap, perClassArtTag(cls, gender, false));
        out.attachActorSno = primary ? static_cast<i32>(*primary) : -1;
        if (s == 5) {
            // The left hand prefers the SECONDARY art when it exists.
            const auto sec = tagMapValue(itemActor.arTagMap, perClassArtTag(cls, gender, true));
            if (sec && static_cast<i32>(*sec) != -1) out.attachActorSno = static_cast<i32>(*sec);
        }
    }
    if (s == 6) {
        // Shoulders attach twice; the twin at HP_right_shoulderPad uses the
        // secondary art for per-class items and the same model otherwise.
        out.secondAttach = true;
        out.secondAttachActorSno = itemActor.dwSnoId;
        if (perClass) {
            const auto sec = tagMapValue(itemActor.arTagMap, perClassArtTag(cls, gender, true));
            out.secondAttachActorSno = sec ? static_cast<i32>(*sec) : -1;
        }
    }
    if (s == 0)
        out.hairStyle = static_cast<HairStyle>(
            tagMapValue(itemActor.arTagMap, kTagItemHairStyle).value_or(0));
    return out;
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
