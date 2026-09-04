// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P3c — the WC3 blend algebra.
///
/// The §7.2.1 split (the first layer's filter mode is the header, not an op) and
/// the §7.2.2 collapse (N modulate/add passes are one combiner chain) are the two
/// claims the MDX material core rests on, and both are statements about *blending*
/// that a corpus sweep cannot check: a sweep proves the fields parsed, never that
/// the reading of them is right. So they are asserted here on hand-built stacks,
/// one per case, including the two the design explicitly refuses.

#include <catch2/catch_test_macros.hpp>

#include "whiteout/models/wem/materials/mdx_core.h"

using namespace whiteout;
using namespace whiteout::models::wem;
using mdx::Layer;

namespace {

Layer makeLayer(Layer::FilterMode mode, u32 textureId = 0, bool hd = false) {
    Layer layer;
    layer.filterMode = mode;
    layer.textureId = textureId;
    layer.is_hd = hd;
    return layer;
}

mdx::Material makeMaterial(std::initializer_list<Layer::FilterMode> modes) {
    mdx::Material material;
    u32 texture = 0;
    for (Layer::FilterMode mode : modes) {
        material.layers.push_back(makeLayer(mode, texture++));
    }
    return material;
}

mdx_core::Context makeContext(u32 textureCount = 8, u32 version = 800) {
    mdx_core::Context context;
    context.modelVersion = version;
    for (u32 i = 0; i < textureCount; ++i) {
        context.textureIndexMap.push_back(i);
    }
    return context;
}

/// The eligibility predicate takes pointers, because it is also how the importer
/// asks after filtering by profile.
bool collapses(const mdx::Material& material) {
    std::vector<const Layer*> layers;
    for (const Layer& layer : material.layers) {
        layers.push_back(&layer);
    }
    return mdx_core::StackCollapses(layers);
}

} // namespace

// ============================================================================
// §7.2.1 — the first layer is the header, not an op
// ============================================================================

TEST_CASE("wem the first layer's filter mode becomes the header blend", "[wem][materials][mdx]") {
    Diagnostics diagnostics;
    const mdx_core::Context context = makeContext();

    struct Case {
        Layer::FilterMode mode;
        BlendMode blend;
    };
    const Case cases[] = {
        {Layer::FilterMode::None, BlendMode::Opaque},
        {Layer::FilterMode::Transparent, BlendMode::Transparent},
        {Layer::FilterMode::Blend, BlendMode::AlphaBlend},
        {Layer::FilterMode::Additive, BlendMode::Additive},
        {Layer::FilterMode::AddAlpha, BlendMode::AdditiveAlpha},
        {Layer::FilterMode::Modulate, BlendMode::Modulate},
        {Layer::FilterMode::Modulate2x, BlendMode::Modulate2x},
    };

    for (const Case& c : cases) {
        const mdx::Material source = makeMaterial({c.mode});
        const Material imported =
            mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, context, diagnostics);
        CHECK(imported.Common().blend == c.blend);

        // ...and the layer itself is `Set`. That is the whole point of the split:
        // a one-layer stack composites onto nothing.
        const CompositeBody* body = imported.Common().composite();
        REQUIRE(body != nullptr);
        REQUIRE(body->layers.size() == 1);
        CHECK(body->layers[0].op == CompositeOp::Set);
    }
}

TEST_CASE("wem later filter modes become composite ops", "[wem][materials][mdx]") {
    Diagnostics diagnostics;
    // A `Blend` first layer keeps the stack out of the collapse path, so the ops
    // survive as ops rather than becoming a combiner chain.
    const mdx::Material source =
        makeMaterial({Layer::FilterMode::Blend, Layer::FilterMode::Modulate,
                      Layer::FilterMode::Additive, Layer::FilterMode::AddAlpha});
    const Material imported =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);

    const CompositeBody* body = imported.Common().composite();
    REQUIRE(body != nullptr);
    REQUIRE(body->layers.size() == 4);
    CHECK(body->layers[0].op == CompositeOp::Set);
    CHECK(body->layers[1].op == CompositeOp::Modulate);
    CHECK(body->layers[2].op == CompositeOp::Add);
    CHECK(body->layers[3].op == CompositeOp::AddAlpha);
    CHECK(imported.Common().blend == BlendMode::AlphaBlend);
}

