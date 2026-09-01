// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P3b — §7.5's removal operations. Ordinals shift under editing, and the
/// point of these being operations rather than vector erases is that a shift
/// never silently retargets a referencer. So every case here checks two things:
/// what moved, and what was *told* that its target died.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/materials/ops.h>
#include <whiteout/models/wem/validate.h>

#include "wem_material_fixture.h"

using namespace whiteout;
using namespace whiteout::models::wem;
using namespace wemfix;

namespace {

/// Adds a third slot and a third material so a removal has both a lower and a
/// higher referencer to move.
Model makeThreeSlotModel() {
    Model model;
    model.name = "model";
    model.materialSlots = {"a", "b", "c"};
    model.meshes.push_back(makeMesh({"a", "b", "c"}));
    for (MeshSection& section : model.meshes[0].sections) {
        section.profiles = ProfileBit(ProfileId::Sc2);
    }
    std::vector<Material> materials;
    materials.push_back(makeComposite("m0"));
    materials.push_back(makeComposite("m1"));
    materials.push_back(makeComposite("m2"));
    model.profileSets.push_back(makeSet(ProfileId::Sc2, std::move(materials)));
    return model;
}

u32 boundMaterial(const Model& model, ProfileId profile, u32 slot, u32 look = 0) {
    return model.setFor(profile)->slotBindings[slot].byLook[look];
}

} // namespace

// ============================================================================
// RemoveMaterial
// ============================================================================

TEST_CASE("wem remove material rewrites higher bindings and invalidates its own",
          "[wem][materials][ops]") {
    Model model = makeThreeSlotModel();

    const RemovalResult result = RemoveMaterial(model, ProfileId::Sc2, 1);
    REQUIRE(result.removed);
    CHECK(result.invalidated == 1u);
    CHECK(result.rewritten == 1u);
    CHECK(codes(result.diagnostics) == "SlotNotBoundx1");

    const ProfileMaterialSet* set = model.setFor(ProfileId::Sc2);
    REQUIRE(set->materials.size() == 2u);
    CHECK(set->materials[0].name == "m0");
    CHECK(set->materials[1].name == "m2");

    CHECK(boundMaterial(model, ProfileId::Sc2, 0) == 0u);
    // The hole, not a repoint: "which material did you mean" is unanswerable here.
    CHECK(boundMaterial(model, ProfileId::Sc2, 1) == kInvalidIndex);
    // And the one above the removal followed it down.
    CHECK(boundMaterial(model, ProfileId::Sc2, 2) == 1u);
}

TEST_CASE("wem remove material refuses an absent set or index", "[wem][materials][ops]") {
    Model model = makeThreeSlotModel();

    const RemovalResult noSet = RemoveMaterial(model, ProfileId::Wow, 0);
    CHECK_FALSE(noSet.removed);
    CHECK(codes(noSet.diagnostics) == "ProfileNotCarriedx1");

    const RemovalResult noMaterial = RemoveMaterial(model, ProfileId::Sc2, 9);
    CHECK_FALSE(noMaterial.removed);
    CHECK(codes(noMaterial.diagnostics) == "IndexOutOfRangex1");
    CHECK(model.setFor(ProfileId::Sc2)->materials.size() == 3u);
}

// ============================================================================
// RemoveLayer
// ============================================================================

