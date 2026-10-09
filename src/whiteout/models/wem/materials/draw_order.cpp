// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/materials/draw_order.h>

#include <whiteout/models/wem/anim/rests.h>
#include <whiteout/models/wem/converters.h>

#include <algorithm>
#include <cstring>
#include <numeric>

#include "../anim/prepared_track.h"
#include "mdx_core.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

// The game tests the byte an alpha rounds to and the renderer tests 0.004
// (`wc3_classify.cpp`'s `kAlphaEps`), so between the two an alpha is hidden to
// one reader and drawn to the other. Full is 1: the game's byte has to be 255,
// which is stricter than the renderer's 0.99 (`kOpaqueFadeAlpha`).
constexpr f32 kSurelyHidden = 0.5f / 255.0f;
constexpr f32 kMaybeHidden = 0.004f;

/// Which sides of drawn an alpha can read, over everything that plays it.
struct AlphaReads {
    bool hidden = false;  ///< Some reader can call it not drawn.
    bool partial = false; ///< Some reader can call it drawn, and less than full.

    void value(f32 alpha) {
        hidden = hidden || alpha <= kMaybeHidden;
        partial = partial || (alpha > kSurelyHidden && alpha < 1.0f);
    }
    /// Every value from @p low to @p high: a line between keys passes them all.
    void range(f32 low, f32 high) {
        partial = partial || (low < high && high > kSurelyHidden && low < 1.0f);
    }
    void anything() {
        hidden = true;
        partial = true;
    }
};

/// What @p channel reads over every clip of @p model, through the reader's own
/// keys (`prepared_track.h`), and where no clip keys it.
void ReadAlpha(const Document& document, u32 model, const AnimChannel& channel, Game storage,
               AlphaReads& out) {
    if (channel.valueType != geom::AttrType::F32) {
        out.anything();
        return;
    }
    bool keyed = false;
    for (const Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (const SubTrackContainer& container : clip.containers) {
            const SubTrack* track = container.find(channel.id);
            if (track == nullptr || track->times.empty()) {
                continue;
            }
            keyed = true;
            const prepared::PreparedTrack<f32> read = prepared::Prepare<f32>(clip, *track, false);
            if (!read.valid || read.keys.empty()) {
                out.anything();
                continue;
            }
            f32 low = read.keys.front().value;
            f32 high = low;
            bool flat = true;
            const Interpolation interp = prepared::PairInterpolation(read);
            for (const prepared::Key<f32>& key : read.keys) {
                out.value(key.value);
                low = std::min(low, key.value);
                high = std::max(high, key.value);
                // A Hermite key holds slopes and a Bezier one control points.
                const f32 level = interp == Interpolation::Hermite ? 0.0f : key.value;
                flat = flat && key.in == level && key.out == level;
            }
            if (interp == Interpolation::Step) {
                continue;
            }
            if (interp == Interpolation::Hermite || interp == Interpolation::Bezier) {
                // A curve can leave the range of its keys; only a flat one
                // between equal keys is known to stay where they are.
                if (low != high || !flat) {
                    out.anything();
                }
                continue;
            }
            // Any two of them, not only neighbours: the last key runs on into
            // the first where the animation loops.
            out.range(low, high);
        }
    }
    const TrackRests rests = RestsPlayed(document, model, channel, storage);
    const std::vector<u8>& rest = keyed ? rests.keyedElsewhere : rests.unkeyed;
    if (rest.size() != sizeof(f32)) {
        out.anything();
        return;
    }
    out.value(prepared::Load<f32>(rest.data()));
}

bool Has(mdx::Layer::ShadingFlag flags, mdx::Layer::ShadingFlag bit) {
    return (static_cast<u32>(flags) & static_cast<u32>(bit)) != 0;
}