TEST_CASE("wem an alpha-keyed header sets the WC3 alpha reference", "[wem][materials][mdx]") {
    Diagnostics diagnostics;
    const mdx::Material source = makeMaterial({Layer::FilterMode::Transparent});
    const Material imported =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);
    CHECK(imported.Common().alphaTestThreshold == mdx_core::kTransparentAlphaRef);
}

// ============================================================================
// §7.2.2 — the collapse, and the two refusals
// ============================================================================

TEST_CASE("wem an opaque-first modulate stack collapses to combiners", "[wem][materials][mdx]") {
    const mdx::Material source =
        makeMaterial({Layer::FilterMode::None, Layer::FilterMode::Modulate,
                      Layer::FilterMode::Modulate2x, Layer::FilterMode::Additive});
    REQUIRE(collapses(source));

    Diagnostics diagnostics;
    const Material imported =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);

    REQUIRE(imported.Common().kind() == MaterialKind::Combiners);
    const CombinersBody* body = imported.Common().combiners();
    REQUIRE(body != nullptr);
    REQUIRE(body->stages.size() == 4);
    CHECK(body->stages[0].rgb == CombinerOp::Opaque);
    CHECK(body->stages[1].rgb == CombinerOp::Mod);
    CHECK(body->stages[2].rgb == CombinerOp::Mod2x);
    CHECK(body->stages[3].rgb == CombinerOp::Add);
}

TEST_CASE("wem an alpha-keyed first layer refuses the collapse", "[wem][materials][mdx]") {
    // The refusal that matters: a `Transparent` first pass leaves the scene in
    // the keyed-out pixels, and the later modulate passes then read the scene.
    // One alpha-tested draw discards those fragments instead — a different
    // picture, not a cheaper one.
    const mdx::Material source =
        makeMaterial({Layer::FilterMode::Transparent, Layer::FilterMode::Modulate});
    CHECK_FALSE(collapses(source));

    Diagnostics diagnostics;
    const Material imported =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);
    CHECK(imported.Common().kind() == MaterialKind::Composite);
    CHECK(diagnostics.countOf(DiagCode::CollapseRefused) == 1);
}

TEST_CASE("wem a non-collapsible later op refuses the collapse", "[wem][materials][mdx]") {
    // `Blend` reads the framebuffer, so pass N does not see pass N-1's output —
    // it sees the blend of it with whatever was already there.
    CHECK_FALSE(collapses(makeMaterial({Layer::FilterMode::None, Layer::FilterMode::Blend})));
    CHECK_FALSE(collapses(makeMaterial({Layer::FilterMode::None, Layer::FilterMode::Transparent})));
    CHECK_FALSE(collapses(makeMaterial(
        {Layer::FilterMode::None, Layer::FilterMode::Modulate, Layer::FilterMode::Blend})));
}

TEST_CASE("wem a single opaque layer is not a collapse", "[wem][materials][mdx]") {
    // Trivially blend-equivalent, and still not a collapse: there is nothing to
    // collapse. `Combiners` is for N passes that became one draw.
    CHECK_FALSE(collapses(makeMaterial({Layer::FilterMode::None})));

    Diagnostics diagnostics;
    const Material imported = mdx_core::ImportMaterial(
        makeMaterial({Layer::FilterMode::None}), ProfileId::Wc3Classic, makeContext(), diagnostics);
    CHECK(imported.Common().kind() == MaterialKind::Composite);
}

// ============================================================================
// The two divergences the design says to report rather than reinterpret
// ============================================================================

