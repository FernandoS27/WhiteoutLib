// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P0 — the profile registry is pure data, so every assertion here is a
/// documented value from WEM_DESIGN.md §6. A change to a number below is a design
/// change, not a refactor.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/profile.h>

using namespace whiteout;
using namespace whiteout::models;

TEST_CASE("wem profile registry covers all seven profiles", "[wem][profile]") {
    const auto all = wem::AllProfileDescs();
    REQUIRE(all.size() == static_cast<std::size_t>(wem::ProfileId::Count));
    REQUIRE(all.size() == 7);

    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto id = static_cast<wem::ProfileId>(i);
        INFO("profile index " << i);
        // The registry is indexed, never searched: index and id must agree.
        CHECK(all[i].id == id);
        CHECK(&wem::Profile(id) == &all[i]);
        CHECK(all[i].name != nullptr);
        CHECK(all[i].displayName != nullptr);
        CHECK(std::string(all[i].name) == wem::ToString(id));
        CHECK(wem::ProfileFromName(all[i].name) == id);
    }

    CHECK(wem::ProfileFromName("not_a_profile") == wem::ProfileId::Count);
}

TEST_CASE("wem profile names and format ids", "[wem][profile]") {
    // §6.1: profiles are named for the game; formatId names the converter that
    // serves them, and is null only for generic, which none does.
    CHECK(std::string(wem::Profile(wem::ProfileId::Generic).name) == "generic");
    CHECK(std::string(wem::Profile(wem::ProfileId::Wc3Classic).name) == "wc3_classic");
    CHECK(std::string(wem::Profile(wem::ProfileId::Wc3Reforged).name) == "wc3_reforged");
    CHECK(std::string(wem::Profile(wem::ProfileId::Wow).name) == "wow");
    CHECK(std::string(wem::Profile(wem::ProfileId::Sc2).name) == "sc2");
    CHECK(std::string(wem::Profile(wem::ProfileId::Heroes).name) == "heroes");
    CHECK(std::string(wem::Profile(wem::ProfileId::Diablo3).name) == "diablo3");

    CHECK(wem::Profile(wem::ProfileId::Generic).formatId == nullptr);
    CHECK(std::string(wem::Profile(wem::ProfileId::Wc3Classic).formatId) == "mdx");
    CHECK(std::string(wem::Profile(wem::ProfileId::Wc3Reforged).formatId) == "mdx");
    CHECK(std::string(wem::Profile(wem::ProfileId::Wow).formatId) == "m2");
    CHECK(std::string(wem::Profile(wem::ProfileId::Sc2).formatId) == "m3");
    CHECK(std::string(wem::Profile(wem::ProfileId::Heroes).formatId) == "m3");
    CHECK(std::string(wem::Profile(wem::ProfileId::Diablo3).formatId) == "d3");
}

TEST_CASE("wem profile source spaces", "[wem][profile]") {
    // §6.4: WEM's canonical space is Blizzard. Five of the six game profiles are
    // the identity; SC2 and Heroes are the 90 degree yaw.
    CHECK(wem::Profile(wem::ProfileId::Generic).sourceSpace == wem::CoordSpace::Blizzard);
    CHECK(wem::Profile(wem::ProfileId::Wc3Classic).sourceSpace == wem::CoordSpace::Blizzard);
    CHECK(wem::Profile(wem::ProfileId::Wc3Reforged).sourceSpace == wem::CoordSpace::Blizzard);
    CHECK(wem::Profile(wem::ProfileId::Wow).sourceSpace == wem::CoordSpace::Blizzard);
    CHECK(wem::Profile(wem::ProfileId::Sc2).sourceSpace == wem::CoordSpace::Sc2);
    CHECK(wem::Profile(wem::ProfileId::Heroes).sourceSpace == wem::CoordSpace::Sc2);
    CHECK(wem::Profile(wem::ProfileId::Diablo3).sourceSpace == wem::CoordSpace::Blizzard);

    // Every Blizzard format is right-handed with CCW front faces, which is why no
    // importer reverses indices.
    for (const auto& desc : wem::AllProfileDescs()) {
        CHECK(desc.handedness == wem::Handedness::Right);
        CHECK(desc.winding == wem::WindingOrder::CounterClockwise);
    }
}

