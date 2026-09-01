// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P3b — the document: `Resolve`, and the validation rules that make the
/// several-profiles-over-one-geometry shape safe. The gate is that a *clean*
/// document reports nothing at every level, and that each rule fires on exactly
/// the one thing it owns — a rule that fires on everything is not a rule.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/validate.h>

#include "wem_material_fixture.h"

using namespace whiteout;
using namespace whiteout::models::wem;
using namespace wemfix;

// ============================================================================
// Resolve
// ============================================================================

TEST_CASE("wem resolve is the only place profile and look meet", "[wem][document]") {
    Document document = makeDocument(ProfileId::Sc2);
    const Model& model = document.models[0];

    REQUIRE(Resolve(model, 0, ProfileId::Sc2) != nullptr);
    CHECK(Resolve(model, 0, ProfileId::Sc2)->name == "bodyMat");
    CHECK(Resolve(model, 1, ProfileId::Sc2)->name == "trimMat");

    // Everything out of range is a null, not an assert: `Validate` is what says
    // which of these a document actually has.
    CHECK(Resolve(model, 2, ProfileId::Sc2) == nullptr);
    CHECK(Resolve(model, 0, ProfileId::Wow) == nullptr);
    CHECK(Resolve(model, 0, ProfileId::Sc2, 1) == nullptr);

    CHECK(model.slotIndex("trim") == 1u);
    CHECK(model.slotIndex("nothing") == kInvalidIndex);
    CHECK(model.drawnProfiles() == ProfileBit(ProfileId::Sc2));
}

TEST_CASE("wem two profiles over one geometry resolve independently", "[wem][document]") {
    Document document = makeDocument(ProfileId::Wc3Classic);
    document.declare(ProfileId::Wc3Reforged);
    Model& model = document.models[0];

    ProfileMaterialSet reforged =
        makeSet(ProfileId::Wc3Reforged, {makeComposite("hdBody"), makeComposite("hdTrim")});
    for (Material& material : reforged.materials) {
        material.InitCommon().setKind(MaterialKind::PBRDeferred);
        material.InitCommon().pbr()->set(PbrSlot::BaseColor, makeInput(0));
    }
    model.profileSets.push_back(std::move(reforged));
    for (MeshSection& section : model.meshes[0].sections) {
        section.profiles |= ProfileBit(ProfileId::Wc3Reforged);
    }

    CHECK(Resolve(model, 0, ProfileId::Wc3Classic)->name == "bodyMat");
    CHECK(Resolve(model, 0, ProfileId::Wc3Reforged)->name == "hdBody");
    CHECK(Resolve(model, 0, ProfileId::Wc3Classic)->Common().kind() == MaterialKind::Composite);
    CHECK(Resolve(model, 0, ProfileId::Wc3Reforged)->Common().kind() == MaterialKind::PBRDeferred);

    // §6.3 rule 3: the sets are independent, so editing one leaves the other be.
    model.setFor(ProfileId::Wc3Reforged)->materials[0].MutableCommon().blend =
        BlendMode::AlphaBlend;
    CHECK(Resolve(model, 0, ProfileId::Wc3Classic)->Common().blend == BlendMode::Opaque);

    CHECK(errorCodes(Validate(document, ValidateLevel::Profile)) == "");
}

TEST_CASE("wem addSlot grows every set's bindings together", "[wem][document]") {
    Document document = makeDocument(ProfileId::Sc2);
    Model& model = document.models[0];

    CHECK(model.addSlot("body") == 0u); // already there; no growth
    CHECK(model.setFor(ProfileId::Sc2)->slotBindings.size() == 2u);

    const u32 added = model.addSlot("cape");
    CHECK(added == 2u);
    // The parallel-array invariant is the set's, so the set grows with the slot
    // rather than finding out later.
    REQUIRE(model.setFor(ProfileId::Sc2)->slotBindings.size() == 3u);
    CHECK(model.setFor(ProfileId::Sc2)->slotBindings[2].byLook.size() == 1u);
    CHECK_FALSE(model.setFor(ProfileId::Sc2)->slotBindings[2].bound(0));

    // A slot no section binds is informational, not an error — the coverage rule
    // is about slots sections actually name.
    const Diagnostics report = Validate(document, ValidateLevel::Profile);
    CHECK(errorCodes(report) == "");
    CHECK(report.countOf(DiagCode::MaterialSlotUnused) == 1u);
}

// ============================================================================
// A clean document reports nothing
// ============================================================================

