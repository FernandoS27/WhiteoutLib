// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P3b — §6.6's two operations. They are kept distinct on purpose:
/// `AddProfileFromImport` brings a second file's set over a geometry it must
/// match, and `DeriveProfile` invents one and is always lossy. So what is
/// asserted here is mostly the *report*: a derive that produces approximations
/// silently is the failure mode, not a derive that produces them.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/validate.h>

#include "wem_material_fixture.h"

using namespace whiteout;
using namespace whiteout::models::wem;
using namespace wemfix;

namespace {

Material makeCombiners(const std::string& name, u32 texture, CombinerOp second) {
    Material material;
    material.name = name;
    CombinersBody body;
    CombinerStage first;
    first.input = makeInput(texture);
    body.stages.push_back(first);
    CombinerStage next;
    next.input = makeInput(texture + 1);
    next.rgb = second;
    next.alpha = second;
    body.stages.push_back(next);
    material.InitCommon().body = std::move(body);
    return material;
}

Material makePbr(const std::string& name, bool withTeamColor) {
    Material material;
    material.name = name;
    PbrDeferredBody body;
    body.set(PbrSlot::BaseColor, makeInput(0));
    body.set(PbrSlot::Normal, makeInput(1));
    if (withTeamColor) {
        body.set(PbrSlot::TeamColorMask, makeInput(2));
    }
    material.InitCommon().body = std::move(body);
    return material;
}

/// A document whose one set belongs to @p profile, built from @p materials.
Document documentWith(ProfileId profile, std::vector<Material> materials) {
    Document document = makeDocument(profile);
    document.models[0].profileSets[0] = makeSet(profile, std::move(materials));
    return document;
}

} // namespace

// ============================================================================
// DeriveProfile — the refusals
// ============================================================================

TEST_CASE("wem derive refuses a source the document does not carry", "[wem][derive]") {
    Document document = makeDocument(ProfileId::Sc2);
    const DeriveResult result = DeriveProfile(document, ProfileId::Wow, ProfileId::Sc2);
    CHECK_FALSE(result.ok);
    CHECK(codes(result.diagnostics) == "ProfileNotCarriedx1");
    CHECK(document.models[0].profileSets.size() == 1u);
}

// ============================================================================
// DeriveProfile — kind conversion
// ============================================================================

TEST_CASE("wem a degenerate composite flattens losslessly and silently", "[wem][derive]") {
    // SC2 (Composite) -> Diablo3, which takes Combiners or LegacyDeferred. The
    // stack is one Set layer per channel, so it *is* a slot map already: §7.2.3's
    // named refinement, and it should cost nothing and say nothing.
    Document document = makeDocument(ProfileId::Sc2);
    const DeriveResult result = DeriveProfile(document, ProfileId::Sc2, ProfileId::Diablo3);
    REQUIRE(result.ok);
    CHECK(codes(result.diagnostics) == "");

    const Material* derived = Resolve(document.models[0], 0, ProfileId::Diablo3);
    REQUIRE(derived != nullptr);
    REQUIRE(derived->Common().kind() == MaterialKind::LegacyDeferred);
    const LegacyDeferredBody* body = derived->Common().legacy();
    REQUIRE(body->find(LegacySlot::Diffuse) != nullptr);
    CHECK(body->find(LegacySlot::Diffuse)->texture == 0u);
    REQUIRE(body->find(LegacySlot::Normal) != nullptr);
    CHECK(body->find(LegacySlot::Normal)->texture == 9u);
    CHECK(document.carries(ProfileId::Diablo3));
}