TEST_CASE("wem profile geometry limits", "[wem][profile]") {
    // §6.2: maxUvSets is 1 wc3_classic | 2 wc3_reforged, wow, diablo3 | 5 sc2, heroes.
    CHECK(wem::Profile(wem::ProfileId::Wc3Classic).maxUvSets == 1);
    CHECK(wem::Profile(wem::ProfileId::Wc3Reforged).maxUvSets == 2);
    CHECK(wem::Profile(wem::ProfileId::Wow).maxUvSets == 2);
    CHECK(wem::Profile(wem::ProfileId::Sc2).maxUvSets == 5);
    CHECK(wem::Profile(wem::ProfileId::Heroes).maxUvSets == 5);
    CHECK(wem::Profile(wem::ProfileId::Diablo3).maxUvSets == 2);

    // Generic is permissive by construction: it must never be narrower than a
    // profile something can be derived from.
    const auto& generic = wem::Profile(wem::ProfileId::Generic);
    for (const auto& desc : wem::AllProfileDescs()) {
        INFO("against profile " << desc.name);
        CHECK(generic.maxUvSets >= desc.maxUvSets);
        CHECK(generic.maxBoneInfluences >= desc.maxBoneInfluences);
        CHECK((generic.commonKinds & desc.commonKinds) == desc.commonKinds);
    }
    CHECK(generic.allowsNgons);
    CHECK(generic.indexWidth == wem::IndexWidth::U32);

    // "4 on the GPU everywhere; D3 writes 3."
    CHECK(wem::Profile(wem::ProfileId::Diablo3).maxBoneInfluences == 3);
    CHECK(wem::Profile(wem::ProfileId::Wc3Classic).maxBoneInfluences == 4);
    CHECK(wem::Profile(wem::ProfileId::Wow).maxBoneInfluences == 4);
    CHECK(wem::Profile(wem::ProfileId::Sc2).maxBoneInfluences == 4);

    // "U16 for all six today"; "false for all six" for n-gons.
    for (std::size_t i = 1; i < static_cast<std::size_t>(wem::ProfileId::Count); ++i) {
        const auto& desc = wem::Profile(static_cast<wem::ProfileId>(i));
        INFO("profile " << desc.name);
        CHECK(desc.indexWidth == wem::IndexWidth::U16);
        CHECK_FALSE(desc.allowsNgons);
    }
}

TEST_CASE("wem profile scene scale is the renderer's framing constant", "[wem][profile]") {
    // §6.2: recorded for reference, applied only when RetargetOptions::rescale asks.
    // The values mirror the renderer's own constants.
    CHECK(wem::Profile(wem::ProfileId::Wc3Classic).sceneScale == 1.0f);
    CHECK(wem::Profile(wem::ProfileId::Wc3Reforged).sceneScale == 1.0f);
    CHECK(wem::Profile(wem::ProfileId::Wow).sceneScale == 100.0f);
    CHECK(wem::Profile(wem::ProfileId::Sc2).sceneScale == 100.0f);
    CHECK(wem::Profile(wem::ProfileId::Heroes).sceneScale == 100.0f);
    CHECK(wem::Profile(wem::ProfileId::Diablo3).sceneScale == 17.0f);
}

TEST_CASE("wem profile common kinds match the 7.2.6 table", "[wem][profile]") {
    using wem::HasMaterialKind;
    using wem::MaterialKind;

    const auto kinds = [](wem::ProfileId id) { return wem::Profile(id).commonKinds; };

    // Wc3Classic: Composite, and Combiners when the stack collapses (§7.2.2).
    CHECK(HasMaterialKind(kinds(wem::ProfileId::Wc3Classic), MaterialKind::Composite));
    CHECK(HasMaterialKind(kinds(wem::ProfileId::Wc3Classic), MaterialKind::Combiners));
    CHECK_FALSE(HasMaterialKind(kinds(wem::ProfileId::Wc3Classic), MaterialKind::PBRDeferred));

    // Wc3Reforged: PBRDeferred.
    CHECK(kinds(wem::ProfileId::Wc3Reforged) == wem::MaterialKindBit(MaterialKind::PBRDeferred));

    // Wow: Combiners — stage order and combine ops are the material.
    CHECK(kinds(wem::ProfileId::Wow) == wem::MaterialKindBit(MaterialKind::Combiners));

    // Sc2 / Heroes: Composite.
    CHECK(kinds(wem::ProfileId::Sc2) == wem::MaterialKindBit(MaterialKind::Composite));
    CHECK(kinds(wem::ProfileId::Heroes) == wem::MaterialKindBit(MaterialKind::Composite));

    // Diablo3: Combiners when a Legacy stage block exists; else LegacyDeferred.
    CHECK(HasMaterialKind(kinds(wem::ProfileId::Diablo3), MaterialKind::Combiners));
    CHECK(HasMaterialKind(kinds(wem::ProfileId::Diablo3), MaterialKind::LegacyDeferred));
    CHECK_FALSE(HasMaterialKind(kinds(wem::ProfileId::Diablo3), MaterialKind::PBRDeferred));

    // Generic: any of the four.
    CHECK(kinds(wem::ProfileId::Generic) == wem::kAllMaterialKinds);
}

