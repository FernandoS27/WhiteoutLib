// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Gate on the rules for setting up a Diablo III player character.
///
/// `character.h` holds two different kinds of claim and they need different
/// checks, so the cases below are split the same way:
///
///   * Rules read out of the Switch 2.6.2 binary -- the look-value fallback, the
///     dye ramp, the equip-slot table, the class ordinals. Nothing in the corpus
///     can confirm these, so the tests pin the exact values against a regression
///     rather than pretending to derive them.
///   * Relations recovered from the shipped `.app` files -- the geoset shape
///     names, sub-object to material, material variants per look. These run over
///     all 14 player Appearances and would catch a parser change that broke them.

#include <catch2/catch_all.hpp>

#include <whiteout/sno/d3/native/character.h>
#include <whiteout/sno/d3/native/d3_native.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace nat = whiteout::sno::d3::native;
using whiteout::u32;
using whiteout::u8;

namespace {

std::vector<u8> readWhole(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = static_cast<size_t>(f.tellg());
    std::vector<u8> b(sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(sz));
    return b;
}

fs::path appearanceDir() {
    for (auto c : {"Corpus/D3/Appearances", "../Corpus/D3/Appearances", "../../Corpus/D3/Appearances"})
        if (fs::is_directory(c)) return c;
    return {};
}

/// Every shipped player Appearance, keyed by its stem.
std::vector<std::pair<std::string, nat::Appearances>> loadPlayerAppearances() {
    std::vector<std::pair<std::string, nat::Appearances>> out;
    const fs::path dir = appearanceDir();
    if (dir.empty()) return out;
    for (whiteout::i32 c = 0; c < static_cast<whiteout::i32>(nat::kPlayerClassCount); ++c)
        for (auto g : {nat::Gender::Male, nat::Gender::Female}) {
            const auto cls = static_cast<nat::PlayerClass>(c);
            const std::string stem = nat::playerAppearanceStem(cls, g);
            const auto data = readWhole(dir / (stem + ".app"));
            if (data.empty()) continue;
            auto a = nat::parseAppearances(data);
            if (a) out.emplace_back(stem, std::move(*a));
        }
    return out;
}

} // namespace

// ===========================================================================
// Rules from the binary
// ===========================================================================

TEST_CASE("d3_character_class_table", "[d3][character]") {
    using PC = nat::PlayerClass;

    // The ordinals two independent string builders agree on.
    CHECK(static_cast<int>(PC::DemonHunter) == 0);
    CHECK(static_cast<int>(PC::Barbarian) == 1);
    CHECK(static_cast<int>(PC::Wizard) == 2);
    CHECK(static_cast<int>(PC::WitchDoctor) == 3);
    CHECK(static_cast<int>(PC::Monk) == 4);
    CHECK(static_cast<int>(PC::Crusader) == 5);
    CHECK(static_cast<int>(PC::Necromancer) == 6);

    // The two spellings really do differ; if they are ever unified by mistake,
    // one of the two lookups they feed starts silently missing.
    CHECK(std::string(nat::playerClassName(PC::DemonHunter)) == "DemonHunter");
    CHECK(std::string(nat::playerPortraitClassName(PC::DemonHunter)) == "Demonhunter");
    CHECK(std::string(nat::playerClassName(PC::WitchDoctor)) == "WitchDoctor");
    CHECK(std::string(nat::playerPortraitClassName(PC::WitchDoctor)) == "Witchdoctor");

    // ...and the asset stem is a third spelling again for two of the seven.
    CHECK(nat::playerAppearanceStem(PC::Barbarian, nat::Gender::Male) == "Barbarian_Male");
    CHECK(nat::playerAppearanceStem(PC::Crusader, nat::Gender::Male) == "X1_Crusader_Male");
    CHECK(nat::playerAppearanceStem(PC::Necromancer, nat::Gender::Female) == "P6_Necro_Female");

    // Out of range is answered, not crashed.
    CHECK(std::string(nat::playerClassName(static_cast<PC>(99))) == "Unknown");
    CHECK(nat::playerAppearanceStem(static_cast<PC>(99), nat::Gender::Male).empty());
}