TEST_CASE("wem a multi-layer stack over a scene-reading header diagnoses",
          "[wem][materials][mdx]") {
    Diagnostics diagnostics;
    const mdx::Material source =
        makeMaterial({Layer::FilterMode::Blend, Layer::FilterMode::Modulate});
    mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);
    CHECK(diagnostics.countOf(DiagCode::SceneReadingMultiLayerStack) == 1);
}

TEST_CASE("wem an opaque multi-layer stack does not diagnose", "[wem][materials][mdx]") {
    Diagnostics diagnostics;
    const mdx::Material source = makeMaterial({Layer::FilterMode::None, Layer::FilterMode::Blend});
    mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);
    // Not collapsible (Blend), but the fold and the passes still agree, because
    // the first pass replaced every pixel the draw covers.
    CHECK(diagnostics.countOf(DiagCode::SceneReadingMultiLayerStack) == 0);
}

TEST_CASE("wem a stack mixing shaded and unshaded layers diagnoses", "[wem][materials][mdx]") {
    mdx::Material source = makeMaterial({Layer::FilterMode::None, Layer::FilterMode::Modulate});
    source.layers[1].shadingFlags |= Layer::ShadingFlag::Unshaded;

    Diagnostics diagnostics;
    mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);
    CHECK(diagnostics.countOf(DiagCode::MixedShadedUnshadedStack) == 1);
}

// ============================================================================
// The export inverse
// ============================================================================

TEST_CASE("wem exporting a collapsed chain restores the layer stack", "[wem][materials][mdx]") {
    const mdx::Material source = makeMaterial(
        {Layer::FilterMode::None, Layer::FilterMode::Modulate2x, Layer::FilterMode::Additive});
    Diagnostics diagnostics;
    Material imported =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);

    // Force the fallback path: with the native block in sync, export reproduces
    // it and the §7.2.2 inverse is never exercised (§7.1).
    imported.MutableCommon();
    const mdx::Material exported =
        mdx_core::ExportMaterial(imported, ProfileId::Wc3Classic, makeContext(), diagnostics);

    REQUIRE(exported.layers.size() == 3);
    CHECK(exported.layers[0].filterMode == Layer::FilterMode::None);
    CHECK(exported.layers[1].filterMode == Layer::FilterMode::Modulate2x);
    CHECK(exported.layers[2].filterMode == Layer::FilterMode::Additive);
    CHECK(exported.layers[1].textureId == 1);
}

TEST_CASE("wem exporting a composite writes only the layers MDX draws", "[wem][materials][mdx]") {
    // The shape every format but MDX has: one material, one texture per surface
    // channel. An `.mdx` layer is a textured draw of the whole geoset, so only
    // the three channels that come out as colour have anywhere to go.
    Material material;
    material.name = "zergling";
    CompositeBody body;
    const SurfaceChannel channels[] = {SurfaceChannel::Color, SurfaceChannel::Specular,
                                       SurfaceChannel::Emissive, SurfaceChannel::Normal};
    u32 texture = 0;
    for (const SurfaceChannel channel : channels) {
        CompositeLayer layer;
        layer.input.texture = texture++;
        layer.target = channel;
        layer.op = channel == SurfaceChannel::Emissive ? CompositeOp::Add : CompositeOp::Set;
        body.layers.push_back(layer);
    }
    // The glow's strength, which an `.m3` fills from `hdrEmissiveMultiplier`. An
    // MDX layer is a whole pass and carries no strength of its own, so a channel
    // whose factor is zero has nowhere to put "and hardly at all" — see the case
    // below.
    body.emissiveFactor = Vector4f{1, 1, 1, 1};
    material.InitCommon().body = std::move(body);
    material.MutableCommon().blend = BlendMode::Opaque;

    Diagnostics diagnostics;
    const mdx::Material exported =
        mdx_core::ExportMaterial(material, ProfileId::Wc3Classic, makeContext(), diagnostics);

    // Specular and Normal are gone. Writing them drew the whole model twice more
    // in flat colour, the second time in its normal map — which is what a
    // StarCraft II model opened as Warcraft III looked like.
    REQUIRE(exported.layers.size() == 2);
    CHECK(exported.layers[0].textureId == 0);
    CHECK(exported.layers[1].textureId == 2);
    // Emissive keeps its additive pass, which is how Warcraft III spells a glow.
    CHECK(exported.layers[0].filterMode == Layer::FilterMode::None);
    CHECK(exported.layers[1].filterMode == Layer::FilterMode::Additive);
}

