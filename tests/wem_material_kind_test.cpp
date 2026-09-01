// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P3b — the common material. The four kinds, the features that cut
/// across them, and the sync policy that keeps a native block from being written
/// stale. The sync cases are the load-bearing ones: §7.1 exists to make one
/// specific bug unreachable, so the test is that it *is* unreachable, not that
/// the enum has four values.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/materials/common.h>
#include <whiteout/models/wem/materials/material.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

TextureInput input(u32 texture) {
    TextureInput value;
    value.texture = texture;
    return value;
}

CompositeLayer layer(SurfaceChannel channel, CompositeOp op, u32 texture) {
    CompositeLayer out;
    out.input = input(texture);
    out.target = channel;
    out.op = op;
    return out;
}

native::M3Material m3Block(u32 version) {
    native::M3Material block;
    block.sourceVersion = version;
    return block;
}

} // namespace

// ============================================================================
// The four kinds
// ============================================================================

TEST_CASE("wem material kind is the body, not a tag", "[wem][materials]") {
    CommonMaterial material;
    CHECK(material.kind() == MaterialKind::Composite);
    CHECK(material.ordinalCount() == 0);

    material.setKind(MaterialKind::Combiners);
    CHECK(material.kind() == MaterialKind::Combiners);
    CHECK(material.combiners() != nullptr);
    CHECK(material.composite() == nullptr);

    // The redundancy `NodeKind`/`NodePayload` has, and for the same reason: the
    // disk format keys on the number, so it has to be the same number.
    CHECK(BodyIndexFor(MaterialKind::Composite) == 0u);
    CHECK(BodyIndexFor(material.kind()) == material.body.index());

    material.setKind(MaterialKind::LegacyDeferred);
    material.legacy()->set(LegacySlot::Diffuse, input(3));
    material.legacy()->set(LegacySlot::Normal, input(4));
    CHECK(material.ordinalCount() == 2);
    REQUIRE(material.inputAt(1) != nullptr);
    CHECK(material.inputAt(1)->texture == 4u);
    CHECK(material.inputAt(2) == nullptr);

    // `set` is an upsert: a slot appears at most once, which is the invariant
    // `Validate` enforces on data that arrives from elsewhere.
    material.legacy()->set(LegacySlot::Diffuse, input(7));
    CHECK(material.ordinalCount() == 2);
    REQUIRE(material.legacy()->find(LegacySlot::Diffuse) != nullptr);
    CHECK(material.legacy()->find(LegacySlot::Diffuse)->texture == 7u);
    CHECK(material.legacy()->find(LegacySlot::Gloss) == nullptr);
}

TEST_CASE("wem composite layers keep their order and their channels", "[wem][materials]") {
    CompositeBody body;
    body.layers.push_back(layer(SurfaceChannel::Color, CompositeOp::Set, 0));
    body.layers.push_back(layer(SurfaceChannel::Normal, CompositeOp::Set, 1));
    body.layers.push_back(layer(SurfaceChannel::Color, CompositeOp::Modulate2x, 2));

    const std::vector<u32> colorLayers = body.layersOf(SurfaceChannel::Color);
    REQUIRE(colorLayers.size() == 2);
    CHECK(colorLayers[0] == 0u);
    CHECK(colorLayers[1] == 2u);
    CHECK(body.layersOf(SurfaceChannel::Specular).empty());
}

// ============================================================================
// Flatten — the one named kind refinement (§7.2.3)
// ============================================================================

TEST_CASE("wem flatten accepts a degenerate composite and refuses a fold", "[wem][materials]") {
    CompositeBody degenerate;
    degenerate.layers.push_back(layer(SurfaceChannel::Color, CompositeOp::Set, 0));
    degenerate.layers.push_back(layer(SurfaceChannel::Normal, CompositeOp::Set, 1));
    degenerate.layers.push_back(layer(SurfaceChannel::Environment, CompositeOp::Set, 2));
    degenerate.specularExponent = 12.5f;
    degenerate.environmentFactor = 0.25f;

    const std::optional<LegacyDeferredBody> flat = Flatten(degenerate);
    REQUIRE(flat.has_value());
    REQUIRE(flat->find(LegacySlot::Diffuse) != nullptr);
    CHECK(flat->find(LegacySlot::Diffuse)->texture == 0u);
    REQUIRE(flat->find(LegacySlot::Normal) != nullptr);
    CHECK(flat->find(LegacySlot::Normal)->texture == 1u);
    REQUIRE(flat->find(LegacySlot::Environment) != nullptr);
    CHECK(flat->find(LegacySlot::Environment)->texture == 2u);
    CHECK(flat->specularExponent == 12.5f);
    CHECK(flat->environmentFactor == 0.25f);
    // The one-directional half: no `SurfaceChannel` feeds these.
    CHECK(flat->find(LegacySlot::Gloss) == nullptr);
    CHECK(flat->find(LegacySlot::Height) == nullptr);

    // Two layers on one channel is a fold, and a slot map cannot hold one.
    CompositeBody twoOnColor = degenerate;
    twoOnColor.layers.push_back(layer(SurfaceChannel::Color, CompositeOp::Modulate, 3));
    CHECK_FALSE(Flatten(twoOnColor).has_value());

    // So is one layer whose op is not a replace, even alone on its channel.
    CompositeBody blended;
    blended.layers.push_back(layer(SurfaceChannel::Color, CompositeOp::AlphaBlend, 0));
    CHECK_FALSE(Flatten(blended).has_value());

    // An empty stack is degenerate, and flattens to a factors-only body.
    const std::optional<LegacyDeferredBody> empty = Flatten(CompositeBody{});
    REQUIRE(empty.has_value());
    CHECK(empty->slots.empty());
}

