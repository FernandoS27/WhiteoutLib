// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file profile.h
 * @brief Profiles — the game and its shading generation (WEM v3, design §6).
 *
 * A profile is **the game and its shading generation, never the file format** —
 * the house rule the renderer already follows (`Diablo3Profile` reads `.app` files
 * through `D3ModelAdapter`). Seven profiles, four native block kinds: the profile
 * axis and the native-record axis are related but not the same size.
 *
 * `ProfileDesc` is pure data in a registry. Nothing here has behaviour, and the
 * profile is consulted at exactly three sites (§6.5): `Validate` at
 * `ValidateLevel::Profile`, export, and native-block construction. The geometry
 * layer never consults it — limits are checked, not enforced during editing.
 *
 * This header also owns the small vocabulary enums `ProfileDesc` describes a
 * profile *in terms of* (`BlendMode`, `CullMode`, `MaterialKind`, `NativeKind`),
 * because they are the capability alphabet; `materials/common.h` includes this
 * header rather than redeclaring them.
 */

#include <string>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

namespace whiteout {
namespace models {
namespace wem {

/// "No index" on the material, model and node-payload axes — the same
/// `0xFFFFFFFF` as `geom::kInvalidId` and `kInvalidNode`, spelled once for
/// everything above the geometry kernel.
inline constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

// ============================================================================
// ProfileId
// ============================================================================

enum class ProfileId : u32 {
    Generic = 0,     ///< No native blocks: authored, retargeted, or neutrally imported.
    Wc3Classic = 1,  ///< Warcraft III, SD shading (`.mdx`, `is_hd == false` layers).
    Wc3Reforged = 2, ///< Warcraft III, HD shading (`.mdx`, `is_hd == true` layers).
    Wow = 3,         ///< World of Warcraft (`.m2`).
    Sc2 = 4,         ///< StarCraft II (`.m3`, MODL v23–v29).
    Heroes = 5,      ///< Heroes of the Storm (`.m3`, MODL v30+ — MADD available).
    Diablo3 = 6,     ///< Diablo III (`.app` / `.acr`).
    Count
};

using ProfileMask = u32; ///< Bit per `ProfileId`.

inline constexpr ProfileMask kAllProfiles = ~0u;
inline constexpr ProfileMask kNoProfiles = 0u;

constexpr ProfileMask ProfileBit(ProfileId id) {
    return static_cast<ProfileMask>(1u) << static_cast<u32>(id);
}

constexpr bool HasProfile(ProfileMask mask, ProfileId id) {
    return (mask & ProfileBit(id)) != 0;
}

/// Registry name — "generic", "wc3_classic", … The stable spelling.
const char* ToString(ProfileId id);

/// Inverse of `ToString`; returns `ProfileId::Count` when nothing matches.
ProfileId ProfileFromName(const std::string& name);

// ============================================================================
// Space
// ============================================================================

/**
 * @brief The authoring space a format's geometry arrives in.
 *
 * WEM's canonical space is `Blizzard` (+X forward, +Y left, +Z up, right-handed,
 * CCW front faces). Every importer rebases into it and records `sourceSpace` so
 * the exporter can rebase back. For `Sc2` that rebase is a 90° yaw —
 * `new = (-old.y, old.x, old.z)`, determinant +1, so a rotation and not a mirror:
 * winding is preserved and no index reversal is needed. It is an axis permutation
 * with sign flips, so it is bit-exact in floating point.
 */
enum class CoordSpace : u8 {
    Blizzard, ///< +X forward, +Y left, +Z up.
    Sc2,      ///< -Y forward, +X left, +Z up (Max's basis verbatim).
};

enum class Handedness : u8 { Right, Left };

/**
 * @brief How a format's rig states the bind pose, and therefore what a node's
 *        TRS animation channels mean.
 *
 * The two are one fact, not two: every format that ships a per-bone inverse
 * bind matrix keys the bone's local transform absolutely, and every format that
 * does not folds the bind into the node's own composition and keys an offset
 * from it. All four agree, so naming them separately would only create a fifth
 * combination nothing writes.
 *
 * This is the fact `RetargetSkeleton` needs and could not previously ask for,
 * and it is why `.m3 -> .mdx` used to write a skeleton of zero pivots.
 */
enum class RigConvention : u8 {
    /**
     * @brief MDX, `.m2`. The inverse bind is the identity: the mesh is stored
     * in the bind pose and a node at rest contributes nothing.
     *
     * A node composes `T(-pivot) * S * R * T(pivot + t)`, so a TRS channel is
     * an **offset** and the pivot is a parameter of the composition rather than
     * a transform of its own. The linear part is therefore always `diag(s) * R`
     * — the format cannot hold shear, which is what makes the retarget into one
     * of these the interesting direction.
     */
    PivotRelative,