TEST_CASE("wem a channel scaled to nothing writes no layer", "[wem][materials][mdx]") {
    // Diablo III leaves `environmentFactor` at zero on every material and lets
    // the gloss map carry the reflection instead. An MDX layer has no strength —
    // it is a whole draw — so writing one anyway added the environment map at
    // full weight, which turned the Skeleton King's gold armour teal.
    Material material;
    material.name = "reflective";
    CompositeBody body;
    CompositeLayer colour;
    colour.input.texture = 0;
    colour.target = SurfaceChannel::Color;
    body.layers.push_back(colour);
    CompositeLayer environment;
    environment.input.texture = 1;
    environment.target = SurfaceChannel::Environment;
    body.layers.push_back(environment);
    body.environmentFactor = 0.0f;
    material.InitCommon().body = std::move(body);
    material.MutableCommon().blend = BlendMode::Opaque;

    Diagnostics diagnostics;
    const mdx::Material exported =
        mdx_core::ExportMaterial(material, ProfileId::Wc3Classic, makeContext(), diagnostics);
    REQUIRE(exported.layers.size() == 1);
    CHECK(exported.layers[0].textureId == 0);

    // Non-zero, and it comes back — scaled into the layer's weight, which is the
    // one knob a pass has for it.
    Material lit = material;
    CompositeBody scaled = *material.Common().composite();
    scaled.environmentFactor = 0.5f;
    lit.MutableCommon().body = std::move(scaled);
    const mdx::Material withEnv =
        mdx_core::ExportMaterial(lit, ProfileId::Wc3Classic, makeContext(), diagnostics);
    REQUIRE(withEnv.layers.size() == 2);
    CHECK(withEnv.layers[1].textureId == 1);
    CHECK(withEnv.layers[1].filterMode == Layer::FilterMode::Additive);
    CHECK(withEnv.layers[1].alpha == 0.5f);
}

TEST_CASE("wem a dropped first layer still leaves an opaque base", "[wem][materials][mdx]") {
    // The base of the stack is the first layer WRITTEN. Reading `i == 0` instead
    // would give the emissive pass the header blend and leave the colour pass
    // additive over nothing.
    Material material;
    material.name = "normal-first";
    CompositeBody body;
    CompositeLayer normal;
    normal.input.texture = 0;
    normal.target = SurfaceChannel::Normal;
    body.layers.push_back(normal);
    CompositeLayer colour;
    colour.input.texture = 1;
    colour.target = SurfaceChannel::Color;
    colour.op = CompositeOp::Modulate;
    body.layers.push_back(colour);
    material.InitCommon().body = std::move(body);
    material.MutableCommon().blend = BlendMode::Opaque;

    Diagnostics diagnostics;
    const mdx::Material exported =
        mdx_core::ExportMaterial(material, ProfileId::Wc3Classic, makeContext(), diagnostics);
    REQUIRE(exported.layers.size() == 1);
    CHECK(exported.layers[0].textureId == 1);
    CHECK(exported.layers[0].filterMode == Layer::FilterMode::None);
}

// ============================================================================
// §7.2.2 the other way — a chain of passes, which is what an `.mdx` layer is
// ============================================================================

namespace {

/// A combiner material with @p ops as its stage colour ops, one texture each.
Material makeChain(std::initializer_list<CombinerOp> ops, BlendMode blend = BlendMode::Opaque) {
    Material material;
    material.name = "chain";
    CombinersBody body;
    u32 texture = 0;
    for (const CombinerOp op : ops) {
        CombinerStage stage;
        stage.input.texture = texture++;
        stage.rgb = op;
        stage.alpha = op;
        body.stages.push_back(stage);
    }
    material.InitCommon().body = std::move(body);
    material.MutableCommon().blend = blend;
    return material;
}

} // namespace