TEST_CASE("wem profile native kinds: seven profiles, four block kinds", "[wem][profile]") {
    // §6.1's table verbatim.
    CHECK(wem::Profile(wem::ProfileId::Generic).nativeMaterialKind == wem::NativeKind::None);
    CHECK(wem::Profile(wem::ProfileId::Wc3Classic).nativeMaterialKind == wem::NativeKind::Mdx);
    CHECK(wem::Profile(wem::ProfileId::Wc3Reforged).nativeMaterialKind == wem::NativeKind::Mdx);
    CHECK(wem::Profile(wem::ProfileId::Wow).nativeMaterialKind == wem::NativeKind::M2);
    CHECK(wem::Profile(wem::ProfileId::Sc2).nativeMaterialKind == wem::NativeKind::M3);
    CHECK(wem::Profile(wem::ProfileId::Heroes).nativeMaterialKind == wem::NativeKind::M3);
    CHECK(wem::Profile(wem::ProfileId::Diablo3).nativeMaterialKind == wem::NativeKind::D3);

    // The two shared-kind pairs are what makes §6.6's cheap-derive path possible.
    CHECK(wem::Profile(wem::ProfileId::Wc3Classic).nativeMaterialKind ==
          wem::Profile(wem::ProfileId::Wc3Reforged).nativeMaterialKind);
    CHECK(wem::Profile(wem::ProfileId::Sc2).nativeMaterialKind ==
          wem::Profile(wem::ProfileId::Heroes).nativeMaterialKind);
}

TEST_CASE("wem profile blend mode sets", "[wem][profile]") {
    const auto& mdx = wem::Profile(wem::ProfileId::Wc3Classic);
    // MDX filterMode 1 is Transparent — the alpha-test mode no other format spells.
    CHECK(mdx.acceptsBlendMode(wem::BlendMode::Transparent));
    CHECK_FALSE(mdx.acceptsBlendMode(wem::BlendMode::AlphaKey));
    CHECK_FALSE(mdx.acceptsBlendMode(wem::BlendMode::BlendAdd));

    const auto& m2 = wem::Profile(wem::ProfileId::Wow);
    CHECK(m2.blendModes.size() == 8); // M2 blendingMode 0..7
    CHECK(m2.acceptsBlendMode(wem::BlendMode::BlendAdd));
    CHECK_FALSE(m2.acceptsBlendMode(wem::BlendMode::Transparent));

    const auto& m3 = wem::Profile(wem::ProfileId::Sc2);
    CHECK(m3.blendModes.size() == 6); // M3 BlendMode 0..5
    CHECK_FALSE(m3.acceptsBlendMode(wem::BlendMode::AlphaKey));

    // Generic accepts everything WEM can name, so a derive never fails on blend.
    const auto& generic = wem::Profile(wem::ProfileId::Generic);
    for (const auto& desc : wem::AllProfileDescs()) {
        for (wem::BlendMode mode : desc.blendModes) {
            INFO(desc.name << " -> " << wem::ToString(mode));
            CHECK(generic.acceptsBlendMode(mode));
        }
    }
}

TEST_CASE("wem profile looks and actors", "[wem][profile]") {
    // §8: looks are D3's variant table and WoW's texture variations. §9: actors
    // are Diablo3's today.
    CHECK(wem::Profile(wem::ProfileId::Diablo3).supportsLooks);
    CHECK(wem::Profile(wem::ProfileId::Wow).supportsLooks);
    CHECK_FALSE(wem::Profile(wem::ProfileId::Wc3Classic).supportsLooks);
    CHECK_FALSE(wem::Profile(wem::ProfileId::Sc2).supportsLooks);

    CHECK(wem::Profile(wem::ProfileId::Diablo3).supportsActors);
    CHECK_FALSE(wem::Profile(wem::ProfileId::Wow).supportsActors);
    CHECK_FALSE(wem::Profile(wem::ProfileId::Sc2).supportsActors);
}

TEST_CASE("wem profile masks", "[wem][profile]") {
    wem::ProfileMask mask = wem::kNoProfiles;
    CHECK_FALSE(wem::HasProfile(mask, wem::ProfileId::Wow));

    mask |= wem::ProfileBit(wem::ProfileId::Wow);
    mask |= wem::ProfileBit(wem::ProfileId::Sc2);
    CHECK(wem::HasProfile(mask, wem::ProfileId::Wow));
    CHECK(wem::HasProfile(mask, wem::ProfileId::Sc2));
    CHECK_FALSE(wem::HasProfile(mask, wem::ProfileId::Heroes));
    CHECK_FALSE(wem::HasProfile(mask, wem::ProfileId::Generic));

    for (std::size_t i = 0; i < static_cast<std::size_t>(wem::ProfileId::Count); ++i) {
        CHECK(wem::HasProfile(wem::kAllProfiles, static_cast<wem::ProfileId>(i)));
    }
}

TEST_CASE("wem profile enum spellings are stable", "[wem][profile]") {
    CHECK(std::string(wem::ToString(wem::CoordSpace::Sc2)) == "sc2");
    CHECK(std::string(wem::ToString(wem::BlendMode::AdditiveAlpha)) == "additive_alpha");
    CHECK(std::string(wem::ToString(wem::MaterialKind::PBRDeferred)) == "pbr_deferred");
    CHECK(std::string(wem::ToString(wem::NativeKind::M3)) == "m3");
    CHECK(std::string(wem::ToString(wem::ProfileId::Count)) == "invalid");
}
