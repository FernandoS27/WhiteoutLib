// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/soft.h>

#include <algorithm>
#include <iterator>

namespace whiteout {
namespace models {
namespace wem {

const char* ToString(SoftEffectKind kind) {
    switch (kind) {
    case SoftEffectKind::Inflate:
        return "inflate";
    case SoftEffectKind::Bulge:
        return "bulge";
    case SoftEffectKind::Jiggle:
        return "jiggle";
    case SoftEffectKind::Wobble:
        return "wobble";
    case SoftEffectKind::Blobify:
        return "blobify";
    case SoftEffectKind::Drip:
        return "drip";
    case SoftEffectKind::SquashStretch:
        return "squash_stretch";
    case SoftEffectKind::Melt:
        return "melt";
    case SoftEffectKind::Ripple:
        return "ripple";
    case SoftEffectKind::Count:
        break;
    }
    return "unknown";
}

const char* ToString(SoftLumpKind kind) {
    switch (kind) {
    case SoftLumpKind::Bone:
        return "bone";
    case SoftLumpKind::Copy:
        return "copy";
    case SoftLumpKind::Surface:
        return "surface";
    case SoftLumpKind::Count:
        break;
    }
    return "unknown";
}

const char* ToString(SoftJigglePreset preset) {
    switch (preset) {
    case SoftJigglePreset::Custom:
        return "custom";
    case SoftJigglePreset::Jelly:
        return "jelly";
    case SoftJigglePreset::Fat:
        return "fat";
    case SoftJigglePreset::Muscle:
        return "muscle";
    case SoftJigglePreset::Rubber:
        return "rubber";
    case SoftJigglePreset::WaterBalloon:
        return "water_balloon";
    case SoftJigglePreset::Water:
        return "water";
    case SoftJigglePreset::Slime:
        return "slime";
    case SoftJigglePreset::Count:
        break;
    }
    return "unknown";
}

const char* ToString(SoftDripPreset preset) {
    switch (preset) {
    case SoftDripPreset::Custom:
        return "custom";
    case SoftDripPreset::Wax:
        return "wax";
    case SoftDripPreset::Slime:
        return "slime";
    case SoftDripPreset::Blood:
        return "blood";
    case SoftDripPreset::Paint:
        return "paint";
    case SoftDripPreset::Chocolate:
        return "chocolate";
    case SoftDripPreset::Honey:
        return "honey";
    case SoftDripPreset::Water:
        return "water";
    case SoftDripPreset::Count:
        break;
    }
    return "unknown";
}

namespace {

/// @p value's name of @p names, which are in the enum's order.
template <class E, std::size_t N>
const char* NameOf(E value, const char* const (&names)[N]) {
    const auto at = static_cast<std::size_t>(value);
    return at < N ? names[at] : "unknown";
}

} // namespace

const char* ToString(SoftAlong along) {
    static const char* const kNames[] = {"surface", "bones"};
    return NameOf(along, kNames);
}

const char* ToString(SoftAxis axis) {
    static const char* const kNames[] = {"auto", "up", "x", "y", "z", "bone", "motion"};
    return NameOf(axis, kNames);
}

const char* ToString(SoftComesFrom from) {
    static const char* const kNames[] = {"spans", "times", "events", "contact"};
    return NameOf(from, kNames);
}

const char* ToString(SoftPulse pulse) {
    static const char* const kNames[] = {"none", "breathe", "heartbeat", "rate"};
    return NameOf(pulse, kNames);
}

const char* ToString(SoftBulgeShape shape) {
    static const char* const kNames[] = {"round", "ring"};
    return NameOf(shape, kNames);
}

const char* ToString(SoftWobblePattern pattern) {
    static const char* const kNames[] = {"jelly", "sway", "roll", "random"};
    return NameOf(pattern, kNames);
}

const char* ToString(SoftNeckLaw law) {
    static const char* const kNames[] = {"linear", "snap", "thread", "sets"};
    return NameOf(law, kNames);
}

const char* ToString(SoftSquashMode mode) {
    static const char* const kNames[] = {"auto", "keyed"};
    return NameOf(mode, kNames);
}

const char* ToString(SoftAnchor anchor) {
    static const char* const kNames[] = {"bottom", "centre", "pivot"};
    return NameOf(anchor, kNames);
}

const char* ToString(SoftMeltOrder order) {
    static const char* const kNames[] = {"top", "bottom", "all_at_once"};
    return NameOf(order, kNames);
}

const char* ToString(SoftRipplePlays plays) {
    static const char* const kNames[] = {"once", "loops"};
    return NameOf(plays, kNames);
}

const char* ToString(SoftThrough through) {
    static const char* const kNames[] = {"skin", "body"};
    return NameOf(through, kNames);
}

bool SoftEffect::settingsMatchKind() const {
    const bool set[] = {inflate.has_value(), bulge.has_value(),  jiggle.has_value(),
                        wobble.has_value(),  blobify.has_value(), drip.has_value(),
                        squash.has_value(),  melt.has_value(),    ripple.has_value()};
    const auto mine = static_cast<std::size_t>(kind);
    for (std::size_t k = 0; k < std::size(set); ++k) {
        if (set[k] != (k == mine)) {
            return false;
        }
    }
    return mine < std::size(set);
}

SoftEffect DefaultSoftEffect(SoftEffectKind kind) {
    SoftEffect effect;
    effect.kind = kind;
    switch (kind) {
    case SoftEffectKind::Inflate:
        effect.inflate.emplace();
        effect.amount = 0.5f;
        break;
    case SoftEffectKind::Bulge:
        effect.bulge.emplace();
        effect.amount = 0.5f;
        break;
    case SoftEffectKind::Jiggle:
        // A jelly: it swings far enough, and long enough, to be seen at once.
        // And no farther than a hand keeps its fingers at: a quarter of the
        // body's size, which a record's own *Limit* allows, leaves a fist
        // behind as threads on any attack.
        effect.jiggle = SoftJigglePresetOf(SoftJigglePreset::Jelly);
        effect.jiggle->limit = 0.08f;
        break;
    case SoftEffectKind::Wobble:
        effect.wobble.emplace();
        effect.amount = 0.08f;
        effect.drivesJiggle = false;
        break;
    case SoftEffectKind::Blobify:
        effect.blobify.emplace();
        break;
    case SoftEffectKind::Drip:
        effect.drip.emplace();
        break;
    case SoftEffectKind::SquashStretch:
        // Stretched, trailing and leaning enough to read in a clip that moves
        // in place, as a game's walks and attacks do.
        effect.squash.emplace();
        effect.squash->stretch = 0.6f;
        effect.squash->squash = 0.6f;
        effect.squash->drag = 0.3f;
        effect.squash->windUp = 0.3f;
        break;
    case SoftEffectKind::Melt:
        effect.melt.emplace();
        break;
    case SoftEffectKind::Ripple:
        // Wave after wave: one alone has passed before it is looked at. Two
        // crests a body, and no higher than a waist or a wrist can take: one
        // crest as long as the body is no ripple but the whole of it swelling
        // and pinching, and a twentieth of its size is most of a thin part.
        effect.ripple.emplace();
        effect.ripple->plays = SoftRipplePlays::Loops;
        effect.ripple->wavelength = 0.5f;
        effect.amount = 0.03f;
        effect.drivesJiggle = false;
        break;
    case SoftEffectKind::Count:
        break;
    }
    return effect;
}

const SoftEffect* SoftBody::effect(u32 wanted) const {
    const auto it = std::find_if(effects.begin(), effects.end(),
                                 [wanted](const SoftEffect& effect) { return effect.id == wanted; });
    return it == effects.end() ? nullptr : &*it;
}

SoftEffect* SoftBody::effect(u32 wanted) {
    const auto it = std::find_if(effects.begin(), effects.end(),
                                 [wanted](const SoftEffect& effect) { return effect.id == wanted; });
    return it == effects.end() ? nullptr : &*it;
}

SoftJiggle SoftJigglePresetOf(SoftJigglePreset preset) {
    SoftJiggle out;
    out.preset = preset;
    const auto set = [&](f32 looseness, f32 bounce, f32 weight, f32 holdShape, f32 keepVolume) {
        out.looseness = looseness;
        out.bounce = bounce;
        out.weight = weight;
        out.holdShape = holdShape;
        out.keepVolume = keepVolume;
    };
    switch (preset) {
    case SoftJigglePreset::Jelly:
        set(1.6f, 0.7f, 0.3f, 0.5f, 0.9f);
        break;
    case SoftJigglePreset::Muscle:
        set(4.0f, 0.2f, 0.2f, 0.8f, 0.9f);
        break;
    case SoftJigglePreset::Rubber:
        set(3.0f, 0.8f, 0.2f, 0.9f, 0.9f);
        break;
    case SoftJigglePreset::WaterBalloon:
        set(1.4f, 0.6f, 0.8f, 0.4f, 1.0f);
        break;
    case SoftJigglePreset::Water:
        // Loose and slow to settle, and moving as a few large swings.
        set(0.9f, 0.8f, 0.6f, 0.85f, 0.7f);
        break;
    case SoftJigglePreset::Slime:
        // Loose, and settling at once.
        set(1.0f, 0.0f, 0.7f, 0.2f, 0.8f);
        break;
    case SoftJigglePreset::Fat:
    case SoftJigglePreset::Custom:
    case SoftJigglePreset::Count:
        set(2.2f, 0.35f, 0.5f, 0.6f, 0.8f);
        break;
    }
    return out;
}

} // namespace wem
} // namespace models
} // namespace whiteout
