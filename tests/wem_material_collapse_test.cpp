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