TEST_CASE("wem remove layer moves feature ordinals but never feature ids",
          "[wem][materials][ops]") {
    Model model = makeThreeSlotModel();
    ProfileMaterialSet& set = *model.setFor(ProfileId::Sc2);

    // makeComposite("m0", 2) is Color/Set, Color/Mod, Color/Mod, Normal/Set.
    set.materials[0] = makeComposite("m0", 2);
    CommonMaterial& common = set.materials[0].InitCommon();
    REQUIRE(common.ordinalCount() == 4u);

    MaterialFeature onFirst;
    onFirst.id = 10;
    onFirst.layer = 0;
    onFirst.payload = UvAnimationFeature{};
    MaterialFeature onDoomed;
    onDoomed.id = 11;
    onDoomed.layer = 1;
    onDoomed.payload = UvAnimationFeature{};
    MaterialFeature onLast;
    onLast.id = 12;
    onLast.layer = 3;
    onLast.payload = UvAnimationFeature{};
    MaterialFeature whole;
    whole.id = 13;
    whole.payload = FresnelFeature{};
    common.features = {onFirst, onDoomed, onLast, whole};

    // The material came from a file, so a native block is what it would carry.
    set.materials[0].SetNativeAuthoritative(native::M3Material{});

    const RemovalResult result = RemoveLayer(model, ProfileId::Sc2, 0, 1);
    REQUIRE(result.removed);
    CHECK(result.invalidated == 1u);
    CHECK(result.rewritten == 1u);
    CHECK(codes(result.diagnostics) == "FeatureDroppedx1");

    const CommonMaterial& after = set.materials[0].Common();
    REQUIRE(after.ordinalCount() == 3u);
    REQUIRE(after.features.size() == 3u);
    CHECK(after.features[0].id == 10u);
    CHECK(after.features[0].layer == 0u); // below the removal: untouched
    CHECK(after.features[1].id == 12u);
    CHECK(after.features[1].layer == 2u); // above it: shifted down, id kept
    CHECK(after.features[2].id == 13u);
    CHECK(after.features[2].layer == kWholeMaterial);

    // The block now describes a layer count the common material does not have,
    // which is exactly what `CommonEdited` means (§7.1).
    CHECK(set.materials[0].sync() == NativeSync::CommonEdited);
    CHECK(set.materials[0].NeedsNativeReDerive());
}

TEST_CASE("wem remove layer works on every kind's ordinal space", "[wem][materials][ops]") {
    Model model = makeThreeSlotModel();
    ProfileMaterialSet& set = *model.setFor(ProfileId::Sc2);

    CommonMaterial& common = set.materials[0].InitCommon();
    common.setKind(MaterialKind::PBRDeferred);
    common.pbr()->set(PbrSlot::BaseColor, makeInput(1));
    common.pbr()->set(PbrSlot::Normal, makeInput(2));
    common.pbr()->set(PbrSlot::Emissive, makeInput(3));

    REQUIRE(RemoveLayer(model, ProfileId::Sc2, 0, 1).removed);
    const PbrDeferredBody* body = set.materials[0].Common().pbr();
    REQUIRE(body != nullptr);
    REQUIRE(body->slots.size() == 2u);
    CHECK(body->find(PbrSlot::BaseColor) != nullptr);
    CHECK(body->find(PbrSlot::Normal) == nullptr);
    CHECK(body->find(PbrSlot::Emissive) != nullptr);

    const RemovalResult past = RemoveLayer(model, ProfileId::Sc2, 0, 5);
    CHECK_FALSE(past.removed);
    CHECK(codes(past.diagnostics) == "IndexOutOfRangex1");
}

// ============================================================================
// RemoveFeature
// ============================================================================

TEST_CASE("wem remove feature leaves every ordinal alone", "[wem][materials][ops]") {
    Model model = makeThreeSlotModel();
    ProfileMaterialSet& set = *model.setFor(ProfileId::Sc2);
    CommonMaterial& common = set.materials[0].InitCommon();

    MaterialFeature first;
    first.id = 4;
    first.layer = 0;
    first.payload = UvAnimationFeature{};
    MaterialFeature second;
    second.id = 9;
    second.layer = 1;
    second.payload = UvAnimationFeature{};
    common.features = {first, second};
    const u32 ordinalsBefore = common.ordinalCount();

    const RemovalResult result = RemoveFeature(model, ProfileId::Sc2, 0, 4);
    REQUIRE(result.removed);
    CHECK(result.rewritten == 0u);
    CHECK(codes(result.diagnostics) == "");

    const CommonMaterial& after = set.materials[0].Common();
    CHECK(after.ordinalCount() == ordinalsBefore);
    REQUIRE(after.features.size() == 1u);
    // A feature is not something other features are numbered relative to, so
    // neither the surviving id nor its layer moves.
    CHECK(after.features[0].id == 9u);
    CHECK(after.features[0].layer == 1u);

    const RemovalResult missing = RemoveFeature(model, ProfileId::Sc2, 0, 4);
    CHECK_FALSE(missing.removed);
    CHECK(codes(missing.diagnostics) == "IndexOutOfRangex1");
}

// ============================================================================
// RemoveLook
// ============================================================================