/// @p material as Warcraft III is handed it for @p profile, by the game's rule.
bool WarcraftOrderFree(const Document& document, u32 model, const MeshSection& section,
                       const Material& material, ProfileId profile, const AlphaReads& sectionReads,
                       Game storage) {
    using Layer = mdx::Layer;
    mdx_core::Context context;
    context.modelVersion = MdxFileVersion(profile);
    context.textureIndexMap.resize(document.textures.size());
    std::iota(context.textureIndexMap.begin(), context.textureIndexMap.end(), 0u);
    Diagnostics unread;
    std::vector<u32> layerOfOrdinal;
    const mdx::Material written =
        mdx_core::ExportMaterial(material, profile, context, unread, &layerOfOrdinal);

    // Without the depth test or its write, a layer shows through or under
    // whatever was drawn before it, opaque or not.
    for (const Layer& layer : written.layers) {
        if (Has(layer.shadingFlags, Layer::ShadingFlag::NoDepthTest) ||
            Has(layer.shadingFlags, Layer::ShadingFlag::NoDepthSet)) {
            return false;
        }
    }
    const Model& owner = document.models[model];
    // Whether the layer at hand can be the first visible one, which is the one
    // the game asks; and whether a layer under it fills every pixel of the
    // geoset at every time.
    bool asked = true;
    bool covered = false;
    for (std::size_t l = 0; l < written.layers.size(); ++l) {
        const Layer& layer = written.layers[l];
        const bool opaque = layer.filterMode == Layer::FilterMode::None ||
                            layer.filterMode == Layer::FilterMode::Transparent;
        // A blended layer shows what lies under it. Asked, the geoset is
        // sorted. Over a layer that always covers it, that is the geoset's own
        // pixels; over a cut-out or nothing, it is the scene as it stands when
        // the layer is drawn.
        if (!opaque && (asked || !covered)) {
            return false;
        }
        AlphaReads reads;
        bool keyed = false;
        for (const AnimChannel& channel : owner.animChannels.channels) {
            const TrackTarget& target = channel.target;
            if (target.kind != TrackTarget::Kind::MaterialLayer || target.channel != Channel::Alpha ||
                target.material.profile != profile || target.material.slot != section.materialSlot) {
                continue;
            }
            // A layer track names an ordinal, which is not the layer's place
            // in the written stack (`ExportMaterial`).
            const bool whole = target.sub == kWholeMaterial;
            if (!whole && (target.sub >= layerOfOrdinal.size() || layerOfOrdinal[target.sub] != l)) {
                continue;
            }
            keyed = true;
            ReadAlpha(document, model, channel, storage, reads);
        }
        if (!keyed) {
            reads.value(layer.alpha);
        }
        if (asked && mdx_core::IsHdLayer(written, layer, context.modelVersion) &&
            (reads.partial || sectionReads.partial)) {
            return false;
        }
        // The next layer is asked only while this one can be hidden.
        asked = asked && reads.hidden;
        covered = covered || (layer.filterMode == Layer::FilterMode::None && !reads.hidden);
    }
    return true;
}

} // namespace

bool DrawsOrderFree(const Document& document, u32 model, u32 mesh, u32 section) {
    if (model >= document.models.size() || mesh >= document.models[model].meshes.size() ||
        section >= document.models[model].meshes[mesh].sections.size()) {
        return false;
    }
    const Model& owner = document.models[model];
    const MeshSection& drawn = owner.meshes[mesh].sections[section];
    const Game storage = GameOf(document.defaultProfile);

    // The section's own alpha: Warcraft III's geoset animation. A channel
    // names the mesh, whichever of its sections it was read from.
    AlphaReads sectionReads;
    bool keyed = false;
    for (const AnimChannel& channel : owner.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Section && channel.target.mesh == mesh &&
            channel.target.channel == Channel::Alpha) {
            keyed = true;
            ReadAlpha(document, model, channel, storage, sectionReads);
        }
    }
    if (!keyed) {
        sectionReads.value(SectionStaticAlpha(drawn));
    }

    for (const ProfileId profile : document.profiles) {
        if (!HasProfile(drawn.profiles, profile)) {
            continue;
        }
        const ProfileMaterialSet* set = owner.setFor(profile);
        const u32 looks = set != nullptr ? static_cast<u32>(set->looks.size()) : 0u;
        for (u32 look = 0; look < std::max(looks, 1u); ++look) {
            const Material* material = Resolve(owner, drawn.materialSlot, profile, look);
            if (material == nullptr) {
                continue;
            }
            if (GameOf(profile) == Game::Warcraft) {
                if (!WarcraftOrderFree(document, model, drawn, *material, profile, sectionReads,
                                       storage)) {
                    return false;
                }
                continue;
            }
            const CommonMaterial& common = material->Common();
            const bool opaque = common.blend == BlendMode::Opaque ||
                                common.blend == BlendMode::AlphaKey ||
                                common.blend == BlendMode::Transparent;
            if (!opaque || !common.depth.test || !common.depth.write) {
                return false;
            }
        }
    }
    return true;
}

} // namespace wem
} // namespace models
} // namespace whiteout