TEST_CASE("wem a clean document validates at every level", "[wem][document][validate]") {
    const Document document = makeDocument(ProfileId::Sc2);
    CHECK(codes(Validate(document, ValidateLevel::Structural)) == "");
    CHECK(codes(Validate(document, ValidateLevel::Manifold)) == "");
    CHECK(codes(Validate(document, ValidateLevel::Profile)) == "");

    // Levels are cumulative, and each one this phase touched has rules now.
    CHECK(ValidationRulesFor(ValidateLevel::Structural).size() == 6u);
    CHECK(ValidationRulesFor(ValidateLevel::Manifold).size() == 1u);
    CHECK(ValidationRulesFor(ValidateLevel::Profile).size() == 3u);
}

// ============================================================================
// Structural rules
// ============================================================================

TEST_CASE("wem an undeclared profile set is structural", "[wem][document][validate]") {
    Document document = makeDocument(ProfileId::Sc2);
    document.models[0].profileSets[0].profile = ProfileId::Heroes;

    CHECK(errorCodes(Validate(document, ValidateLevel::Structural)) == "ProfileNotCarriedx1");

    document.declare(ProfileId::Heroes);
    CHECK(errorCodes(Validate(document, ValidateLevel::Structural)) == "");

    // And so is a default nothing declares.
    document.defaultProfile = ProfileId::Diablo3;
    CHECK(errorCodes(Validate(document, ValidateLevel::Structural)) == "ProfileNotCarriedx1");
}

TEST_CASE("wem binding shape is checked, not assumed", "[wem][document][validate]") {
    SECTION("byLook must match the look table") {
        Document document = makeDocument(ProfileId::Sc2);
        document.models[0].profileSets[0].looks.add("B");
        // The table grew and the bindings did not — a parse bug, per §8.
        CHECK(errorCodes(Validate(document, ValidateLevel::Structural)) ==
              "LookBindingMalformedx2");
    }

    SECTION("one binding per slot") {
        Document document = makeDocument(ProfileId::Sc2);
        document.models[0].profileSets[0].slotBindings.pop_back();
        CHECK(errorCodes(Validate(document, ValidateLevel::Structural)) == "IndexOutOfRangex1");
    }

    SECTION("a set always has a look") {
        Document document = makeDocument(ProfileId::Sc2);
        document.models[0].profileSets[0].looks.looks.clear();
        for (SlotBinding& binding : document.models[0].profileSets[0].slotBindings) {
            binding.byLook.clear();
        }
        CHECK(errorCodes(Validate(document, ValidateLevel::Structural)) ==
              "LookBindingMalformedx1");
    }
}

TEST_CASE("wem body invariants are enforced per kind", "[wem][document][validate]") {
    SECTION("a deferred slot appears at most once") {
        Document document = makeDocument(ProfileId::Sc2);
        CommonMaterial& common = document.models[0].profileSets[0].materials[0].InitCommon();
        common.setKind(MaterialKind::LegacyDeferred);
        // Straight into the vector, which is what a bad parse would do; `set` is
        // an upsert and could not produce this.
        common.legacy()->slots.emplace_back(LegacySlot::Diffuse, TextureInput{});
        common.legacy()->slots.emplace_back(LegacySlot::Diffuse, TextureInput{});
        CHECK(
            Validate(document, ValidateLevel::Structural).countOf(DiagCode::MaterialBodyInvalid) ==
            1u);
    }

    SECTION("Orm and the unpacked three are alternatives") {
        Document document = makeDocument(ProfileId::Sc2);
        CommonMaterial& common = document.models[0].profileSets[0].materials[0].InitCommon();
        common.setKind(MaterialKind::PBRDeferred);
        common.pbr()->set(PbrSlot::Orm, makeInput(1));
        common.pbr()->set(PbrSlot::Roughness, makeInput(2));
        CHECK(
            Validate(document, ValidateLevel::Structural).countOf(DiagCode::MaterialBodyInvalid) ==
            1u);

        // Either alone is fine.
        common.setKind(MaterialKind::PBRDeferred);
        common.pbr()->set(PbrSlot::Orm, makeInput(1));
        CHECK(
            Validate(document, ValidateLevel::Structural).countOf(DiagCode::MaterialBodyInvalid) ==
            0u);
    }

    SECTION("one feature of a kind per layer, and no repeated ids") {
        Document document = makeDocument(ProfileId::Sc2);
        CommonMaterial& common = document.models[0].profileSets[0].materials[0].InitCommon();
        MaterialFeature a;
        a.id = 1;
        a.layer = 0;
        a.payload = UvAnimationFeature{};
        MaterialFeature b = a;
        b.id = 2;
        common.features = {a, b};
        CHECK(
            Validate(document, ValidateLevel::Structural).countOf(DiagCode::MaterialBodyInvalid) ==
            1u);

        // Different layers is fine; a repeated id never is, because sub-tracks
        // join on it.
        common.features[1].layer = 1;
        CHECK(
            Validate(document, ValidateLevel::Structural).countOf(DiagCode::MaterialBodyInvalid) ==
            0u);
        common.features[1].id = 1;
        CHECK(
            Validate(document, ValidateLevel::Structural).countOf(DiagCode::MaterialBodyInvalid) ==
            1u);
    }
}

