// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file features.h
 * @brief Cross-cutting material optionals (WEM v3, design §7.2.5).
 *
 * The third tier of the field-placement rule: kind-independent state on the
 * material header, per-shading-model state in the kind body, and everything that
 * cuts across the kinds here. It is where revision 2's casualties return —
 * `FresnelProperties` was evicted from the common material because only M3 and
 * Reforged MDX have one, which was right about the *rule* and wrong about the
 * destination, because a native block is invisible to every generic consumer.
 *
 * The contract that makes the mechanism safe, and the reason both initial
 * features are the ones they are:
 *
 * > A consumer that does not implement a feature must be able to render the
 * > material as if the feature were absent.
 *
 * `id` is never reused within a material — sub-tracks join on it (§10.8.1) — and
 * `layer` *is* an ordinal and therefore shifts under editing, which is what §7.5's
 * removal operations exist to keep honest.
 */

#include <variant>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/models/wem/reflect.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {

/// `MaterialFeature::layer` when the feature is about the whole material rather
/// than one of its layers, stages or slots. Same `0xFFFFFFFF` as every other
/// "no index" in WEM; named for what it means here.
inline constexpr u32 kWholeMaterial = 0xFFFFFFFFu;

enum class FeatureKind : u8 {
    Fresnel = 0,
    UvAnimation = 1,
    Count
    // Deliberately not designed yet: TeamColor, DepthFade, Refraction. Each needs
    // a profile that can express it on both sides before it earns a place.
};

const char* ToString(FeatureKind kind);

// ============================================================================
// Payloads
// ============================================================================

/**
 * @brief A rim term over the assembled surface.
 *
 * `outMin`/`outMax` keep M3's **measured** meaning: they remap the *result* of
 * the fresnel term, they are not an input clamp. MDX Reforged's
 * `{fresnelColor, fresnelOpacity, fresnelTeamColor}` maps as `color`,
 * `outMax = opacity` with `outMin = 0`, and `teamColor`.
 */
struct FresnelFeature {
    Vector3f color{1, 1, 1};
    f32 exponent = 1;
    f32 outMin = 0;
    f32 outMax = 1;
    f32 teamColor = 0; ///< MDX Reforged: how much team colour blends into `color`.

    template <class V>
    void reflect(V& v) {
        v.field("color", color);
        v.field("exponent", exponent);
        v.field("outMin", outMin);
        v.field("outMax", outMax);
        v.field("teamColor", teamColor);
    }
};

/**
 * @brief Moving UVs — every profile animates them and no two agree how.
 *
 * Constant-rate sources fill the rates: D3's `uvTransformMode` family (0..6,
 * 82.7% mode 2), and M3's `texAnimMatrices` where the function is linear. Keyed
 * sources — MDX `TextureAnimation`, M2 texture transforms reached through
 * `textureTransformCombos` — leave the rates zero and put sub-tracks on the
 * feature's `UvTranslate`/`UvRotate`/`UvScale` channels (§10.8).
 *
 * Both forms compose with `TextureInput::uvTransform`, which stays the static
 * part.
 */
struct UvAnimationFeature {
    Vector2f scrollRate{0, 0}; ///< UV units per second.
    f32 rotateRate = 0;        ///< Radians per second, about the UV centre.
    Vector2f scaleRate{0, 0};

    template <class V>
    void reflect(V& v) {
        v.field("scrollRate", scrollRate);
        v.field("rotateRate", rotateRate);
        v.field("scaleRate", scaleRate);
    }

    bool isConstantRate() const {
        return scrollRate.x != 0 || scrollRate.y != 0 || rotateRate != 0 || scaleRate.x != 0 ||
               scaleRate.y != 0;
    }
};

using FeaturePayload = std::variant<FresnelFeature, UvAnimationFeature>;

/// The payload alternative @p kind requires — the same redundancy as
/// `NodeKind`/`NodePayload`, and validated the same way.
constexpr std::size_t FeaturePayloadIndexFor(FeatureKind kind) {
    return static_cast<std::size_t>(kind);
}

// ============================================================================
// MaterialFeature
// ============================================================================

struct MaterialFeature {
    /// Stable within the material; never reused, because animation sub-tracks
    /// join on it and a reused id silently retargets one.
    u32 id = 0;
    /// A layer / stage / slot ordinal, or `kWholeMaterial`.
    u32 layer = kWholeMaterial;
    FeaturePayload payload = FresnelFeature{};

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
        v.field("layer", layer);
        const FeatureKind featureKind = VariantKind<FeatureKind>(v, "kind", payload);
        switch (featureKind) {
        case FeatureKind::Fresnel:
            v.chunk("fresnel", VariantAs<FresnelFeature>(payload));
            break;
        case FeatureKind::UvAnimation:
            v.chunk("uvAnimation", VariantAs<UvAnimationFeature>(payload));
            break;
        case FeatureKind::Count:
            break;
        }
    }

    FeatureKind kind() const {
        return static_cast<FeatureKind>(payload.index());
    }

    bool isWholeMaterial() const {
        return layer == kWholeMaterial;
    }

    FresnelFeature* fresnel() {
        return std::get_if<FresnelFeature>(&payload);
    }
    const FresnelFeature* fresnel() const {
        return std::get_if<FresnelFeature>(&payload);
    }
    UvAnimationFeature* uvAnimation() {
        return std::get_if<UvAnimationFeature>(&payload);
    }
    const UvAnimationFeature* uvAnimation() const {
        return std::get_if<UvAnimationFeature>(&payload);
    }
};

/// The feature of @p kind on @p layer, or null. At most one exists — `Validate`
/// asserts it — so "the" is well-defined.
const MaterialFeature* FindFeature(const std::vector<MaterialFeature>& features, FeatureKind kind,
                                   u32 layer);

/// One past the highest id in use, so ids only ever increase within a material —
/// which is what makes "never reused" true by construction rather than by
/// discipline.
u32 NextFeatureId(const std::vector<MaterialFeature>& features);

} // namespace wem
} // namespace models
} // namespace whiteout
