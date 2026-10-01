// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "export_sections.h"

#include <algorithm>

namespace whiteout {
namespace models {
namespace wem {

namespace {

void Note(std::vector<std::pair<u64, f32>>& map, u64 key, f32 alpha, bool takeMax = false) {
    for (auto& entry : map) {
        if (entry.first == key) {
            if (takeMax) {
                entry.second = std::max(entry.second, alpha);
            }
            return;
        }
    }
    map.emplace_back(key, alpha);
}

f32 Lookup(const std::vector<std::pair<u64, f32>>& map, u64 key) {
    for (const auto& entry : map) {
        if (entry.first == key) {
            return entry.second;
        }
    }
    return 1.0f;
}

} // namespace

f32 EvalScalarTrack(const SubTrack& track, f32 time) {
    if (track.times.empty()) {
        return 1.0f;
    }
    const u32 perKey = ValuesPerKey(track.interp);
    const f32* values = reinterpret_cast<const f32*>(track.values.data());
    if (time <= track.times.front()) {
        return values[0];
    }
    std::size_t k = 0;
    while (k + 1 < track.times.size() && track.times[k + 1] <= time) {
        ++k;
    }
    if (k + 1 >= track.times.size() || track.interp == Interpolation::Step) {
        return values[k * perKey];
    }
    const f32 span = track.times[k + 1] - track.times[k];
    const f32 u = span > 1e-9f ? (time - track.times[k]) / span : 0.0f;
    return values[k * perKey] * (1.0f - u) + values[(k + 1) * perKey] * u;
}

DefaultLookAlpha::DefaultLookAlpha(const Document& document, const Model& model, u32 modelIndex,
                                   ProfileId profile, u32 look) {
    const Clip* clip = nullptr;
    for (const Clip& candidate : document.clips) {
        if (candidate.model != modelIndex || hasFlag(candidate.flags, ClipFlags::AutoPlay)) {
            continue;
        }
        bool stand = candidate.name.size() >= 5;
        for (std::size_t i = 0; stand && i < 5; ++i) {
            const char c = candidate.name[i];
            stand = (c | 0x20) == "stand"[i];
        }
        if (stand) {
            clip = &candidate;
            break;
        }
        if (clip == nullptr) {
            clip = &candidate;
        }
    }
    if (clip == nullptr) {
        return;
    }

    // Containers flatten by priority, the same rule the animation export
    // applies: the highest priority that keys a channel speaks for it.
    std::vector<u32> order(clip->containers.size());
    for (u32 i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        return clip->containers[a].priority > clip->containers[b].priority;
    });
    std::vector<u32> seen;
    for (const u32 containerIndex : order) {
        for (const SubTrack& track : clip->containers[containerIndex].subTracks) {
            if (std::find(seen.begin(), seen.end(), track.channel) != seen.end()) {
                continue;
            }
            seen.push_back(track.channel);
            const AnimChannel* channel = model.animChannels.find(track.channel);
            if (channel == nullptr || channel->target.channel != Channel::Alpha ||
                channel->valueType != geom::AttrType::F32 || !track.wellSized(channel->valueType) ||
                track.times.empty()) {
                continue;
            }
            // The larger of start and midpoint: a fade-in still counts as
            // shown, a sequence-scoped zero still counts as hidden.
            const f32 alpha = std::max(EvalScalarTrack(track, 0.0f),
                                       EvalScalarTrack(track, clip->duration * 0.5f));
            if (channel->target.kind == TrackTarget::Kind::Section) {
                Note(sections_,
                     (static_cast<u64>(channel->target.mesh) << 32) | channel->target.sub, alpha);
            } else if (channel->target.kind == TrackTarget::Kind::MaterialLayer &&
                       channel->target.material.profile == profile &&
                       channel->target.material.look == look) {
                // Layers stack, so the slot shows if any layer does — max
                // across the ordinals.
                Note(slots_, channel->target.material.slot, alpha, true);
            }
        }
    }
}

f32 DefaultLookAlpha::sectionAlpha(u32 mesh, u32 section) const {
    return Lookup(sections_, (static_cast<u64>(mesh) << 32) | section);
}

f32 DefaultLookAlpha::slotAlpha(u32 slot) const {
    return Lookup(slots_, slot);
}

SectionSkip SkipSection(const Mesh& mesh, u32 meshOrdinal, u32 section, u32 materialSlot,
                        ProfileId profile, const SlotDrawState* slot,
                        const DefaultLookAlpha* defaultLook) {
    if (section < mesh.sections.size() && !HasProfile(mesh.sections[section].profiles, profile)) {
        return SectionSkip::NotInProfile;
    }
    if (section < mesh.sections.size() &&
        hasFlag(mesh.sections[section].flags, SectionFlags::Hidden)) {
        return SectionSkip::Hidden;
    }
    if (slot != nullptr && slot->invisible) {
        return SectionSkip::Invisible;
    }
    if (slot != nullptr && slot->gameComposited) {
        return SectionSkip::GameComposited;
    }
    if (defaultLook != nullptr &&
        defaultLook->sectionAlpha(meshOrdinal, section) * defaultLook->slotAlpha(materialSlot) <
            0.02f) {
        return SectionSkip::RestHidden;
    }
    return SectionSkip::None;
}

void SectionSkipCounts::count(SectionSkip skip) {
    switch (skip) {
    case SectionSkip::NotInProfile:
    case SectionSkip::Hidden:
        ++undrawn;
        break;
    case SectionSkip::Invisible:
        ++invisible;
        break;
    case SectionSkip::GameComposited:
        ++composited;
        break;
    case SectionSkip::RestHidden:
        ++restHidden;
        break;
    case SectionSkip::None:
        break;
    }
}

void SectionSkipCounts::report(Diagnostics& diagnostics, const std::string& meshName,
                               u32 meshOrdinal, ProfileId profile) const {
    const ElementRef where(ElementKind::Mesh, meshOrdinal);
    if (invisible != 0) {
        diagnostics.info(DiagCode::SectionUndrawn,
                         "mesh '" + meshName + "': " + std::to_string(invisible) +
                             " section(s) bound to invisible materials, skipped",
                         where, profile);
    }
    if (composited != 0) {
        diagnostics.info(DiagCode::SectionUndrawn,
                         "mesh '" + meshName + "': " + std::to_string(composited) +
                             " section(s) coloured only by game-composited textures (team "
                             "colour/glow), skipped",
                         where, profile);
    }
    if (restHidden != 0) {
        diagnostics.info(DiagCode::SectionUndrawn,
                         "mesh '" + meshName + "': " + std::to_string(restHidden) +
                             " section(s) alpha-keyed invisible in the default look, skipped",
                         where, profile);
    }
    if (undrawn != 0) {
        diagnostics.info(DiagCode::Unspecified,
                         "mesh '" + meshName + "': " + std::to_string(undrawn) +
                             " section(s) not drawn by profile " + ToString(profile) +
                             ", skipped",
                         where, profile);
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout
