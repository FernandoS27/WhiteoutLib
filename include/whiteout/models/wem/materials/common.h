// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file common.h
 * @brief `CommonMaterial` — one of four shading models (WEM v3, design §7.2).
 *
 * Revision 2's common material was one flat struct — a semantic-keyed slot list
 * plus Blinn-Phong factors — which is secretly *one* shading model (M3's standard
 * material, sanded down) posing as neutral. A WoW material has no slot semantics
 * at all (stage order and combine ops **are** the material); a Reforged surface
 * has different slots under a different lighting model; and WC3's layer stack had
 * to flee to the native block because a semantic-keyed map cannot hold an ordered
 * stack.
 *
 * So v3 makes the shading model explicit: a common material **is one of four
 * kinds**, and the kind is the contract a generic consumer renders by. Two of the
 * four are ordered (`Composite`, `Combiners`) and two are slot maps
 * (`LegacyDeferred`, `PBRDeferred`), which is the split the shipped data actually
 * has — a D3 material's texture keys provably never repeat across 19,200 shipped
 * variants, and a WC3 stack provably has an order that matters.
 *
 * Field placement is three tiers: kind-independent state on the header,
 * per-shading-model state in the kind body, cross-cutting optionals as features
 * (features.h).
 */

#include <string>
#include <variant>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/models/wem/reflect.h>
#include <whiteout/vector_types.h>

#include "../profile.h"
#include "features.h"
#include "texture.h"

namespace whiteout {
namespace models {
namespace wem {

/// "No index" on the material and model axes — the same `0xFFFFFFFF` as
/// `geom::kInvalidId` and `kInvalidNode`, spelled once for everything above the
/// geometry kernel.
inline constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

// ============================================================================
// Kind-independent state
// ============================================================================

/**
 * @brief What every kind needs before it can draw at all.
 *
 * Note what is *not* a flag: depth test and depth write are `DepthState`, and
 * two-sidedness is `CullMode::None`. MDX spells those as layer flags and M2 as
 * render flags; keeping them as state rather than bits is what lets a converter
 * map both without a lookup table.
 */
struct DepthState {
    bool test = true;
    bool write = true;
    f32 bias = 0;

    template <class V>
    void reflect(V& v) {
        v.field("test", test);
        v.field("write", write);
        v.field("bias", bias);
    }
};

enum class MaterialFlags : u32 {
    None = 0,
    Unlit = 0x01,    ///< MDX Unshaded, M2 Unlit, M3; D3's tag 0xA000F.
    Unfogged = 0x02, ///< MDX, M2.
    NoShadowCast = 0x04,
    NoShadowReceive = 0x08,
    SortNearZ = 0x10, ///< MDX SortPrimsNearZ — a draw-order hint, not a depth mode.
    SortFarZ = 0x20,  ///< MDX SortPrimsFarZ.
};

constexpr MaterialFlags operator|(MaterialFlags a, MaterialFlags b) {
    return static_cast<MaterialFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr MaterialFlags operator&(MaterialFlags a, MaterialFlags b) {
    return static_cast<MaterialFlags>(static_cast<u32>(a) & static_cast<u32>(b));
}
inline MaterialFlags& operator|=(MaterialFlags& a, MaterialFlags b) {
    a = a | b;
    return a;
}
constexpr bool hasFlag(MaterialFlags value, MaterialFlags bit) {
    return (static_cast<u32>(value) & static_cast<u32>(bit)) != 0;
}

// ============================================================================
// TextureInput
// ============================================================================

/**
 * @brief A 2D affine transform, rows `(m00 m01 m02)` / `(m10 m11 m12)`, applied
 *        as `uv' = M * (u, v, 1)`.
 *
 * `vector_types.h` has no 3x2 and a 4x4 would invite storing things a UV
 * transform cannot have. Affine rather than TRS because M3 and D3 both ship a
 * matrix, and a matrix can shear.
 */
struct Matrix3x2f {
    f32 m[2][3] = {{1, 0, 0}, {0, 1, 0}};

    template <class V>
    void reflect(V& v) {
        // A C array has no `reflect()` and is not one of the field kinds, so
        // the six values are named individually. Row-major, matching `m`.
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 3; ++col) {
                v.field("m", m[row][col]);
            }
        }
    }