TEST_CASE("d3_character_look_value_fallback", "[d3][character]") {
    // Transcribed from ActorModel_ApplyLook's three bitmask tests. The third
    // rule (6, 9 -> 5) is the one an earlier reading of this function missed.
    CHECK(nat::fallbackLookValue(2) == 1);
    CHECK(nat::fallbackLookValue(7) == 1);
    CHECK(nat::fallbackLookValue(4) == 3);
    CHECK(nat::fallbackLookValue(8) == 3);
    CHECK(nat::fallbackLookValue(6) == 5);
    CHECK(nat::fallbackLookValue(9) == 5);

    // The base values of each group have no intermediate step of their own.
    for (u32 v : {0u, 1u, 3u, 5u}) {
        INFO("value " << v);
        CHECK(nat::fallbackLookValue(v) == 0);
    }
    // The binary guards on `<= 9`; everything past it goes straight to 0.
    for (u32 v : {10u, 11u, 255u, 0xFFFFFFFFu}) {
        INFO("value " << v);
        CHECK(nat::fallbackLookValue(v) == 0);
    }

    // The inferred weight -> base value mapping is exactly the set of values
    // that are their own fallback target, which is the evidence for it.
    CHECK(nat::armourWeightBaseLookValue(nat::ArmourWeight::Naked) == 0);
    CHECK(nat::armourWeightBaseLookValue(nat::ArmourWeight::Light) == 1);
    CHECK(nat::armourWeightBaseLookValue(nat::ArmourWeight::Medium) == 3);
    CHECK(nat::armourWeightBaseLookValue(nat::ArmourWeight::Heavy) == 5);
}

TEST_CASE("d3_character_equip_slot_table", "[d3][character]") {
    const auto pairs = nat::equipSlotLookCategories();
    REQUIRE(pairs.size() == 4); // exactly four geoset slots, the rest are attachments

    const std::map<u32, u32> expect = {{1, 2}, {2, 7}, {3, 5}, {7, 9}};
    std::map<u32, u32> got;
    for (const auto& p : pairs) got[p.visualSlot] = p.lookCategory;
    CHECK(got == expect);

    // Category 8 is the hair/helm path and is deliberately NOT one of the four.
    for (const auto& p : pairs) CHECK(p.lookCategory != nat::kLookCategoryHairHelm);
}

TEST_CASE("d3_character_dye_ramp", "[d3][character]") {
    // 21 rows, ids 2..22, sampled at row centres.
    CHECK(nat::kDyeLast - nat::kDyeFirst + 1 == nat::kDyeRampRows);

    CHECK_FALSE(nat::usesDyeType(0));
    CHECK(nat::usesDyeType(1)); // reserved, but still "dyed" as far as the shader is told
    CHECK(nat::usesDyeType(2));

    // Below the first real dye the ramp coordinate is 0, not a negative row.
    CHECK(nat::dyeRampU(0) == 0.0f);
    CHECK(nat::dyeRampU(1) == 0.0f);

    // Row centres: dye 2 is the middle of row 0, dye 22 the middle of row 20.
    CHECK(nat::dyeRampU(2) == Catch::Approx(0.5f / 21.0f));
    CHECK(nat::dyeRampU(22) == Catch::Approx(20.5f / 21.0f));

    // Every dye lands strictly inside its own row -- this is what makes it a
    // palette rather than a gradient, so it is worth asserting directly.
    for (whiteout::i32 d = nat::kDyeFirst; d <= nat::kDyeLast; ++d) {
        const float u = nat::dyeRampU(d);
        const float row = static_cast<float>(d - nat::kDyeFirst);
        INFO("dye " << d);
        CHECK(u > row / 21.0f);
        CHECK(u < (row + 1.0f) / 21.0f);
    }

    // The five tint slots the Appearance carries, selected by kind 9..13.
    CHECK_FALSE(nat::tintSlotForKind(8).has_value());
    CHECK(nat::tintSlotForKind(9) == std::optional<size_t>(0));
    CHECK(nat::tintSlotForKind(13) == std::optional<size_t>(4));
    CHECK_FALSE(nat::tintSlotForKind(14).has_value());
}