TEST_CASE("wem a chain's base layer takes the material's blend", "[wem][materials][mdx]") {
    // Not an unconditional `None`. A chain seeded by an opaque stage still meets
    // the scene however its header says, and Diablo III's additive wings met it
    // as solid plates for as long as this was hard-coded.
    Diagnostics diagnostics;
    const mdx::Material exported = mdx_core::ExportMaterial(
        makeChain({CombinerOp::Opaque, CombinerOp::Mod}, BlendMode::AdditiveAlpha),
        ProfileId::Wc3Classic, makeContext(), diagnostics);
    REQUIRE(exported.layers.size() == 2);
    CHECK(exported.layers[0].filterMode == Layer::FilterMode::AddAlpha);
    CHECK(exported.layers[1].filterMode == Layer::FilterMode::Modulate);
}

TEST_CASE("wem a mid-chain replace restarts the stack", "[wem][materials][mdx]") {
    // An `.mdx` layer is a PASS. `None` on a later layer is not an identity and
    // not a chain-replace: it is an opaque draw of the whole geoset, and every
    // layer under it stops existing. Which is the right reading of a replace —
    // a stage that replaces its register really does kill the ones before it —
    // but only if the stack is actually restarted. Left in place it is a stack
    // whose LAST opaque layer is the whole material, which is how a Diablo III
    // wing arrived as one flat alpha mask.
    Diagnostics diagnostics;
    const mdx::Material exported = mdx_core::ExportMaterial(
        makeChain({CombinerOp::Opaque, CombinerOp::Mod, CombinerOp::Opaque, CombinerOp::Mod},
                  BlendMode::AlphaBlend),
        ProfileId::Wc3Classic, makeContext(), diagnostics);
    REQUIRE(exported.layers.size() == 2);
    CHECK(exported.layers[0].textureId == 2); // the replace, now the base
    CHECK(exported.layers[0].filterMode == Layer::FilterMode::Blend);
    CHECK(exported.layers[1].textureId == 3);
    CHECK(exported.layers[1].filterMode == Layer::FilterMode::Modulate);
}

TEST_CASE("wem a passing stage draws only when it is the base", "[wem][materials][mdx]") {
    // `Pass` is Diablo III's identity: the stage masks alpha and leaves the
    // colour alone. Warcraft III has no filter mode that draws nothing, so a
    // later one is dropped — while the FIRST is the register's only value and
    // therefore what the chain draws. Dropping that left a wing as its two
    // masks, which is a bright sheet rather than a wing.
    Diagnostics diagnostics;
    const mdx::Material exported = mdx_core::ExportMaterial(
        makeChain({CombinerOp::Pass, CombinerOp::Mod, CombinerOp::Pass}, BlendMode::AdditiveAlpha),
        ProfileId::Wc3Classic, makeContext(), diagnostics);
    REQUIRE(exported.layers.size() == 2);
    CHECK(exported.layers[0].textureId == 0);
    CHECK(exported.layers[0].filterMode == Layer::FilterMode::AddAlpha);
    CHECK(exported.layers[1].textureId == 1);
    CHECK(exported.layers[1].filterMode == Layer::FilterMode::Modulate);
    CHECK(diagnostics.byCode(DiagCode::LayerDropped).size() == 1);
}