    static Matrix3x2f identity() {
        return Matrix3x2f{};
    }
    bool isIdentity() const;
    Vector2f apply(const Vector2f& uv) const;
};

/// How a texture's coordinates are produced. Revision 2's `UVMappingMode` had one
/// enumerator per explicit UV set *and* the projections; the set is
/// `TextureInput::uvSet` now, so this is only the projection family.
enum class UVMappingMode : u8 {
    ExplicitUV, ///< Read `uvSet` from the mesh.
    EnvSphere,  ///< MDX SphereEnvMap, M3 spherical environment.
    EnvCube,    ///< M3 cubic environment reflection.
};

const char* ToString(UVMappingMode mode);

/**
 * @brief One texture input, wherever a kind body wants one.
 *
 * Revision 2's `TextureSlot` minus `semantic`: the key moved into the kind
 * bodies, where its meaning lives. `texture == kInvalidIndex` means there is no
 * texture and `constant` **is** the input, which is how a factor-only slot and a
 * textured one stay the same shape.
 */
struct TextureInput {
    u32 texture = kInvalidIndex; ///< -> `Document::textures[]`.
    Vector4f constant{1, 1, 1, 1};
    u32 uvSet = 0;
    UVMappingMode mapping = UVMappingMode::ExplicitUV;
    ColorSpace colorSpace = ColorSpace::Auto;
    WrapMode wrapU = WrapMode::Repeat;
    WrapMode wrapV = WrapMode::Repeat;
    Matrix3x2f uvTransform; ///< The STATIC part; animation is a feature (§7.2.5).
    f32 weight = 1.0f;      ///< Static per-layer/per-stage transparency.

    template <class V>
    void reflect(V& v) {
        v.field("texture", texture);
        v.field("constant", constant);
        v.field("uvSet", uvSet);
        v.field("mapping", mapping);
        v.field("colorSpace", colorSpace);
        v.field("wrapU", wrapU);
        v.field("wrapV", wrapV);
        v.field("uvTransform", uvTransform);
        v.field("weight", weight);
    }

    bool hasTexture() const {
        return texture != kInvalidIndex;
    }
};

// ============================================================================
// Composite — SC2's material, and WC3's implicitly
// ============================================================================

enum class SurfaceChannel : u8 {
    Color = 0,
    Emissive,
    Specular,
    Normal,
    AmbientOcclusion,
    /// M3's env layer family. Sampled per `TextureInput::mapping`
    /// (`EnvSphere` / `EnvCube`), scaled by `CompositeBody::environmentFactor`.
    Environment,
    Count
};

const char* ToString(SurfaceChannel channel);

/**
 * @brief WC3's filter modes, verbatim — the superset in real use.
 *
 * M3's own layer op field is near-signal-free in shipped content (mod and add
 * cover it), so the richer vocabulary is the one worth keeping.
 */
enum class CompositeOp : u8 {
    Set = 0,    ///< Replace. WC3 None; the first layer of a channel.
    AlphaKey,   ///< Replace where `sample.a >= alphaTestThreshold`. WC3 Transparent.
    AlphaBlend, ///< Lerp by source alpha. WC3 Blend.
    Add,        ///< WC3 Additive; M3 add.
    AddAlpha,   ///< Add, scaled by source alpha. WC3 AddAlpha.
    Modulate,   ///< WC3 Modulate; M3 mod.
    Modulate2x, ///< WC3 Modulate2x; M3 mod2x.
    Count
};

const char* ToString(CompositeOp op);

struct CompositeLayer {
    TextureInput input;
    SurfaceChannel target = SurfaceChannel::Color;
    CompositeOp op = CompositeOp::Set;