TEST_CASE("wem a folding composite reports what the slot map cannot hold", "[wem][derive]") {
    // Two Color layers is a fold, so `Flatten` refuses. Diablo3 takes Combiners
    // *and* LegacyDeferred, and the stack also builds a normal — which a chain
    // with one register and no channels would lose entirely — so the slot map
    // wins and the fold is what gets reported.
    Document document = documentWith(ProfileId::Sc2, {makeComposite("m", 1)});
    const DeriveResult result = DeriveProfile(document, ProfileId::Sc2, ProfileId::Diablo3);
    REQUIRE(result.ok);
    // The kind note plus "flattened to one layer per channel".
    CHECK(result.diagnostics.countOf(DiagCode::LossyKindConversion) == 2u);
    CHECK(result.diagnostics.countOf(DiagCode::LayerDropped) == 1u);

    const LegacyDeferredBody* body =
        Resolve(document.models[0], 0, ProfileId::Diablo3)->Common().legacy();
    REQUIRE(body != nullptr);
    REQUIRE(body->find(LegacySlot::Diffuse) != nullptr);
    CHECK(body->find(LegacySlot::Diffuse)->texture == 0u); // the first, not the last
    REQUIRE(body->find(LegacySlot::Normal) != nullptr);
    CHECK(body->find(LegacySlot::Normal)->texture == 9u);
}

TEST_CASE("wem a single-channel fold stays ordered", "[wem][derive]") {
    // Same target, same refusal from `Flatten` — but every layer builds the
    // colour, so a chain keeps the whole fold and a slot map would keep one
    // layer of it.
    Material colorOnly;
    colorOnly.name = "m";
    CompositeBody stack;
    CompositeLayer base;
    base.input = makeInput(0);
    stack.layers.push_back(base);
    CompositeLayer over;
    over.input = makeInput(1);
    over.op = CompositeOp::Modulate2x;
    stack.layers.push_back(over);
    colorOnly.InitCommon().body = std::move(stack);

    Document document = documentWith(ProfileId::Sc2, {std::move(colorOnly)});
    const DeriveResult result = DeriveProfile(document, ProfileId::Sc2, ProfileId::Diablo3);
    REQUIRE(result.ok);
    CHECK(result.diagnostics.countOf(DiagCode::LayerDropped) == 0u);

    const CombinersBody* body =
        Resolve(document.models[0], 0, ProfileId::Diablo3)->Common().combiners();
    REQUIRE(body != nullptr);
    REQUIRE(body->stages.size() == 2u);
    CHECK(body->stages[0].rgb == CombinerOp::Opaque);
    CHECK(body->stages[1].rgb == CombinerOp::Mod2x);
}

TEST_CASE("wem composite becomes pbr when that is all the target takes", "[wem][derive]") {
    Document document = makeDocument(ProfileId::Sc2);
    const DeriveResult result = DeriveProfile(document, ProfileId::Sc2, ProfileId::Wc3Reforged);
    REQUIRE(result.ok);
    // One kind-change note per material, and nothing dropped: both channels have
    // a PBR slot.
    CHECK(result.diagnostics.countOf(DiagCode::LossyKindConversion) == 2u);
    CHECK(result.diagnostics.countOf(DiagCode::LayerDropped) == 0u);

    const Material* derived = Resolve(document.models[0], 0, ProfileId::Wc3Reforged);
    REQUIRE(derived->Common().kind() == MaterialKind::PBRDeferred);
    REQUIRE(derived->Common().pbr()->find(PbrSlot::BaseColor) != nullptr);
    CHECK(derived->Common().pbr()->find(PbrSlot::BaseColor)->texture == 0u);
    CHECK(derived->Common().pbr()->find(PbrSlot::Normal)->texture == 9u);
}

TEST_CASE("wem combiner stages become an ordered stack", "[wem][derive]") {
    Document document = documentWith(ProfileId::Wow, {makeCombiners("m", 4, CombinerOp::Mod2x)});
    const DeriveResult result = DeriveProfile(document, ProfileId::Wow, ProfileId::Sc2);
    REQUIRE(result.ok);

    const CompositeBody* body =
        Resolve(document.models[0], 0, ProfileId::Sc2)->Common().composite();
    REQUIRE(body != nullptr);
    REQUIRE(body->layers.size() == 2u);
    // Stage 0 seeds the register, which is what `Set` means on a channel.
    CHECK(body->layers[0].op == CompositeOp::Set);
    CHECK(body->layers[0].input.texture == 4u);
    CHECK(body->layers[1].op == CompositeOp::Modulate2x);
    CHECK(body->layers[1].target == SurfaceChannel::Color);
}