// ============================================================================
// Colour space by slot (§7.2, and the measured .m3 lesson)
// ============================================================================

TEST_CASE("wem auto colour space comes from the slot, never the filename", "[wem][materials]") {
    CHECK(AutoColorSpaceFor(SurfaceChannel::Color) == ColorSpace::Srgb);
    CHECK(AutoColorSpaceFor(SurfaceChannel::Normal) == ColorSpace::Linear);
    CHECK(AutoColorSpaceFor(LegacySlot::Diffuse) == ColorSpace::Srgb);
    CHECK(AutoColorSpaceFor(LegacySlot::Gloss) == ColorSpace::Linear);
    CHECK(AutoColorSpaceFor(PbrSlot::BaseColor) == ColorSpace::Srgb);
    CHECK(AutoColorSpaceFor(PbrSlot::Orm) == ColorSpace::Linear);
    // A mask selects how much tint lands; tone-mapping it would bend the ramp.
    CHECK(AutoColorSpaceFor(PbrSlot::TeamColorMask) == ColorSpace::Linear);

    // An explicit declaration always wins — that is the whole point of keeping
    // the field rather than guessing.
    TextureInput normalAsSrgb = input(0);
    normalAsSrgb.colorSpace = ColorSpace::Srgb;
    CHECK(ResolvedColorSpace(normalAsSrgb, SurfaceChannel::Normal) == ColorSpace::Srgb);
    CHECK(ResolvedColorSpace(input(0), SurfaceChannel::Normal) == ColorSpace::Linear);
}

TEST_CASE("wem uv transform is affine, not a TRS", "[wem][materials]") {
    Matrix3x2f identity;
    CHECK(identity.isIdentity());
    const Vector2f uv{0.25f, 0.75f};
    CHECK(identity.apply(uv).x == uv.x);
    CHECK(identity.apply(uv).y == uv.y);

    // A shear: the case a scale/rotate/translate triple cannot express, and the
    // reason M3's and D3's matrices are stored as matrices.
    Matrix3x2f shear;
    shear.m[0][1] = 2.0f;
    CHECK_FALSE(shear.isIdentity());
    CHECK(shear.apply(uv).x == 0.25f + 1.5f);
    CHECK(shear.apply(uv).y == 0.75f);
}

// ============================================================================
// Features (§7.2.5)
// ============================================================================

TEST_CASE("wem features are per material and per layer", "[wem][materials]") {
    CommonMaterial material;
    material.composite()->layers.push_back(layer(SurfaceChannel::Color, CompositeOp::Set, 0));
    material.composite()->layers.push_back(layer(SurfaceChannel::Color, CompositeOp::Add, 1));

    MaterialFeature fresnel;
    fresnel.id = NextFeatureId(material.features);
    fresnel.payload = FresnelFeature{};
    fresnel.fresnel()->outMax = 0.6f;
    material.features.push_back(fresnel);

    MaterialFeature scroll;
    scroll.id = NextFeatureId(material.features);
    scroll.layer = 1;
    scroll.payload = UvAnimationFeature{};
    scroll.uvAnimation()->scrollRate = Vector2f{0.1f, 0};
    material.features.push_back(scroll);

    CHECK(fresnel.id == 0u);
    CHECK(scroll.id == 1u);
    CHECK(NextFeatureId(material.features) == 2u);

    REQUIRE(material.feature(FeatureKind::Fresnel) != nullptr);
    CHECK(material.feature(FeatureKind::Fresnel)->isWholeMaterial());
    CHECK(material.feature(FeatureKind::Fresnel)->fresnel()->outMax == 0.6f);

    // A layer feature is not the whole-material one, and vice versa: the lookup
    // is on `(kind, layer)` because that pair is what `Validate` makes unique.
    CHECK(material.feature(FeatureKind::UvAnimation) == nullptr);
    REQUIRE(material.feature(FeatureKind::UvAnimation, 1) != nullptr);
    CHECK(material.feature(FeatureKind::UvAnimation, 1)->uvAnimation()->isConstantRate());
    CHECK(material.feature(FeatureKind::UvAnimation, 0) == nullptr);

    CHECK(FeaturePayloadIndexFor(FeatureKind::UvAnimation) == scroll.payload.index());
}