TEST_CASE("wem a native block must agree with its set's profile", "[wem][document][validate]") {
    Document document = makeDocument(ProfileId::Sc2);
    Material& material = document.models[0].profileSets[0].materials[0];

    material.SetNativeAuthoritative(native::M3Material{});
    CHECK(errorCodes(Validate(document, ValidateLevel::Structural)) == "");

    // An `.m2` record in an SC2 set: the §6.5 site that catches a converter
    // writing into the wrong array.
    material.SetNativeAuthoritative(native::M2Material{});
    CHECK(errorCodes(Validate(document, ValidateLevel::Structural)) ==
          "NativeKindProfileMismatchx1");
}

// ============================================================================
// Profile rules
// ============================================================================

TEST_CASE("wem coverage holes are errors, not fallbacks", "[wem][document][validate]") {
    Document document = makeDocument(ProfileId::Sc2);
    document.models[0].profileSets[0].slotBindings[1].byLook[0] = kInvalidIndex;

    const Diagnostics report = Validate(document, ValidateLevel::Profile);
    CHECK(errorCodes(report) == "SlotNotBoundx1");
    // Structural is silent about it — an unbound slot is well-formed, it is just
    // not covered, and the two questions have different answers.
    CHECK(errorCodes(Validate(document, ValidateLevel::Structural)) == "");
}

TEST_CASE("wem a section drawn in a profile with no set is a coverage error",
          "[wem][document][validate]") {
    Document document = makeDocument(ProfileId::Sc2);
    document.declare(ProfileId::Heroes);
    document.models[0].meshes[0].sections[0].profiles |= ProfileBit(ProfileId::Heroes);

    CHECK(errorCodes(Validate(document, ValidateLevel::Profile)) == "ProfileCoverageIncompletex1");
}

TEST_CASE("wem a section nothing draws is reported", "[wem][document][validate]") {
    Document document = makeDocument(ProfileId::Sc2);
    document.models[0].meshes[0].sections[1].profiles = kNoProfiles;

    const Diagnostics report = Validate(document, ValidateLevel::Profile);
    CHECK(errorCodes(report) == "");
    CHECK(report.countOf(DiagCode::SectionUndrawn) == 1u);
    // …and its slot then has no section naming it, which is the informational
    // half of the same fact.
    CHECK(report.countOf(DiagCode::MaterialSlotUnused) == 1u);
}

TEST_CASE("wem profile limits are checked at the profile level", "[wem][document][validate]") {
    SECTION("a kind the profile's exporter does not accept") {
        Document document = makeDocument(ProfileId::Sc2);
        // SC2 takes Composite only.
        document.models[0].profileSets[0].materials[0].InitCommon().setKind(
            MaterialKind::Combiners);
        CHECK(errorCodes(Validate(document, ValidateLevel::Profile)) ==
              "UnsupportedMaterialKindx1");
    }

    SECTION("a blend mode the format cannot write") {
        Document document = makeDocument(ProfileId::Sc2);
        // M3 has no alpha-key mode; the exporter would have to approximate.
        document.models[0].profileSets[0].materials[0].InitCommon().blend = BlendMode::AlphaKey;
        const Diagnostics report = Validate(document, ValidateLevel::Profile);
        CHECK(errorCodes(report) == "");
        CHECK(report.countOf(DiagCode::LossyBlendMode) == 1u);
    }

    SECTION("more uv sets than the profile has") {
        Document document = makeDocument(ProfileId::Wc3Classic);
        Mesh& mesh = document.models[0].meshes[0];
        mesh.attributes.create(geom::names::uv(0), geom::Domain::Halfedge, geom::AttrType::F32x2);
        mesh.attributes.create(geom::names::uv(1), geom::Domain::Halfedge, geom::AttrType::F32x2);
        // Classic WC3 has one UV set; the second is a warning, not a failure, and
        // the geometry is not touched.
        const Diagnostics report = Validate(document, ValidateLevel::Profile);
        CHECK(errorCodes(report) == "");
        CHECK(report.countOf(DiagCode::UvSetLimit) == 1u);
    }
}