TEST_CASE("wem an op with no equivalent is approximated and named", "[wem][derive]") {
    Document document = documentWith(ProfileId::Wow, {makeCombiners("m", 0, CombinerOp::Decal)});
    const DeriveResult result = DeriveProfile(document, ProfileId::Wow, ProfileId::Sc2);
    REQUIRE(result.ok);
    // The kind note plus the op note — two different losses, both counted.
    CHECK(result.diagnostics.countOf(DiagCode::LossyKindConversion) == 2u);
    CHECK(Resolve(document.models[0], 0, ProfileId::Sc2)->Common().composite()->layers[1].op ==
          CompositeOp::AlphaBlend);
}

TEST_CASE("wem a pbr-only slot has nowhere to go in a classic stack", "[wem][derive]") {
    Document document = documentWith(ProfileId::Wc3Reforged, {makePbr("hd", true)});
    const DeriveResult result =
        DeriveProfile(document, ProfileId::Wc3Reforged, ProfileId::Wc3Classic);
    REQUIRE(result.ok);
    CHECK(result.diagnostics.countOf(DiagCode::LayerDropped) == 1u); // TeamColorMask

    const CompositeBody* body =
        Resolve(document.models[0], 0, ProfileId::Wc3Classic)->Common().composite();
    REQUIRE(body != nullptr);
    REQUIRE(body->layers.size() == 2u);
    CHECK(body->layers[0].target == SurfaceChannel::Color);
    CHECK(body->layers[1].target == SurfaceChannel::Normal);
}

// ============================================================================
// DeriveProfile — the other axes
// ============================================================================

TEST_CASE("wem a shared native kind is carried, a foreign one is dropped", "[wem][derive]") {
    SECTION("the WC3 pair shares MdxMaterial") {
        Document document = documentWith(ProfileId::Wc3Reforged, {makePbr("hd", false)});
        document.models[0].profileSets[0].materials[0].SetNativeAuthoritative(
            native::MdxMaterial{});

        const DeriveResult result =
            DeriveProfile(document, ProfileId::Wc3Reforged, ProfileId::Wc3Classic);
        REQUIRE(result.ok);
        CHECK(result.diagnostics.countOf(DiagCode::DroppedNativeBlock) == 0u);
        const Material* derived = Resolve(document.models[0], 0, ProfileId::Wc3Classic);
        CHECK(derived->nativeKind() == NativeKind::Mdx);
    }

    SECTION("and asking for the expensive path drops it anyway") {
        Document document = documentWith(ProfileId::Wc3Reforged, {makePbr("hd", false)});
        document.models[0].profileSets[0].materials[0].SetNativeAuthoritative(
            native::MdxMaterial{});

        RetargetOptions options;
        options.keepSharedNative = false;
        const DeriveResult result =
            DeriveProfile(document, ProfileId::Wc3Reforged, ProfileId::Wc3Classic, options);
        REQUIRE(result.ok);
        CHECK(result.diagnostics.countOf(DiagCode::DroppedNativeBlock) == 1u);
        CHECK(Resolve(document.models[0], 0, ProfileId::Wc3Classic)->hasNative() == false);
    }

    SECTION("an M3 block has no place in a WC3 set") {
        Document document = makeDocument(ProfileId::Sc2);
        document.models[0].profileSets[0].materials[0].SetNativeAuthoritative(native::M3Material{});

        const DeriveResult result = DeriveProfile(document, ProfileId::Sc2, ProfileId::Wc3Classic);
        REQUIRE(result.ok);
        CHECK(result.diagnostics.countOf(DiagCode::DroppedNativeBlock) == 1u);
        CHECK(Resolve(document.models[0], 0, ProfileId::Wc3Classic)->hasNative() == false);
        // And the result is structurally valid, which the mismatch rule would
        // have caught had the block been carried.
        CHECK(errorCodes(Validate(document, ValidateLevel::Structural)) == "");
    }
}