TEST_CASE("d3_character_tag_map_lookup", "[d3][character]") {
    std::vector<nat::TagMapEntry> tags = {
        {2, nat::kTagItemLookValue, 3},
        {2, nat::kTagItemLookName, 0x42},
        {2, 0x10402, 7},
    };
    CHECK(nat::tagMapValue(tags, nat::kTagItemLookValue) == std::optional<u32>(3));
    CHECK(nat::tagMapValue(tags, nat::kTagItemLookName) == std::optional<u32>(0x42));
    CHECK_FALSE(nat::tagMapValue(tags, 0x99999).has_value());

    // An item that says nothing is distinguishable from one that says 0.
    nat::Actor bare{};
    const auto look = nat::itemLook(bare);
    CHECK_FALSE(look.lookValue.has_value());
    CHECK_FALSE(look.lookName.has_value());
}

// ===========================================================================
// Relations in the shipped .app files
// ===========================================================================

TEST_CASE("d3_character_player_appearances_load", "[d3][character][corpus]") {
    if (appearanceDir().empty()) SKIP("D3 Appearances corpus not found");
    const auto apps = loadPlayerAppearances();
    // 7 classes x 2 genders, and every stem must resolve -- this is what pins
    // the X1_/P6_ prefixes rather than leaving them as a claim in a comment.
    REQUIRE(apps.size() == 14);
    for (const auto& [stem, a] : apps) {
        INFO(stem);
        CHECK(a.arLooks.size() > 0);
        CHECK(a.arMaterials.size() > 0);
        CHECK(a.tGeoSet0.arSubObjects.size() > 0);
    }
}

TEST_CASE("d3_character_look_selection", "[d3][character][corpus]") {
    if (appearanceDir().empty()) SKIP("D3 Appearances corpus not found");
    const auto apps = loadPlayerAppearances();
    REQUIRE(apps.size() == 14);

    size_t subs = 0, resolved = 0;
    for (const auto& [stem, a] : apps) {
        INFO(stem);
        // The default look the engine falls back to when nothing is equipped.
        const size_t defaultIdx = nat::findLookIndex(a, nat::kDefaultLookName);
        UNSCOPED_INFO("  " << stem << ": default look '" << nat::kDefaultLookName << "' -> index "
                           << defaultIdx << " of " << a.arLooks.size());

        // A name that is not there resolves to 0, matching the binary rather
        // than returning something the caller has to check.
        CHECK(nat::findLookIndex(a, "no-such-look-name") == 0);

        // Every sub-object must reach a material variant at every look index,
        // since that is what the look index is for.
        for (const auto* gs : {&a.tGeoSet0, &a.tGeoSet1})
            for (const auto& so : gs->arSubObjects) {
                ++subs;
                const auto* first = nat::subObjectAppearance(a, so, 0);
                const auto* last = nat::subObjectAppearance(a, so, a.arLooks.size() - 1);
                INFO("sub-object " << so.szName);
                CHECK(first != nullptr);
                CHECK(last != nullptr);
                if (first && last) ++resolved;
                // Past the end is refused rather than read out of range.
                CHECK(nat::subObjectAppearance(a, so, a.arLooks.size()) == nullptr);
            }
    }
    CHECK(subs == 441);
    CHECK(resolved == subs);
}