    /**
     * @brief `.m3` (IREF), D3 (`tTransform4`). One inverse model-space bind
     * matrix per bone, shipped beside the rest chain and **not** derivable from
     * it — on `Marine.m3` the two disagree by 2.2 units.
     *
     * A node composes `S * R * T` from its channels directly, and `pivot` is
     * zero.
     */
    ExplicitBind,
};

const char* ToString(RigConvention rig);

enum class WindingOrder : u8 { CounterClockwise, Clockwise };

enum class IndexWidth : u8 { U16, U32 };

const char* ToString(CoordSpace space);

// ============================================================================
// Material vocabulary
// ============================================================================

/**
 * @brief Rendering blend mode — the superset of MDX filterMode, M2 blendingMode
 *        and M3 BlendMode.
 *
 * Enumerator order is load-bearing: it is the on-disk value in both the v2 format
 * and v3's `CommonMaterial::blend`, so entries are appended, never reordered.
 */
enum class BlendMode : u32 {
    Opaque,             ///< MDX None(0), M2 Opaque(0), M3 Opaque(0)
    AlphaKey,           ///< M2 AlphaKey(1)
    AlphaBlend,         ///< MDX Blend(2), M2 Alpha(2), M3 AlphaBlend(1)
    Additive,           ///< MDX Additive(3), M2 Add(4), M3 Add(2)
    AdditiveAlpha,      ///< MDX AddAlpha(4), M2 NoAlphaAdd(3), M3 AlphaAdd(3)
    Modulate,           ///< MDX Modulate(5), M2 Mod(5), M3 Mod(4)
    Modulate2x,         ///< MDX Modulate2x(6), M2 Mod2x(6), M3 Mod2x(5)
    BlendAdd,           ///< M2 BlendAdd(7)
    Transparent,        ///< MDX Transparent(1)
    PremultipliedAlpha, ///< D3's premultiplied family (SrcBlend One) — see P6
};

const char* ToString(BlendMode mode);

enum class CullMode : u8 {
    Back,  ///< The default everywhere; front faces are CCW.
    Front, ///< WoW's mirrored-actor case.
    None,  ///< Two-sided.
};

/**
 * @brief The shading model a common material contracts to (§7.2).
 *
 * The kind *is* the contract a generic consumer renders by, which is why it
 * discriminates the body rather than being an advisory tag.
 */
enum class MaterialKind : u8 {
    Composite = 0,      ///< Ordered layer stack over named surface channels, lit forward.
    Combiners = 1,      ///< Fixed-function stage chain in one draw.
    LegacyDeferred = 2, ///< Named-slot G-buffer, legacy spec/gloss lighting.
    PBRDeferred = 3,    ///< Named-slot G-buffer, PBR lighting.
    Count
};

using MaterialKindMask = u8; ///< Bit per `MaterialKind`; `ProfileDesc::commonKinds`.

constexpr MaterialKindMask MaterialKindBit(MaterialKind kind) {
    return static_cast<MaterialKindMask>(1u << static_cast<u32>(kind));
}

constexpr bool HasMaterialKind(MaterialKindMask mask, MaterialKind kind) {
    return (mask & MaterialKindBit(kind)) != 0;
}

inline constexpr MaterialKindMask kAllMaterialKinds = static_cast<MaterialKindMask>(
    MaterialKindBit(MaterialKind::Composite) | MaterialKindBit(MaterialKind::Combiners) |
    MaterialKindBit(MaterialKind::LegacyDeferred) | MaterialKindBit(MaterialKind::PBRDeferred));

const char* ToString(MaterialKind kind);

/**
 * @brief Which native material record a profile's materials may carry (§6.1).
 *
 * Named for the **format**, because that is what a native block is — a record of
 * what the file held. The profile is named for the game. Same split as
 * `D3ModelAdapter` vs `Diablo3Profile`.
 */
enum class NativeKind : u8 {
    None = 0, ///< `Generic` only; the variant must be `monostate`.
    Mdx = 1,  ///< `Wc3Classic`, `Wc3Reforged`.
    M2 = 2,   ///< `Wow`.
    M3 = 3,   ///< `Sc2`, `Heroes`.
    D3 = 4,   ///< `Diablo3`.
};

const char* ToString(NativeKind kind);

// ============================================================================
// ProfileDesc
// ============================================================================

/**
 * @brief Everything WEM knows about a profile, as data.
 */
struct ProfileDesc {
    ProfileId id = ProfileId::Generic;
    const char* name = "";          ///< Registry name; `ToString(ProfileId)` returns it.
    const char* displayName = "";   ///< Human-readable.
    const char* formatId = nullptr; ///< "mdx" | "m2" | "m3" | "d3"; nullptr for generic.

    // --- space ---
    CoordSpace sourceSpace = CoordSpace::Blizzard;
    Handedness handedness = Handedness::Right;
    WindingOrder winding = WindingOrder::CounterClockwise;

    /// How this format states the bind pose — `RetargetSkeleton`'s target.
    RigConvention rig = RigConvention::PivotRelative;

    /**
     * @brief The RENDERER's framing constant. Documentation, not an operation.
     *
     * WoW's and SC2's 100 are unit conversions; D3's 17 is a framing constant
     * fitted so a 7.3-unit Barbarian lands where WC3's camera constants expect a
     * character. Neither belongs baked into WEM geometry, and a retarget that
     * treats them as interchangeable gets the scale wrong by 6x. Applied only
     * when `RetargetOptions::rescale` explicitly asks for it.
     */
    f32 sceneScale = 1.0f;

    // --- geometry limits ---
    u32 maxBoneInfluences = 4; ///< 4 on the GPU everywhere; D3 writes 3.
    u32 maxUvSets = 1;
    u32 maxBonesPerPalette = 0; ///< 0 = unlimited.
    IndexWidth indexWidth = IndexWidth::U16;
    bool allowsNgons = false;
    bool allowsVertexColor = false;

    // --- materials ---
    std::span<const BlendMode> blendModes;
    MaterialKindMask commonKinds = 0; ///< Which §7.2 kinds the exporter accepts.
    NativeKind nativeMaterialKind = NativeKind::None;
    bool supportsLooks = false;
    bool supportsActors = false;

    bool acceptsBlendMode(BlendMode mode) const;
};

/// The registry entry for a profile. `id` must be < `ProfileId::Count`.
const ProfileDesc& Profile(ProfileId id);

/// All seven entries, indexed by `ProfileId`.
std::span<const ProfileDesc> AllProfileDescs();

} // namespace wem
} // namespace models
} // namespace whiteout