TEST_CASE("wem a blend mode the target cannot write moves to its nearest", "[wem][derive]") {
    Document document = documentWith(ProfileId::Wow, {makeCombiners("m", 0, CombinerOp::Mod)});
    // M2's BlendAdd has no M3 equivalent.
    document.models[0].profileSets[0].materials[0].InitCommon().blend = BlendMode::BlendAdd;

    const DeriveResult result = DeriveProfile(document, ProfileId::Wow, ProfileId::Sc2);
    REQUIRE(result.ok);
    CHECK(result.diagnostics.countOf(DiagCode::LossyBlendMode) == 1u);
    CHECK(Resolve(document.models[0], 0, ProfileId::Sc2)->Common().blend == BlendMode::Additive);
}

TEST_CASE("wem a look table collapses to one, and says which survived", "[wem][derive]") {
    Document document = documentWith(ProfileId::Wow, {makeCombiners("a", 0, CombinerOp::Mod),
                                                      makeCombiners("b", 2, CombinerOp::Mod),
                                                      makeCombiners("c", 4, CombinerOp::Mod)});
    ProfileMaterialSet& wow = document.models[0].profileSets[0];
    wow.looks.looks[0].name = "A";
    wow.looks.add("B");
    wow.looks.add("C");
    wow.slotBindings.resize(2);
    wow.slotBindings[0].byLook = {0, 1, 2};
    wow.slotBindings[1].byLook = {2, 1, 0};

    RetargetOptions options;
    options.keepLook = 1;
    const DeriveResult result = DeriveProfile(document, ProfileId::Wow, ProfileId::Sc2, options);
    REQUIRE(result.ok);
    CHECK(result.diagnostics.countOf(DiagCode::LookDropped) == 2u);

    const ProfileMaterialSet* sc2 = document.models[0].setFor(ProfileId::Sc2);
    REQUIRE(sc2->looks.size() == 1u);
    CHECK(sc2->looks.looks[0].name == "B");
    REQUIRE(sc2->slotBindings.size() == 2u);
    CHECK(sc2->slotBindings[0].byLook == std::vector<u32>{1});
    CHECK(sc2->slotBindings[1].byLook == std::vector<u32>{1});
}

TEST_CASE("wem derive reports geometry limits and changes nothing", "[wem][derive]") {
    Document document = makeDocument(ProfileId::Sc2);
    Mesh& mesh = document.models[0].meshes[0];
    for (u32 i = 0; i < 3; ++i) {
        mesh.attributes.create(geom::names::uv(i), geom::Domain::Halfedge, geom::AttrType::F32x2);
    }
    const std::size_t layersBefore = mesh.attributes.layerCount();

    // Classic WC3 has one UV set. The geometry is shared with the SC2 set, so a
    // material-set derive does not get to change it (§6.3 rule 3).
    const DeriveResult result = DeriveProfile(document, ProfileId::Sc2, ProfileId::Wc3Classic);
    REQUIRE(result.ok);
    CHECK(result.diagnostics.countOf(DiagCode::UvSetLimit) == 1u);
    CHECK(document.models[0].meshes[0].attributes.layerCount() == layersBefore);
}

TEST_CASE("wem derive ignores rescale and says so", "[wem][derive]") {
    Document document = makeDocument(ProfileId::Sc2);
    RetargetOptions options;
    options.rescale = true;
    const DeriveResult result =
        DeriveProfile(document, ProfileId::Sc2, ProfileId::Wc3Classic, options);
    REQUIRE(result.ok);
    CHECK(result.diagnostics.countOf(DiagCode::OperationUnsupported) == 1u);
}