TEST_CASE("d3_character_geoset_names", "[d3][character][corpus]") {
    if (appearanceDir().empty()) SKIP("D3 Appearances corpus not found");
    const auto apps = loadPlayerAppearances();
    REQUIRE(apps.size() == 14);

    size_t subs = 0, parsed = 0, armour = 0;
    std::set<std::string> unparsed;
    std::map<int, size_t> slotHist;
    std::map<int, size_t> weightHist;
    std::set<char> variants;

    for (const auto& [stem, a] : apps)
        for (const auto* gs : {&a.tGeoSet0, &a.tGeoSet1})
            for (const auto& so : gs->arSubObjects) {
                ++subs;
                const auto g = nat::parseGeosetName(so);
                if (!g.parsed) {
                    unparsed.insert(so.szMaterialName);
                    continue;
                }
                ++parsed;
                slotHist[static_cast<int>(g.slot)] += 1;
                if (g.slot != nat::LookSlot::Unknown && g.slot != nat::LookSlot::Hair) {
                    ++armour;
                    weightHist[static_cast<int>(g.weight)] += 1;
                    // An armour geoset always names its weight class; without
                    // one there would be nothing for the look value to select.
                    INFO(stem << " / " << so.szMaterialName);
                    CHECK(g.weight != nat::ArmourWeight::Unknown);
                    if (g.variant) variants.insert(g.variant);
                }
            }

    UNSCOPED_INFO("parsed " << parsed << " of " << subs << " sub-object shape names");
    for (const auto& u : unparsed) UNSCOPED_INFO("  unparsed: " << u);

    CHECK(subs == 441);
    // 436 of 441. The five that do not parse are effect meshes (Vengeance,
    // two oneBatch) that share one material name: the literal "HC", with no
    // shape suffix at all. Pinning the string as well as the count says why
    // they are excluded, so a future miss shows up as a different name rather
    // than as a number that drifted.
    CHECK(parsed == 436);
    CHECK(subs - parsed == 5);
    REQUIRE(unparsed.size() == 1);
    CHECK(*unparsed.begin() == "HC");

    // All four armour slots and all four weight classes are represented.
    for (auto s : {nat::LookSlot::Torso, nat::LookSlot::Legs, nat::LookSlot::Boots,
                   nat::LookSlot::Gloves, nat::LookSlot::Hair}) {
        INFO("slot " << static_cast<int>(s));
        CHECK(slotHist[static_cast<int>(s)] > 0);
    }
    for (auto w : {nat::ArmourWeight::Naked, nat::ArmourWeight::Light, nat::ArmourWeight::Medium,
                   nat::ArmourWeight::Heavy}) {
        INFO("weight " << static_cast<int>(w));
        CHECK(weightHist[static_cast<int>(w)] > 0);
    }
    CHECK(armour > 0);

    // Variants observed in the shipped files are A, B and C -- the same width as
    // each fallback group (a base plus two spares), which is the corroboration
    // for armourWeightBaseLookValue().
    CHECK(variants == std::set<char>{'A', 'B', 'C'});
}

TEST_CASE("d3_character_geoset_name_grammar", "[d3][character]") {
    // Exercise the token shapes directly, including the two that the corpus
    // only has a handful of and the case-varying _Cloth suffix.
    struct Case {
        const char* name;
        const char* material;
        nat::LookSlot slot;
        nat::ArmourWeight weight;
        char variant;
        bool cloth;
        const char* qualifier;
    };
    const Case cases[] = {
        {"Barb_F_HVY_mat", "N_TRS_HVY_AShape_Barb_F_HVY_mat_001", nat::LookSlot::Torso,
         nat::ArmourWeight::Heavy, 'A', false, ""},
        {"Barb_F_NKD_mat", "N_GLV_NKDShape_Barb_F_NKD_mat_001", nat::LookSlot::Gloves,
         nat::ArmourWeight::Naked, 0, false, ""},
        {"Barb_F_HVY_Cloth", "N_LEG_HVY_A_clothShape_Barb_F_HVY_Cloth_001", nat::LookSlot::Legs,
         nat::ArmourWeight::Heavy, 'A', true, ""},
        {"Barb_F_NKD_Cloth", "N_LEG_NKD_ClothShape_Barb_F_NKD_Cloth_001", nat::LookSlot::Legs,
         nat::ArmourWeight::Naked, 0, true, ""},
        {"DHF_CLS_MED_B_mat", "N_TRS_CLS_MED_BShape_DHF_CLS_MED_B_mat_001", nat::LookSlot::Torso,
         nat::ArmourWeight::Medium, 'B', false, "CLS"},
        {"Barb_F_Hair_mat", "Hair_HLM1Shape_Barb_F_Hair_mat_001", nat::LookSlot::Hair,
         nat::ArmourWeight::Unknown, 0, false, ""},
        // "Hair" has to be consumed WITH the slot: leave it in place and the
        // weight token behind it is read as a qualifier instead.
        {"Barb_F_Hair_mat", "Hair_NKDShape_Barb_F_Hair_mat_001", nat::LookSlot::Hair,
         nat::ArmourWeight::Naked, 0, false, ""},
        {"A_F_Hair", "N_HairShape_A_F_Hair_001", nat::LookSlot::Hair,
         nat::ArmourWeight::Unknown, 0, false, ""},
        {"Necromancer_F_HVY_MAT", "N_BTS_HVY_AShapeDeformed_Necromancer_F_HVY_MAT_001",
         nat::LookSlot::Boots, nat::ArmourWeight::Heavy, 'A', false, ""},
    };

    for (const Case& c : cases) {
        nat::SubObject so;
        so.szName = c.name;
        so.szMaterialName = c.material;
        const auto g = nat::parseGeosetName(so);
        INFO(c.material);
        REQUIRE(g.parsed);
        CHECK(g.slot == c.slot);
        CHECK(g.weight == c.weight);
        CHECK(g.variant == c.variant);
        CHECK(g.cloth == c.cloth);
        CHECK(g.qualifier == std::string_view(c.qualifier));
    }

    // A material name that is not a shape name at all parses as "not parsed"
    // rather than as a token of nothing.
    nat::SubObject odd;
    odd.szName = "Vengeance_mat";
    odd.szMaterialName = "HC";
    CHECK_FALSE(nat::parseGeosetName(odd).parsed);

    // The suffix must belong to THIS sub-object: a shape name naming a
    // different sub-object is rejected, which is what stops a stray "Shape_"
    // in an unrelated string from being read as a geoset.
    nat::SubObject mismatched;
    mismatched.szName = "Barb_F_HVY_mat";
    mismatched.szMaterialName = "N_TRS_HVY_AShape_Something_Else_001";
    CHECK_FALSE(nat::parseGeosetName(mismatched).parsed);
}

