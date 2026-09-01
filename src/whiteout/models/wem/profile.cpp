// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/profile.h>

namespace whiteout {
namespace models {
namespace wem {

namespace {

// ---------------------------------------------------------------------------
// Blend-mode sets, one per format family. These are the modes the *format*
// encodes, not the modes a renderer can produce: an exporter that meets a mode
// outside its profile's set records `DiagCode::LossyBlendMode` and writes the
// nearest neighbour.
// ---------------------------------------------------------------------------

/// MDX filterMode 0..6: None, Transparent, Blend, Additive, AddAlpha, Modulate, Modulate2x.
constexpr BlendMode kMdxBlendModes[] = {
    BlendMode::Opaque,        BlendMode::Transparent, BlendMode::AlphaBlend, BlendMode::Additive,
    BlendMode::AdditiveAlpha, BlendMode::Modulate,    BlendMode::Modulate2x,
};

/// M2 blendingMode 0..7.
constexpr BlendMode kM2BlendModes[] = {
    BlendMode::Opaque,   BlendMode::AlphaKey, BlendMode::AlphaBlend, BlendMode::AdditiveAlpha,
    BlendMode::Additive, BlendMode::Modulate, BlendMode::Modulate2x, BlendMode::BlendAdd,
};

/// M3 BlendMode 0..5.
constexpr BlendMode kM3BlendModes[] = {
    BlendMode::Opaque,        BlendMode::AlphaBlend, BlendMode::Additive,
    BlendMode::AdditiveAlpha, BlendMode::Modulate,   BlendMode::Modulate2x,
};

/// Diablo III. Provisional until P6 pins the RenderPass blend enum — which is
/// *not* D3DBLEND — against the reconstructed shader atlas.
constexpr BlendMode kD3BlendModes[] = {
    BlendMode::Opaque,     BlendMode::AlphaKey,           BlendMode::AlphaBlend,
    BlendMode::Additive,   BlendMode::AdditiveAlpha,      BlendMode::Modulate,
    BlendMode::Modulate2x, BlendMode::PremultipliedAlpha,
};

/// Everything WEM can name. `Generic` has no exporter, so nothing it allows can
/// be written wrong.
constexpr BlendMode kAllBlendModes[] = {
    BlendMode::Opaque,        BlendMode::AlphaKey,
    BlendMode::AlphaBlend,    BlendMode::Additive,
    BlendMode::AdditiveAlpha, BlendMode::Modulate,
    BlendMode::Modulate2x,    BlendMode::BlendAdd,
    BlendMode::Transparent,   BlendMode::PremultipliedAlpha,
};

constexpr MaterialKindMask kComposite = MaterialKindBit(MaterialKind::Composite);
constexpr MaterialKindMask kCombiners = MaterialKindBit(MaterialKind::Combiners);
constexpr MaterialKindMask kLegacy = MaterialKindBit(MaterialKind::LegacyDeferred);
constexpr MaterialKindMask kPbr = MaterialKindBit(MaterialKind::PBRDeferred);

template <std::size_t N>
constexpr std::span<const BlendMode> modes(const BlendMode (&a)[N]) {
    return std::span<const BlendMode>(a, N);
}

// ---------------------------------------------------------------------------
// The registry. Indexed by ProfileId; `kDescs[i].id == ProfileId(i)` is asserted
// by wem_profile_test.
//
// `winding` and `handedness` agree across all seven today — every Blizzard format
// is right-handed with CCW front faces, which is why no importer reverses indices
// and the SC2 rebase (a rotation, §6.4) costs nothing. The fields exist so a
// format that disagrees is describable, not so a converter has to guess.
// ---------------------------------------------------------------------------

ProfileDesc makeDesc(ProfileId id, const char* name, const char* displayName, const char* formatId,
                     CoordSpace space, f32 sceneScale, u32 maxInfluences, u32 maxUvSets,
                     u32 maxPalette, IndexWidth width, bool ngons, bool vertexColor,
                     std::span<const BlendMode> blends, MaterialKindMask kinds, NativeKind native,
                     bool looks, bool actors) {
    ProfileDesc d;
    d.id = id;
    d.name = name;
    d.displayName = displayName;
    d.formatId = formatId;
    d.sourceSpace = space;
    d.handedness = Handedness::Right;
    d.winding = WindingOrder::CounterClockwise;
    d.sceneScale = sceneScale;
    d.maxBoneInfluences = maxInfluences;
    d.maxUvSets = maxUvSets;
    d.maxBonesPerPalette = maxPalette;
    d.indexWidth = width;
    d.allowsNgons = ngons;
    d.allowsVertexColor = vertexColor;
    d.blendModes = blends;
    d.commonKinds = kinds;
    d.nativeMaterialKind = native;
    d.supportsLooks = looks;
    d.supportsActors = actors;
    return d;
}

const std::array<ProfileDesc, static_cast<std::size_t>(ProfileId::Count)>& descs() {
    static const std::array<ProfileDesc, static_cast<std::size_t>(ProfileId::Count)> table = {{
        // Generic is the permissive profile: no exporter, so its limits exist only
        // to be wider than every source it can be derived from. n-gons and U32
        // indices are allowed here and nowhere else, because an authored or
        // retargeted document is allowed to be richer than any single game.
        makeDesc(ProfileId::Generic, "generic", "Generic", nullptr, CoordSpace::Blizzard, 1.0f, 4,
                 8, 0, IndexWidth::U32, /*ngons*/ true, /*vcolor*/ true, modes(kAllBlendModes),
                 kAllMaterialKinds, NativeKind::None,
                 /*looks*/ true, /*actors*/ true),

        // Warcraft III, SD. One UV set; the layer stack is Composite, and collapses
        // to Combiners when the whole stack folds into one draw (§7.2.2).
        makeDesc(ProfileId::Wc3Classic, "wc3_classic", "Warcraft III (Classic)", "mdx",
                 CoordSpace::Blizzard, 1.0f, 4, 1, 0, IndexWidth::U16, false, false,
                 modes(kMdxBlendModes), kComposite | kCombiners, NativeKind::Mdx, false, false),

        // Warcraft III, HD. Same container, PBR shading: `SlotType` renames onto
        // `PbrSlot` and layer fresnel becomes a feature.
        makeDesc(ProfileId::Wc3Reforged, "wc3_reforged", "Warcraft III (Reforged)", "mdx",
                 CoordSpace::Blizzard, 1.0f, 4, 2, 0, IndexWidth::U16, false, false,
                 modes(kMdxBlendModes), kPbr, NativeKind::Mdx, false, false),

        // World of Warcraft. Stage order and combine ops *are* the material, so
        // Combiners is the only kind; looks carry texture variations (§8).
        makeDesc(ProfileId::Wow, "wow", "World of Warcraft", "m2", CoordSpace::Blizzard, 100.0f, 4,
                 2, 0, IndexWidth::U16, false, false, modes(kM2BlendModes), kCombiners,
                 NativeKind::M2, /*looks*/ true, false),

        // StarCraft II. Five UV sets; the 256-entry bone palette is the M3 ceiling.
        makeDesc(ProfileId::Sc2, "sc2", "StarCraft II", "m3", CoordSpace::Sc2, 100.0f, 4, 5, 256,
                 IndexWidth::U16, false, true, modes(kM3BlendModes), kComposite, NativeKind::M3,
                 false, false),

        // Heroes of the Storm. Same container and space; the difference is the
        // version range and therefore the available material kinds (MADD at v30).
        makeDesc(ProfileId::Heroes, "heroes", "Heroes of the Storm", "m3", CoordSpace::Sc2, 100.0f,
                 4, 5, 256, IndexWidth::U16, false, true, modes(kM3BlendModes), kComposite,
                 NativeKind::M3, false, false),

        // Diablo III. Three influences on disk, a packed vertex colour pair, and the
        // only profile with actors today. A Legacy stage block makes a material
        // Combiners; without one the shader-named texture types project onto
        // legacy slots.
        makeDesc(ProfileId::Diablo3, "diablo3", "Diablo III", "d3", CoordSpace::Blizzard, 17.0f, 3,
                 2, 0, IndexWidth::U16, false, /*vcolor*/ true, modes(kD3BlendModes),
                 kCombiners | kLegacy, NativeKind::D3, /*looks*/ true,
                 /*actors*/ true),
    }};
    return table;
}

} // namespace

// ============================================================================

const char* ToString(ProfileId id) {
    if (static_cast<u32>(id) >= static_cast<u32>(ProfileId::Count)) {
        return "invalid";
    }
    return descs()[static_cast<std::size_t>(id)].name;
}

ProfileId ProfileFromName(const std::string& name) {
    const auto& table = descs();
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (name == table[i].name) {
            return static_cast<ProfileId>(i);
        }
    }
    return ProfileId::Count;
}

const char* ToString(CoordSpace space) {
    switch (space) {
    case CoordSpace::Blizzard:
        return "blizzard";
    case CoordSpace::Sc2:
        return "sc2";
    }
    return "invalid";
}

const char* ToString(BlendMode mode) {
    switch (mode) {
    case BlendMode::Opaque:
        return "opaque";
    case BlendMode::AlphaKey:
        return "alpha_key";
    case BlendMode::AlphaBlend:
        return "alpha_blend";
    case BlendMode::Additive:
        return "additive";
    case BlendMode::AdditiveAlpha:
        return "additive_alpha";
    case BlendMode::Modulate:
        return "modulate";
    case BlendMode::Modulate2x:
        return "modulate_2x";
    case BlendMode::BlendAdd:
        return "blend_add";
    case BlendMode::Transparent:
        return "transparent";
    case BlendMode::PremultipliedAlpha:
        return "premultiplied_alpha";
    }
    return "invalid";
}

const char* ToString(MaterialKind kind) {
    switch (kind) {
    case MaterialKind::Composite:
        return "composite";
    case MaterialKind::Combiners:
        return "combiners";
    case MaterialKind::LegacyDeferred:
        return "legacy_deferred";
    case MaterialKind::PBRDeferred:
        return "pbr_deferred";
    case MaterialKind::Count:
        break;
    }
    return "invalid";
}

const char* ToString(NativeKind kind) {
    switch (kind) {
    case NativeKind::None:
        return "none";
    case NativeKind::Mdx:
        return "mdx";
    case NativeKind::M2:
        return "m2";
    case NativeKind::M3:
        return "m3";
    case NativeKind::D3:
        return "d3";
    }
    return "invalid";
}

bool ProfileDesc::acceptsBlendMode(BlendMode mode) const {
    for (BlendMode m : blendModes) {
        if (m == mode) {
            return true;
        }
    }
    return false;
}

const ProfileDesc& Profile(ProfileId id) {
    const auto& table = descs();
    const auto index = static_cast<std::size_t>(id);
    return table[index < table.size() ? index : 0];
}

std::span<const ProfileDesc> AllProfileDescs() {
    const auto& table = descs();
    return std::span<const ProfileDesc>(table.data(), table.size());
}

} // namespace wem
} // namespace models
} // namespace whiteout
