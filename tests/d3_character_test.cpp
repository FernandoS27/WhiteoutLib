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