    template <class V>
    void reflect(V& v) {
        v.field("input", input);
        v.field("target", target);
        v.field("op", op);
    }
};

/**
 * @brief An ordered layer stack over named surface channels, lit forward.
 *
 * The evaluation contract, stated once: **per channel, fold that channel's layers
 * left to right with their ops, starting from the factor defaults; then light the
 * assembled surface with the legacy forward model unless `Unlit`.**
 *
 * The MDX split that makes WC3's multi-pass drawing agree with this fold: the
 * *first* layer's filter mode is not an op — it is how the whole stack meets the
 * scene — so it becomes the header `blend` and the layer imports with op `Set`.
 * On that split, fold-in-shader and blend-per-pass agree op for op **provided the
 * first pass replaces every pixel the draw covers**. Where the header itself
 * reads the scene, the two genuinely diverge; WEM's contract is the fold, and the
 * converter says so with a diagnostic.
 */
struct CompositeBody {
    std::vector<CompositeLayer> layers; ///< Ordered — order IS evaluation order.
    Vector4f diffuseFactor{1, 1, 1, 1};
    Vector4f emissiveFactor{0, 0, 0, 0};
    Vector4f specularFactor{0, 0, 0, 1};
    f32 specularExponent = 0;
    f32 environmentFactor = 0;

    /// The layers targeting @p channel, in order. Convenience for the fold.

    template <class V>
    void reflect(V& v) {
        v.field("layers", layers);
        v.field("diffuseFactor", diffuseFactor);
        v.field("emissiveFactor", emissiveFactor);
        v.field("specularFactor", specularFactor);
        v.field("specularExponent", specularExponent);
        v.field("environmentFactor", environmentFactor);
    }

    std::vector<u32> layersOf(SurfaceChannel channel) const;
};

// ============================================================================
// Combiners — WoW's material
// ============================================================================

/// M2's vocabulary. The pixel shaders are literally named for it:
/// `Combiners_Opaque_Mod2x` is stage 0 `Opaque`, stage 1 `Mod2x`.
enum class CombinerOp : u8 { Opaque = 0, Mod, Mod2x, Add, Decal, Fade, Count };

const char* ToString(CombinerOp op);

struct CombinerStage {
    TextureInput input;
    CombinerOp rgb = CombinerOp::Opaque;
    CombinerOp alpha = CombinerOp::Opaque;

    template <class V>
    void reflect(V& v) {
        v.field("input", input);
        v.field("rgb", rgb);
        v.field("alpha", alpha);
    }
};

/**
 * @brief A fixed-function stage chain in one draw.
 *
 * The difference from `Composite`, since the two look similar on paper: a
 * combiner chain runs against one running register inside one draw and never
 * names a surface channel; a composite stack is defined over channel accumulators
 * and may be drawn as passes. Stage 0 seeds the register, stage *i* applies
 * `op(register, sample)`, and the register is what draws.
 *
 * Static per-stage transparency is `TextureInput::weight`. The animated form —
 * WoW's batch colour x transparency, where only texture unit 0 takes the weight
 * and the *product* is what draws — targets the stage through §10.8's material
 * channels; the unit-0 rule is the M2 converter's to apply, not the schema's.
 */
struct CombinersBody {
    std::vector<CombinerStage> stages;
    Vector4f diffuseFactor{1, 1, 1, 1};
    Vector4f emissiveFactor{0, 0, 0, 0};

    template <class V>
    void reflect(V& v) {
        v.field("stages", stages);
        v.field("diffuseFactor", diffuseFactor);
        v.field("emissiveFactor", emissiveFactor);
    }
};

// ============================================================================
// LegacyDeferred
// ============================================================================

/**
 * @brief SC2's slot vocabulary — that generation's lingua franca.
 *
 * The kind is named for the **lighting family** (a legacy spec/gloss G-buffer),
 * not for a claim about SC2's pass structure: SC2 itself runs forward with a
 * G-buffer sidecar and defers only local lights.
 */
enum class LegacySlot : u8 {
    Diffuse = 0,
    Normal,
    Specular,
    Gloss,
    Emissive,
    Environment,
    AmbientOcclusion,
    Height,
    Lightmap,
    Count
};

const char* ToString(LegacySlot slot);

struct LegacyDeferredBody {
    /// At most one entry per slot — `Validate` asserts it. A vector rather than a
    /// fixed array because most materials fill two or three of nine.
    std::vector<std::pair<LegacySlot, TextureInput>> slots;
    Vector4f diffuseFactor{1, 1, 1, 1};
    Vector4f emissiveFactor{0, 0, 0, 0};
    Vector4f specularFactor{0, 0, 0, 1};
    f32 specularExponent = 0;
    f32 environmentFactor = 0;