TEST_CASE("d3_character_retail_tables", "[d3][character]") {
    // g_LookCategoryNames (retail 0x14776F0): six categories, NULL elsewhere.
    CHECK(nat::lookCategoryName(2) == std::string_view("TRS"));
    CHECK(nat::lookCategoryName(3) == std::string_view("RH1"));
    CHECK(nat::lookCategoryName(4) == std::string_view("LH1"));
    CHECK(nat::lookCategoryName(5) == std::string_view("GLV"));
    CHECK(nat::lookCategoryName(7) == std::string_view("BTS"));
    CHECK(nat::lookCategoryName(9) == std::string_view("LEG"));
    CHECK(nat::lookCategoryName(1) == nullptr);  // Head: attachment-only
    CHECK(nat::lookCategoryName(8) == nullptr);  // Shoulders: attachment-only

    // g_LookValueNames (retail 0x14777E0), all nineteen, NKD for unknown.
    CHECK(nat::lookValueName(0) == std::string_view("NKD"));
    CHECK(nat::lookValueName(1) == std::string_view("LIT_A"));
    CHECK(nat::lookValueName(2) == std::string_view("LIT_B"));
    CHECK(nat::lookValueName(7) == std::string_view("LIT_C"));
    CHECK(nat::lookValueName(3) == std::string_view("MED_A"));
    CHECK(nat::lookValueName(4) == std::string_view("MED_B"));
    CHECK(nat::lookValueName(8) == std::string_view("MED_C"));
    CHECK(nat::lookValueName(5) == std::string_view("HVY_A"));
    CHECK(nat::lookValueName(6) == std::string_view("HVY_B"));
    CHECK(nat::lookValueName(9) == std::string_view("HVY_C"));
    CHECK(nat::lookValueName(10) == std::string_view("CLS_LIT_A"));
    CHECK(nat::lookValueName(18) == std::string_view("CLS_HVY_C"));
    CHECK(nat::lookValueName(99) == std::string_view("NKD"));

    // The geoset-token inverse agrees with the name table.
    using AW = nat::ArmourWeight;
    CHECK(nat::lookValueForGeoset(AW::Naked, 0, false) == 0u);
    CHECK(nat::lookValueForGeoset(AW::Light, 'C', false) == 7u);
    CHECK(nat::lookValueForGeoset(AW::Medium, 'B', false) == 4u);
    CHECK(nat::lookValueForGeoset(AW::Heavy, 'C', false) == 9u);
    CHECK(nat::lookValueForGeoset(AW::Medium, 'A', true) == 13u);
    CHECK_FALSE(nat::lookValueForGeoset(AW::Naked, 'A', true).has_value());

    // The engine's substring match on a real corpus spelling.
    const std::string_view name = "N_TRS_HVY_AShape_Barb_F_HVY_mat_001";
    CHECK(nat::matchesLookCategory(name, 2));
    CHECK(nat::matchesLook(name, 2, 5));        // TRS_HVY_A
    CHECK_FALSE(nat::matchesLook(name, 2, 6));  // TRS_HVY_B
    CHECK_FALSE(nat::matchesLookCategory(name, 9));
    CHECK_FALSE(nat::matchesLookCategory(name, 8));  // unnamed category: never

    // Str_Hash33 is case-sensitive, unlike gbidHash.
    CHECK(nat::lookNameHash33("A") == u32('A'));
    CHECK(nat::lookNameHash33("A") != nat::lookNameHash33("a"));
    CHECK(nat::gbidHash("A") == nat::gbidHash("a"));
}