TEST_CASE("wem remove look drops one column of every binding", "[wem][materials][ops]") {
    Model model = makeThreeSlotModel();
    ProfileMaterialSet& set = *model.setFor(ProfileId::Sc2);
    set.looks.looks[0].name = "A";
    set.looks.add("B");
    set.looks.add("C");
    for (std::size_t slot = 0; slot < set.slotBindings.size(); ++slot) {
        set.slotBindings[slot].byLook = {0, 1, 2};
    }

    const RemovalResult result = RemoveLook(model, ProfileId::Sc2, 1);
    REQUIRE(result.removed);
    CHECK(result.rewritten == 3u);
    REQUIRE(set.looks.size() == 2u);
    CHECK(set.looks.looks[0].name == "A");
    CHECK(set.looks.looks[1].name == "C");
    for (const SlotBinding& binding : set.slotBindings) {
        REQUIRE(binding.byLook.size() == 2u);
        CHECK(binding.byLook[0] == 0u);
        CHECK(binding.byLook[1] == 2u);
    }
    CHECK(set.looks.find("C") == 1u);
    CHECK(set.looks.find("B") == kInvalidIndex);
}

TEST_CASE("wem remove look refuses the last one", "[wem][materials][ops]") {
    Model model = makeThreeSlotModel();
    const RemovalResult result = RemoveLook(model, ProfileId::Sc2, 0);
    CHECK_FALSE(result.removed);
    CHECK(codes(result.diagnostics) == "IndexOutOfRangex1");
    CHECK(model.setFor(ProfileId::Sc2)->looks.size() == 1u);
}

// ============================================================================
// RemoveProfileSet
// ============================================================================

TEST_CASE("wem remove profile set clears the section bits it owned", "[wem][materials][ops]") {
    Model model = makeThreeSlotModel();
    // A second profile over the same geometry — the two-set case §6.3 exists for.
    model.profileSets.push_back(makeSet(
        ProfileId::Wow, {makeComposite("wow0"), makeComposite("wow1"), makeComposite("wow2")}));
    for (MeshSection& section : model.meshes[0].sections) {
        section.profiles |= ProfileBit(ProfileId::Wow);
    }
    // …except the last section, which only SC2 drew.
    model.meshes[0].sections[2].profiles = ProfileBit(ProfileId::Sc2);

    const RemovalResult result = RemoveProfileSet(model, ProfileId::Sc2);
    REQUIRE(result.removed);
    CHECK(model.setFor(ProfileId::Sc2) == nullptr);
    CHECK(model.setFor(ProfileId::Wow) != nullptr);
    CHECK(result.rewritten == 3u);

    // The SC2-only section is now drawn by nobody. Reported, and still there:
    // dropping geometry is not a side effect this operation gets to have.
    CHECK(result.invalidated == 1u);
    CHECK(codes(result.diagnostics) == "SectionUndrawnx1");
    CHECK(model.meshes[0].sections.size() == 3u);
    CHECK(model.meshes[0].sections[2].profiles == kNoProfiles);
    CHECK(model.drawnProfiles() == ProfileBit(ProfileId::Wow));
}

// ============================================================================
// The referencer table
// ============================================================================

TEST_CASE("wem material referencer check finds every dangling index", "[wem][materials][ops]") {
    Model model = makeThreeSlotModel();
    {
        Diagnostics clean;
        CheckMaterialReferencers(model, 0, clean);
        CHECK(codes(clean) == "");
    }

    ProfileMaterialSet& set = *model.setFor(ProfileId::Sc2);
    set.slotBindings[0].byLook[0] = 7;                    // past the material array
    set.materials[1].InitCommon().features.push_back({}); // ordinal 0 exists…
    set.materials[1].InitCommon().features[0].layer = 40; // …40 does not
    model.meshes[0].sections[1].materialSlot = 12;        // past the slot list

    Diagnostics broken;
    CheckMaterialReferencers(model, 0, broken);
    CHECK(codes(broken) == "IndexOutOfRangex3");

    // A deliberately unbound slot is a coverage question, not a dangling index —
    // the two are reported by different rules on purpose.
    set.slotBindings[0].byLook[0] = kInvalidIndex;
    set.materials[1].InitCommon().features.clear();
    model.meshes[0].sections[1].materialSlot = 1;
    Diagnostics holeOnly;
    CheckMaterialReferencers(model, 0, holeOnly);
    CHECK(codes(holeOnly) == "");
}