    const TextureInput* find(LegacySlot slot) const;

    template <class V>
    void reflect(V& v) {
        // `std::pair` has neither a `reflect()` nor a chunk tag, so the run is
        // an inline count and two named halves per entry.
        v.count("slots", slots);
        for (auto& entry : slots) {
            v.field("slot", entry.first);
            v.field("input", entry.second);
        }
        v.field("diffuseFactor", diffuseFactor);
        v.field("emissiveFactor", emissiveFactor);
        v.field("specularFactor", specularFactor);
        v.field("specularExponent", specularExponent);
        v.field("environmentFactor", environmentFactor);
    }

    TextureInput& set(LegacySlot slot, const TextureInput& input);
};

// ============================================================================
// PBRDeferred
// ============================================================================

/**
 * @brief Reforged's `SlotType` enum, nearly verbatim.
 *
 * Deliberate: Reforged HD is the only shipped PBR content among the six games, so
 * its vocabulary is the ground truth and the MDX HD import is a rename, not a
 * mapping. `Orm` and the unpacked `Metallic`/`Roughness`/`AmbientOcclusion` are
 * **alternatives, not composable** — `Validate` rejects a body carrying both —
 * and DCC-sourced authoring (glTF-style split maps) uses the unpacked form.
 */
enum class PbrSlot : u8 {
    BaseColor = 0,
    Normal,
    Orm, ///< Occlusion-roughness-metalness, packed.
    Metallic,
    Roughness,
    AmbientOcclusion,
    Emissive,
    Environment,
    TeamColorMask,
    Count
};

const char* ToString(PbrSlot slot);

struct PbrDeferredBody {
    std::vector<std::pair<PbrSlot, TextureInput>> slots;
    Vector4f baseColorFactor{1, 1, 1, 1};
    f32 metallicFactor = 1;
    f32 roughnessFactor = 1;
    Vector3f emissiveFactor{0, 0, 0};

    const TextureInput* find(PbrSlot slot) const;

    template <class V>
    void reflect(V& v) {
        // `std::pair` has neither a `reflect()` nor a chunk tag, so the run is
        // an inline count and two named halves per entry.
        v.count("slots", slots);
        for (auto& entry : slots) {
            v.field("slot", entry.first);
            v.field("input", entry.second);
        }
        v.field("baseColorFactor", baseColorFactor);
        v.field("metallicFactor", metallicFactor);
        v.field("roughnessFactor", roughnessFactor);
        v.field("emissiveFactor", emissiveFactor);
    }

    TextureInput& set(PbrSlot slot, const TextureInput& input);
};

// ============================================================================
// CommonMaterial
// ============================================================================

using MaterialBody =
    std::variant<CompositeBody, CombinersBody, LegacyDeferredBody, PbrDeferredBody>;

/// The body alternative @p kind requires — the same deliberate redundancy as
/// `NodeKind`/`NodePayload`: `kind()` is the cheap discriminator the disk format
/// and the bindings key on, and their agreement is a structural rule.
constexpr std::size_t BodyIndexFor(MaterialKind kind) {
    return static_cast<std::size_t>(kind);
}

struct CommonMaterial {
    // --- kind-independent state ---
    BlendMode blend = BlendMode::Opaque;
    CullMode cull = CullMode::Back;
    DepthState depth;
    f32 alphaTestThreshold = 0; ///< [0,1] normalised — not M3's 0..255.
    i32 priorityPlane = 0;
    MaterialFlags flags = MaterialFlags::None;

    MaterialBody body = CompositeBody{};
    std::vector<MaterialFeature> features;