TEST_CASE("d3_character_attachment_rules", "[d3][character]") {
    // perClassArtTag (retail 0x750BD0): a switch, not base + 2*class, and +1
    // is FEMALE (measured: Helm_hell_base_01 0x17000 -> barbM, 0x17001 ->
    // barbF).
    using PC = nat::PlayerClass;
    using G = nat::Gender;
    CHECK(nat::perClassArtTag(PC::Barbarian, G::Male, false) == 0x17000u);
    CHECK(nat::perClassArtTag(PC::Barbarian, G::Female, false) == 0x17001u);
    CHECK(nat::perClassArtTag(PC::DemonHunter, G::Male, false) == 0x17008u);
    CHECK(nat::perClassArtTag(PC::Wizard, G::Male, false) == 0x17002u);
    CHECK(nat::perClassArtTag(PC::WitchDoctor, G::Male, false) == 0x17004u);
    CHECK(nat::perClassArtTag(PC::Monk, G::Male, false) == 0x17006u);
    CHECK(nat::perClassArtTag(PC::Crusader, G::Male, false) == 0x1700Au);
    CHECK(nat::perClassArtTag(PC::Necromancer, G::Female, false) == 0x1700Du);
    CHECK(nat::perClassArtTag(PC::Monk, G::Male, true) == 0x17206u);

    // Item_GetAttachHardpointName (retail 0xAB9060).
    using VS = nat::EVisualSlot;
    const nat::ItemTypeTraits none{};
    nat::ItemTypeTraits shield;
    shield.shield = shield.offhandOnly = true;
    nat::ItemTypeTraits helm;
    helm.helm = true;
    nat::ItemTypeTraits shoulder;
    shoulder.shoulder = true;
    nat::ItemTypeTraits hip;
    hip.sheathAtHip = true;
    nat::ItemTypeTraits never;
    never.neverDraw = true;

    auto hp = [](std::optional<std::string_view> v) { return v.value_or("<none>"); };
    CHECK(hp(nat::attachHardpointName(0, VS::RightHand, none, false)) == "HP_rightWeapon");
    CHECK(hp(nat::attachHardpointName(0, VS::LeftHand, none, false)) == "HP_leftWeapon");
    CHECK(hp(nat::attachHardpointName(3, VS::LeftHand, none, false)) == "HP_rightWeapon");
    CHECK(hp(nat::attachHardpointName(6, VS::RightHand, none, false)) == "HP_leftWeapon");
    CHECK(hp(nat::attachHardpointName(0xF, VS::RightHand, none, false)) == "HP_rightFist");
    CHECK(hp(nat::attachHardpointName(0xF, VS::LeftHand, none, false)) == "HP_leftFist");
    CHECK(hp(nat::attachHardpointName(0, VS::LeftHand, shield, false)) == "HP_leftWeapon");
    CHECK(hp(nat::attachHardpointName(0x12, VS::LeftHand, shield, false)) == "HP_shield");
    CHECK(hp(nat::attachHardpointName(0, VS::Head, helm, false)) == "HP_helm");
    CHECK(hp(nat::attachHardpointName(0, VS::Shoulders, shoulder, false)) == "HP_left_shoulderPad");
    CHECK_FALSE(nat::attachHardpointName(0, VS::RightHand, never, false).has_value());

    // Sheathed: shields to the sheath hardpoint, weapons to back or hip, the
    // helm untouched.
    CHECK(hp(nat::attachHardpointName(0, VS::RightHand, none, true)) == "HP_sheath_right_Back");
    CHECK(hp(nat::attachHardpointName(0, VS::LeftHand, none, true)) == "HP_sheath_left_Back");
    CHECK(hp(nat::attachHardpointName(0, VS::RightHand, hip, true)) == "HP_sheath_right_Hip");
    CHECK(hp(nat::attachHardpointName(0, VS::LeftHand, shield, true)) == "HP_sheath_Shield");
    CHECK(hp(nat::attachHardpointName(0, VS::Head, helm, true)) == "HP_helm");

    // Hardpoint_IsSheath (retail 0xAB9840) is case-insensitive.
    CHECK(nat::isSheathHardpoint("HP_sheath_left_Back"));
    CHECK(nat::isSheathHardpoint("hp_SHEATH_shield"));
    CHECK_FALSE(nat::isSheathHardpoint("HP_rightWeapon"));

    // The trait table rows crack against shipped type names.
    CHECK(nat::itemTypeTraits(nat::gbidHash("Shield")).shield);
    CHECK(nat::itemTypeTraits(nat::gbidHash("CrusaderShield")).offhandOnly);
    CHECK(nat::itemTypeTraits(nat::gbidHash("VoodooMask")).helm);
    CHECK(nat::itemTypeTraits(nat::gbidHash("Shoulders")).shoulder);
    CHECK(nat::itemTypeTraits(nat::gbidHash("Dagger")).sheathAtHip);
    CHECK_FALSE(nat::itemTypeTraits(nat::gbidHash("Sword")).shield);

    // g_HairStyleNames (retail 0x144CC50), BALD included.
    CHECK(nat::hairStyleName(nat::HairStyle::Naked) == std::string_view("NKD"));
    CHECK(nat::hairStyleName(nat::HairStyle::Helm1) == std::string_view("HLM1"));
    CHECK(nat::hairStyleName(nat::HairStyle::Helm2) == std::string_view("HLM2"));
    CHECK(nat::hairStyleName(nat::HairStyle::Bald) == std::string_view("BALD"));
}