TEST_CASE("wem deriving twice refreshes rather than accumulates", "[wem][derive]") {
    Document document = makeDocument(ProfileId::Sc2);
    REQUIRE(DeriveProfile(document, ProfileId::Sc2, ProfileId::Diablo3).ok);
    REQUIRE(DeriveProfile(document, ProfileId::Sc2, ProfileId::Diablo3).ok);
    CHECK(document.models[0].profileSets.size() == 2u);
    CHECK(document.profiles.size() == 2u);
}

// ============================================================================
// AddProfileFromImport
// ============================================================================

namespace {

/// A second file over the same geometry: matching meshes and section names, its
/// own slot *order*, and its own texture table.
Document makeImport() {
    Document imported;
    imported.declare(ProfileId::Wow);
    imported.defaultProfile = ProfileId::Wow;

    Model model;
    model.name = "model";
    model.materialSlots = {"trim", "body"}; // deliberately the other way round
    model.meshes.push_back(makeMesh({"body", "trim"}));
    for (MeshSection& section : model.meshes[0].sections) {
        section.profiles = ProfileBit(ProfileId::Wow);
    }
    model.profileSets.push_back(
        makeSet(ProfileId::Wow, {makeCombiners("trimMat", 0, CombinerOp::Mod),
                                 makeCombiners("bodyMat", 1, CombinerOp::Mod)}));
    imported.models.push_back(std::move(model));

    TextureRef shared;
    shared.key = TexturePath{"tex3.dds"};
    shared.path = "tex3.dds";
    TextureRef own;
    own.key = TexturePath{"wow_only.dds"};
    own.path = "wow_only.dds";
    TextureRef second;
    second.key = TexturePath{"wow_only2.dds"};
    second.path = "wow_only2.dds";
    imported.textures = {shared, own, second};
    return imported;
}

} // namespace

TEST_CASE("wem import joins slots by name and remaps the texture table", "[wem][derive][import]") {
    Document document = makeDocument(ProfileId::Sc2);
    const Document imported = makeImport();

    const DeriveResult result = AddProfileFromImport(document, ProfileId::Wow, imported);
    REQUIRE(result.ok);
    CHECK(codes(result.diagnostics) == "");
    CHECK(document.carries(ProfileId::Wow));

    // One of the three was already in the table; the other two were appended.
    REQUIRE(document.textures.size() == 12u);
    CHECK(document.textures[10].path == "wow_only.dds");
    CHECK(document.textures[11].path == "wow_only2.dds");

    // The two files numbered their slots differently, so the join is on the name.
    const Model& model = document.models[0];
    REQUIRE(Resolve(model, 0, ProfileId::Wow) != nullptr);
    CHECK(Resolve(model, 0, ProfileId::Wow)->name == "bodyMat");
    CHECK(Resolve(model, 1, ProfileId::Wow)->name == "trimMat");

    // …and each material's texture indices now address the merged table.
    CHECK(Resolve(model, 0, ProfileId::Wow)->Common().inputAt(0)->texture == 10u);
    CHECK(Resolve(model, 1, ProfileId::Wow)->Common().inputAt(0)->texture == 3u);

    // The SC2 set is untouched, and both profiles now draw both sections.
    CHECK(Resolve(model, 0, ProfileId::Sc2)->name == "bodyMat");
    CHECK(model.drawnProfiles() == (ProfileBit(ProfileId::Sc2) | ProfileBit(ProfileId::Wow)));
    CHECK(errorCodes(Validate(document, ValidateLevel::Profile)) == "");
}