    template <class V>
    void reflect(V& v) {
        v.field("blend", blend);
        v.field("cull", cull);
        v.field("depth", depth);
        v.field("alphaTestThreshold", alphaTestThreshold);
        v.field("priorityPlane", priorityPlane);
        v.field("flags", flags);

        // The body is the kind: `kind()` reads the variant index, so the
        // discriminator on disk and the alternative in memory cannot drift.
        const MaterialKind bodyKind = VariantKind<MaterialKind>(v, "kind", body);
        switch (bodyKind) {
        case MaterialKind::Composite:
            v.chunk("composite", VariantAs<CompositeBody>(body));
            break;
        case MaterialKind::Combiners:
            v.chunk("combiners", VariantAs<CombinersBody>(body));
            break;
        case MaterialKind::LegacyDeferred:
            v.chunk("legacyDeferred", VariantAs<LegacyDeferredBody>(body));
            break;
        case MaterialKind::PBRDeferred:
            v.chunk("pbrDeferred", VariantAs<PbrDeferredBody>(body));
            break;
        case MaterialKind::Count:
            break;
        }

        v.field("features", features);
    }

    MaterialKind kind() const {
        return static_cast<MaterialKind>(body.index());
    }

    /// Replaces the body with @p kind's default. Destructive by design — a kind
    /// *change* that preserves content is `DeriveProfile`'s job, not a setter's.
    void setKind(MaterialKind kind);

    CompositeBody* composite() {
        return std::get_if<CompositeBody>(&body);
    }
    const CompositeBody* composite() const {
        return std::get_if<CompositeBody>(&body);
    }
    CombinersBody* combiners() {
        return std::get_if<CombinersBody>(&body);
    }
    const CombinersBody* combiners() const {
        return std::get_if<CombinersBody>(&body);
    }
    LegacyDeferredBody* legacy() {
        return std::get_if<LegacyDeferredBody>(&body);
    }
    const LegacyDeferredBody* legacy() const {
        return std::get_if<LegacyDeferredBody>(&body);
    }
    PbrDeferredBody* pbr() {
        return std::get_if<PbrDeferredBody>(&body);
    }
    const PbrDeferredBody* pbr() const {
        return std::get_if<PbrDeferredBody>(&body);
    }

    /// How many layers / stages / slots the body has — what a feature's `layer`
    /// and an animation sub-track's `sub` index into.
    u32 ordinalCount() const;

    /// The texture input at @p ordinal, or null. One accessor instead of four
    /// switch statements at every call site that walks a material's textures.
    const TextureInput* inputAt(u32 ordinal) const;
    TextureInput* inputAt(u32 ordinal);

    const MaterialFeature* feature(FeatureKind kind, u32 layer = kWholeMaterial) const {
        return FindFeature(features, kind, layer);
    }
};

// ============================================================================
// Colour space, and the one kind refinement
// ============================================================================

/// What `ColorSpace::Auto` means in each slot vocabulary. Colour channels are
/// sRGB; everything else is data.
ColorSpace AutoColorSpaceFor(SurfaceChannel channel);
ColorSpace AutoColorSpaceFor(LegacySlot slot);
ColorSpace AutoColorSpaceFor(PbrSlot slot);
/// A combiner stage has no semantic at all, so its samples are always colour.
ColorSpace AutoColorSpaceForCombinerStage();

/// `input.colorSpace` when it is explicit, the slot's default when it is `Auto`.
ColorSpace ResolvedColorSpace(const TextureInput& input, SurfaceChannel channel);
ColorSpace ResolvedColorSpace(const TextureInput& input, LegacySlot slot);
ColorSpace ResolvedColorSpace(const TextureInput& input, PbrSlot slot);

/**
 * @brief The degenerate `Composite` -> `LegacyDeferred` refinement.
 *
 * A composite body where every channel holds at most one layer and that layer's
 * op is `Set` *is* a slot map, and a large share of shipped SC2 materials are
 * exactly that. Returns `nullopt` when the body is not degenerate.
 *
 * The relation is one-directional: `Gloss`, `Height` and `Lightmap` are
 * legacy-only slots no `SurfaceChannel` feeds, so the kind is a strict superset
 * of what this can produce.
 */
std::optional<LegacyDeferredBody> Flatten(const CompositeBody& composite);

} // namespace wem
} // namespace models
} // namespace whiteout