TEST_CASE("d3_character_resolve_equip", "[d3][character]") {
    using VS = nat::EVisualSlot;
    using PC = nat::PlayerClass;
    using G = nat::Gender;

    // An armour item: the two look tags come through untouched, nothing is
    // attached.
    nat::Actor armour;
    armour.dwSnoId = 4242;
    armour.arTagMap = {{0, nat::kTagItemLookValue, 5}, {0, nat::kTagItemLookName, u32('B')}};
    const auto a = nat::resolveEquip(armour, {}, VS::Torso, PC::Barbarian, G::Male, false);
    CHECK(a.lookValue == 5u);
    CHECK(a.lookNameHash == u32('B'));
    CHECK(a.attachActorSno == -1);
    CHECK_FALSE(a.drawn);

    // A plain weapon in the right hand: its own actor at HP_rightWeapon.
    nat::Actor sword;
    sword.dwSnoId = 195174;
    const auto w = nat::resolveEquip(sword, {}, VS::RightHand, PC::Barbarian, G::Male, false);
    CHECK(w.drawn);
    CHECK(w.attachActorSno == 195174);
    CHECK(w.hardpoint == "HP_rightWeapon");
    CHECK_FALSE(w.secondAttach);

    // A per-class helm: the class/gender tag picks the model, the hair tag
    // comes along (measured pair from Helm_hell_base_01.acr).
    nat::Actor helmActor;
    helmActor.dwSnoId = 1000;
    helmActor.arTagMap = {{0, nat::kTagUsePerClassArt, 1},
                          {0, 0x17000, 90588},
                          {0, 0x17001, 95788},
                          {0, nat::kTagItemHairStyle, 2}};
    nat::ItemTypeTraits helmTraits;
    helmTraits.helm = true;
    const auto hm = nat::resolveEquip(helmActor, helmTraits, VS::Head, PC::Barbarian, G::Male, false);
    CHECK(hm.drawn);
    CHECK(hm.hardpoint == "HP_helm");
    CHECK(hm.attachActorSno == 90588);
    CHECK(hm.hairStyle == nat::HairStyle::Helm2);
    const auto hf = nat::resolveEquip(helmActor, helmTraits, VS::Head, PC::Barbarian, G::Female, false);
    CHECK(hf.attachActorSno == 95788);

    // Shoulders attach twice; without per-class art the twin is the same
    // model at the right pad.
    nat::Actor pads;
    pads.dwSnoId = 777;
    nat::ItemTypeTraits padTraits;
    padTraits.shoulder = true;
    const auto sh = nat::resolveEquip(pads, padTraits, VS::Shoulders, PC::Monk, G::Male, false);
    CHECK(sh.drawn);
    CHECK(sh.hardpoint == "HP_left_shoulderPad");
    CHECK(sh.secondAttach);
    CHECK(sh.secondAttachActorSno == 777);
}