TEST_CASE("wem an invisible material draws nothing", "[wem][materials][mdx]") {
    // Diablo III's visibility bit is per (sub-object, look) and a material is
    // per (slot, look), so the material is the only record with the right scope
    // to hold it. MDX has no such flag and the idiom it does have is a blended
    // pass at zero opacity.
    Material material = makeChain({CombinerOp::Opaque});
    material.MutableCommon().flags |= MaterialFlags::Invisible;

    Diagnostics diagnostics;
    const mdx::Material exported =
        mdx_core::ExportMaterial(material, ProfileId::Wc3Classic, makeContext(), diagnostics);
    REQUIRE(exported.layers.size() == 1);
    CHECK(exported.layers[0].filterMode == Layer::FilterMode::Blend);
    CHECK(exported.layers[0].alpha == 0.0f);
    // The texture stays, so a tool reading the file still sees what the surface
    // would have worn.
    CHECK(exported.layers[0].textureId == 0);
}

TEST_CASE("wem a material with no drawable layer still writes one", "[wem][materials][mdx]") {
    // An empty stack is not "draws nothing" in MDX — a geoset whose material has
    // no layers draws untextured white, and Diablo's `FX_EMIT` emitter proxies
    // (which carry no texture at all) came through his chest as white shards.
    Material material;
    material.name = "FX_EMIT";
    material.InitCommon().body = CombinersBody{};

    Diagnostics diagnostics;
    const mdx::Material exported =
        mdx_core::ExportMaterial(material, ProfileId::Wc3Classic, makeContext(), diagnostics);
    REQUIRE(exported.layers.size() == 1);
    CHECK(exported.layers[0].filterMode == Layer::FilterMode::Blend);
    CHECK(exported.layers[0].alpha == 0.0f);
}

TEST_CASE("wem a native block in sync is what export writes", "[wem][materials][mdx]") {
    mdx::Material source = makeMaterial({Layer::FilterMode::Modulate});
    source.priorityPlane = 7;
    source.shader = "Shader_SD_FixedFunction";

    Diagnostics diagnostics;
    const Material imported =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);
    REQUIRE(imported.sync() == NativeSync::InSync);

    const mdx::Material exported =
        mdx_core::ExportMaterial(imported, ProfileId::Wc3Classic, makeContext(), diagnostics);
    CHECK(exported.priorityPlane == 7);
    CHECK(exported.shader == "Shader_SD_FixedFunction");
    REQUIRE(exported.layers.size() == 1);
    CHECK(exported.layers[0].filterMode == Layer::FilterMode::Modulate);
}

TEST_CASE("wem an add scaled by its own alpha keeps the scale", "[wem][materials][mdx]") {
    // Warcraft III has the op — `AddAlpha` is `dst + src.rgb * src.a` — and so
    // do five of World of Warcraft's shipped combiners. `CombinerOp` did not,
    // so they folded onto `Add` and every glow mask was added at full strength
    // over the whole surface. Kil'jaeden's skin came out tan.
    Diagnostics diagnostics;
    const mdx::Material exported =
        mdx_core::ExportMaterial(makeChain({CombinerOp::Opaque, CombinerOp::AddAlpha}),
                                 ProfileId::Wc3Classic, makeContext(), diagnostics);
    REQUIRE(exported.layers.size() == 2);
    CHECK(exported.layers[0].filterMode == Layer::FilterMode::None);
    CHECK(exported.layers[1].filterMode == Layer::FilterMode::AddAlpha);
}

TEST_CASE("wem a layer this export wrote names no texture animation", "[wem][materials][mdx]") {
    // `Layer::textureAnimationId` has no presence bit and `mdx::Layer` defaults
    // it to 0, which is a valid TXAN index. Every layer written here says the
    // sentinel instead, so the animation export hands out one entry per animated
    // layer rather than pouring every scrolling UV into entry 0.
    Diagnostics diagnostics;
    const mdx::Material exported =
        mdx_core::ExportMaterial(makeChain({CombinerOp::Opaque, CombinerOp::Mod}),
                                 ProfileId::Wc3Classic, makeContext(), diagnostics);
    REQUIRE(exported.layers.size() == 2);
    CHECK(exported.layers[0].textureAnimationId == 0xFFFFFFFFu);
    CHECK(exported.layers[1].textureAnimationId == 0xFFFFFFFFu);
}