// ============================================================================
// The sync policy (§7.1) — the bug this design exists to prevent
// ============================================================================

TEST_CASE("wem material sync starts absent and stays absent without a block", "[wem][materials]") {
    Material material;
    CHECK(material.sync() == NativeSync::Absent);
    CHECK_FALSE(material.hasNative());
    CHECK(material.nativeKind() == NativeKind::None);
    CHECK_FALSE(material.NativeIsAuthoritative());
    CHECK_FALSE(material.NeedsNativeReDerive());

    // Editing a material with no native block cannot make one stale, so the state
    // does not move.
    material.MutableCommon().blend = BlendMode::Additive;
    CHECK(material.sync() == NativeSync::Absent);
    CHECK(material.Common().blend == BlendMode::Additive);
}

TEST_CASE("wem mutable common makes both native states stale", "[wem][materials]") {
    SECTION("from InSync") {
        Material material;
        material.SetNativeInSync(m3Block(30));
        CHECK(material.sync() == NativeSync::InSync);
        CHECK(material.NativeIsAuthoritative());

        material.MutableCommon().alphaTestThreshold = 0.5f;
        CHECK(material.sync() == NativeSync::CommonEdited);
        CHECK_FALSE(material.NativeIsAuthoritative());
        CHECK(material.NeedsNativeReDerive());
    }

    SECTION("and from NativeAuthoritative — the mirror-image bug") {
        Material material;
        material.SetNativeAuthoritative(m3Block(23));
        CHECK(material.sync() == NativeSync::NativeAuthoritative);
        CHECK(material.NativeIsAuthoritative());

        // An edit to a lossy view still makes the view the newest truth. Leaving
        // the block authoritative here is the same silent-staleness bug, mirrored.
        material.MutableCommon().cull = CullMode::None;
        CHECK(material.sync() == NativeSync::CommonEdited);
        CHECK_FALSE(material.NativeIsAuthoritative());
    }

    SECTION("but reading does not") {
        Material material;
        material.SetNativeInSync(m3Block(30));
        CHECK(material.Common().blend == BlendMode::Opaque);
        CHECK(material.sync() == NativeSync::InSync);
    }

    SECTION("and neither does the importer's own write") {
        Material material;
        material.SetNativeAuthoritative(m3Block(30));
        // InitCommon is the "I am deriving common FROM native" call — the one
        // edit that does not make the block stale.
        material.InitCommon().blend = BlendMode::AlphaBlend;
        CHECK(material.sync() == NativeSync::NativeAuthoritative);
    }
}

TEST_CASE("wem re-syncing and clearing a native block", "[wem][materials]") {
    Material material;
    material.SetNativeAuthoritative(m3Block(30));
    material.MutableCommon().priorityPlane = 3;
    REQUIRE(material.NeedsNativeReDerive());

    // The exporter's escape: re-derive, then say so. After that the block is not
    // stale any more and nothing is written behind the user's back.
    material.SetNativeInSync(m3Block(30));
    CHECK(material.sync() == NativeSync::InSync);
    CHECK_FALSE(material.NeedsNativeReDerive());
    CHECK(material.Common().priorityPlane == 3);

    material.ClearNative();
    CHECK(material.sync() == NativeSync::Absent);
    CHECK(material.nativeKind() == NativeKind::None);
    CHECK(material.Common().priorityPlane == 3);

    // Handing a monostate to either setter is "no block", not "a block of kind
    // none" — the two would otherwise disagree with `hasNative`.
    material.SetNativeInSync(NativeMaterial{});
    CHECK(material.sync() == NativeSync::Absent);
}

TEST_CASE("wem native kind and the variant index are the same number", "[wem][materials]") {
    CHECK(NativeKindOf(NativeMaterial{}) == NativeKind::None);
    CHECK(NativeKindOf(native::MdxMaterial{}) == NativeKind::Mdx);
    CHECK(NativeKindOf(native::M2Material{}) == NativeKind::M2);
    CHECK(NativeKindOf(native::M3Material{}) == NativeKind::M3);
    CHECK(NativeKindOf(native::D3Material{}) == NativeKind::D3);
    CHECK(NativeIndexFor(NativeKind::D3) == NativeMaterial(native::D3Material{}).index());
}