TEST_CASE("d3_character_stem_inverse", "[d3][character]") {
    using PC = nat::PlayerClass;
    using G = nat::Gender;
    // Round-trip every class and gender, plus the suffixed rig variants the
    // installs ship, which are the same character.
    for (whiteout::i32 c = 0; c < static_cast<whiteout::i32>(nat::kPlayerClassCount); ++c) {
        for (const G g : {G::Male, G::Female}) {
            const auto cls = static_cast<PC>(c);
            const auto back = nat::playerFromAppearanceStem(nat::playerAppearanceStem(cls, g));
            REQUIRE(back.has_value());
            CHECK(back->first == cls);
            CHECK(back->second == g);
        }
    }
    const auto sel = nat::playerFromAppearanceStem("Barbarian_Male_characterSelect");
    REQUIRE(sel.has_value());
    CHECK(sel->first == PC::Barbarian);
    CHECK(sel->second == G::Male);
    CHECK(nat::playerFromAppearanceStem("barbarian_female").has_value()); // case-blind
    CHECK_FALSE(nat::playerFromAppearanceStem("SkeletonKing").has_value());
    CHECK_FALSE(nat::playerFromAppearanceStem("").has_value());
}

TEST_CASE("d3_character_class_from_type", "[d3][character]") {
    using PC = nat::PlayerClass;
    // The game's own encoding: the 44 <Slot>_<Class> spellings.
    CHECK(nat::playerClassFromTypeName("ChestArmor_Wizard") == PC::Wizard);
    CHECK(nat::playerClassFromTypeName("SpiritStone_Monk") == PC::Monk);
    CHECK(nat::playerClassFromTypeName("Helm_Necromancer") == PC::Necromancer);
    CHECK(nat::playerClassFromTypeName("belt_barbarian") == PC::Barbarian); // case-blind
    // The curated class-locked base families.
    CHECK(nat::playerClassFromTypeName("MightyWeapon1H") == PC::Barbarian);
    CHECK(nat::playerClassFromTypeName("CeremonialDagger") == PC::WitchDoctor);
    CHECK(nat::playerClassFromTypeName("HandXBow") == PC::DemonHunter);
    CHECK(nat::playerClassFromTypeName("NecromancerOffhand") == PC::Necromancer);
    // Neutral and near-miss spellings stay neutral: shared families, the
    // non-class underscore suffixes ItemTypeNames.stl really ships, and the
    // record stems that LOOK suffixed but are not types at all.
    CHECK_FALSE(nat::playerClassFromTypeName("Ring").has_value());
    CHECK_FALSE(nat::playerClassFromTypeName("Sword2H").has_value());
    CHECK_FALSE(nat::playerClassFromTypeName("Runestone_A").has_value());
    CHECK_FALSE(nat::playerClassFromTypeName("CraftingPlan_Smith").has_value());
    CHECK_FALSE(nat::playerClassFromTypeName("").has_value());

    // The six all-generic-typed sets, and nothing else.
    CHECK(nat::playerClassForSetKey("Ninja_Set_x1") == PC::DemonHunter);
    CHECK(nat::playerClassForSetKey("Monkey_King_Set_x1") == PC::Monk);
    CHECK(nat::playerClassForSetKey("Dot_Set_x1") == PC::WitchDoctor);
    CHECK(nat::playerClassForSetKey("Earthquake_Set_x1") == PC::Barbarian);
    CHECK(nat::playerClassForSetKey("Arcane_Wraps_Set_x1") == PC::Wizard);
    CHECK(nat::playerClassForSetKey("Thorns_Set_x1") == PC::Crusader);
    CHECK_FALSE(nat::playerClassForSetKey("Firebird_Set_x1").has_value());
}