TEST_CASE("wem import refuses geometry that does not match", "[wem][derive][import]") {
    SECTION("a different section name") {
        Document document = makeDocument(ProfileId::Sc2);
        Document imported = makeImport();
        imported.models[0].meshes[0].sections[1].name = "cape";

        const DeriveResult result = AddProfileFromImport(document, ProfileId::Wow, imported);
        CHECK_FALSE(result.ok);
        CHECK(codes(result.diagnostics) == "GeometryMismatchx1");
        CHECK_FALSE(document.carries(ProfileId::Wow));
        CHECK(document.models[0].profileSets.size() == 1u);
    }

    SECTION("a different vertex count") {
        Document document = makeDocument(ProfileId::Sc2);
        Document imported = makeImport();
        imported.models[0].meshes[0] = makeMesh({"body", "trim", "extra"});

        const DeriveResult result = AddProfileFromImport(document, ProfileId::Wow, imported);
        CHECK_FALSE(result.ok);
        CHECK(codes(result.diagnostics) == "GeometryMismatchx1");
    }

    SECTION("a different model count") {
        Document document = makeDocument(ProfileId::Sc2);
        Document imported = makeImport();
        imported.models.push_back(imported.models[0]);

        const DeriveResult result = AddProfileFromImport(document, ProfileId::Wow, imported);
        CHECK_FALSE(result.ok);
        CHECK(codes(result.diagnostics) == "GeometryMismatchx1");
    }
}

// ============================================================================
// DeriveProfile — the material channels (§10.8)
// ============================================================================

TEST_CASE("wem a derive gives the new set channels of its own", "[wem][derive][anim]") {
    // A channel names its material by `(profile, slot, look)`, so a derived set
    // starts with none: every channel in the table still names the source. That
    // is the whole material animation gone — WoW hides a conditional batch by
    // keying its `M2Color` alpha to zero, and the derived set had nothing to
    // hide it with.
    Document document = documentWith(ProfileId::Sc2, {makeComposite("m")});

    AnimChannel channel;
    channel.id = 7;
    channel.target.kind = TrackTarget::Kind::MaterialLayer;
    channel.target.material.profile = ProfileId::Sc2;
    channel.target.material.slot = 0;
    channel.target.sub = kWholeMaterial;
    channel.target.channel = Channel::Alpha;
    channel.valueType = geom::AttrType::F32;
    document.models[0].animChannels.add(channel);

    Clip clip;
    clip.name = "stand";
    clip.model = 0;
    clip.duration = 1.0f;
    SubTrackContainer container;
    SubTrack track;
    track.channel = 7;
    track.times = {0.0f};
    track.values.assign(sizeof(f32), 0);
    container.subTracks.push_back(std::move(track));
    clip.containers.push_back(std::move(container));
    document.clips.push_back(std::move(clip));

    REQUIRE(DeriveProfile(document, ProfileId::Sc2, ProfileId::Diablo3).ok);

    const AnimChannelTable& table = document.models[0].animChannels;
    REQUIRE(table.channels.size() == 2u);
    const AnimChannel* twin = nullptr;
    for (const AnimChannel& entry : table.channels) {
        if (entry.target.material.profile == ProfileId::Diablo3) {
            twin = &entry;
        }
    }
    REQUIRE(twin != nullptr);
    CHECK(twin->id != 7u); // a new id: the source channel is still here
    CHECK(twin->target.sub == kWholeMaterial);
    CHECK(twin->target.channel == Channel::Alpha);
    // The curve came with it. A channel is a join key; the keys live in the clip.
    REQUIRE(document.clips[0].containers[0].subTracks.size() == 2u);
    CHECK(document.clips[0].containers[0].find(twin->id) != nullptr);

    // And deriving again is a refresh, not an accumulation — the same rule the
    // set itself follows.
    REQUIRE(DeriveProfile(document, ProfileId::Sc2, ProfileId::Diablo3).ok);
    CHECK(document.models[0].animChannels.channels.size() == 2u);
    CHECK(document.clips[0].containers[0].subTracks.size() == 2u);
}
