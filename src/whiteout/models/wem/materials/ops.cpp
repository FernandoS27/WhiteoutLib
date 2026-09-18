// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/materials/ops.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <type_traits>

namespace whiteout {
namespace models {
namespace wem {

namespace {

std::string number(u64 value) {
    return std::to_string(value);
}

/// The set for @p profile, or null with the diagnostic already recorded. Every
/// operation starts here, so the "profile not carried" message is written once.
ProfileMaterialSet* setOrReport(Model& model, ProfileId profile, RemovalResult& result) {
    ProfileMaterialSet* set = model.setFor(profile);
    if (set == nullptr) {
        result.diagnostics.error(DiagCode::ProfileNotCarried,
                                 std::string("the model carries no ") + ToString(profile) +
                                     " material set",
                                 ElementRef(), profile);
    }
    return set;
}

Material* materialOrReport(ProfileMaterialSet& set, u32 material, ProfileId profile,
                           RemovalResult& result) {
    if (material >= set.materials.size()) {
        result.diagnostics.error(DiagCode::IndexOutOfRange,
                                 "material " + number(material) + " of " +
                                     number(set.materials.size()),
                                 ElementRef(ElementKind::Material, material), profile);
        return nullptr;
    }
    return &set.materials[material];
}

/// Erases the layer / stage / slot at @p ordinal from whichever body @p common
/// holds. The four kinds differ only in which vector they keep.
void eraseOrdinal(CommonMaterial& common, u32 ordinal) {
    const std::size_t index = static_cast<std::size_t>(ordinal);
    if (CompositeBody* composite = common.composite()) {
        composite->layers.erase(composite->layers.begin() + index);
    } else if (CombinersBody* combiners = common.combiners()) {
        combiners->stages.erase(combiners->stages.begin() + index);
    } else if (LegacyDeferredBody* legacy = common.legacy()) {
        legacy->slots.erase(legacy->slots.begin() + index);
    } else if (PbrDeferredBody* pbr = common.pbr()) {
        pbr->slots.erase(pbr->slots.begin() + index);
    }
}

/// Whether @p set binds @p material at (@p slot, @p look). A material can be
/// bound at several, which is why the animation fix-ups below iterate the
/// bindings rather than deriving one pair from the material index.
bool bindsMaterial(const ProfileMaterialSet& set, std::size_t slot, std::size_t look,
                   u32 material) {
    return slot < set.slotBindings.size() && look < set.slotBindings[slot].byLook.size() &&
           set.slotBindings[slot].byLook[look] == material;
}

/// Marks @p channel as pointing at nothing and records why. The declaration
/// stays in the table: ids are never reused, and dropping it would leave the
/// sub-tracks that joined on it indistinguishable from a merge that has not
/// landed (§10.8.1).
void invalidate(AnimChannel& channel, const char* why, ProfileId profile, RemovalResult& result) {
    channel.target.material.slot = kInvalidIndex;
    ++result.invalidated;
    result.diagnostics.warn(DiagCode::AnimChannelInvalidated,
                            "channel " + number(channel.id) + " targeted " + why,
                            ElementRef(ElementKind::Channel, channel.id), profile);
}

} // namespace

// ============================================================================
// RemoveMaterial
// ============================================================================

RemovalResult RemoveMaterial(Model& model, ProfileId profile, u32 material) {
    RemovalResult result;
    ProfileMaterialSet* set = setOrReport(model, profile, result);
    if (set == nullptr || materialOrReport(*set, material, profile, result) == nullptr) {
        return result;
    }

    set->materials.erase(set->materials.begin() + static_cast<std::size_t>(material));

    for (std::size_t slot = 0; slot < set->slotBindings.size(); ++slot) {
        std::vector<u32>& byLook = set->slotBindings[slot].byLook;
        for (std::size_t look = 0; look < byLook.size(); ++look) {
            u32& bound = byLook[look];
            if (bound == material) {
                // Invalidated, never repointed: "which material did you mean" is
                // not a question this layer can answer. The coverage rule reports
                // the hole until the caller binds something.
                bound = kInvalidIndex;
                ++result.invalidated;
                result.diagnostics.warn(
                    DiagCode::SlotNotBound,
                    "slot '" + model.materialSlots[slot] + "' look " + number(look) +
                        " referenced the removed material",
                    ElementRef(ElementKind::Slot, static_cast<u32>(slot), static_cast<u32>(look)),
                    profile);
            } else if (bound != kInvalidIndex && bound > material) {
                --bound;
                ++result.rewritten;
            }
        }
    }

    result.removed = true;
    return result;
}

// ============================================================================
// RemoveLayer
// ============================================================================

RemovalResult RemoveLayer(Model& model, ProfileId profile, u32 material, u32 ordinal) {
    RemovalResult result;
    ProfileMaterialSet* set = setOrReport(model, profile, result);
    if (set == nullptr) {
        return result;
    }
    Material* entry = materialOrReport(*set, material, profile, result);
    if (entry == nullptr) {
        return result;
    }
    if (ordinal >= entry->Common().ordinalCount()) {
        result.diagnostics.error(DiagCode::IndexOutOfRange,
                                 "ordinal " + number(ordinal) + " of " +
                                     number(entry->Common().ordinalCount()),
                                 ElementRef(ElementKind::Layer, material, ordinal), profile);
        return result;
    }

    // Through MutableCommon, so the native block goes stale here rather than
    // being written later with a layer count that disagrees (§7.1).
    CommonMaterial& common = entry->MutableCommon();
    eraseOrdinal(common, ordinal);

    std::vector<MaterialFeature> kept;
    kept.reserve(common.features.size());
    for (MaterialFeature& feature : common.features) {
        if (feature.layer == ordinal) {
            ++result.invalidated;
            result.diagnostics.warn(DiagCode::FeatureDropped,
                                    std::string(ToString(feature.kind())) +
                                        " feature was attached to the removed ordinal",
                                    ElementRef(ElementKind::Feature, material, feature.id),
                                    profile);
            continue;
        }
        // Ids are never rewritten — that is what they are for. Only `layer`,
        // which is an ordinal, moves.
        if (feature.layer != kWholeMaterial && feature.layer > ordinal) {
            --feature.layer;
            ++result.rewritten;
        }
        kept.push_back(std::move(feature));
    }
    common.features = std::move(kept);

    // `AnimChannel::target.material` (§7.5). A layer ordinal shifts exactly the
    // way a feature's `layer` does; a feature *id* never does, which is why the
    // target kind distinguishes the two.
    for (AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind != TrackTarget::Kind::MaterialLayer ||
            channel.target.material.profile != profile) {
            continue;
        }
        if (!bindsMaterial(*set, channel.target.material.slot, channel.target.material.look,
                           material)) {
            continue;
        }
        if (channel.target.sub == kWholeMaterial) {
            continue; // Not an ordinal, so nothing renumbers it.
        }
        if (channel.target.sub == ordinal) {
            invalidate(channel, "the removed ordinal", profile, result);
        } else if (channel.target.sub > ordinal) {
            --channel.target.sub;
            ++result.rewritten;
        }
    }

    result.removed = true;
    return result;
}

// ============================================================================
// RemoveFeature
// ============================================================================

RemovalResult RemoveFeature(Model& model, ProfileId profile, u32 material, u32 featureId) {
    RemovalResult result;
    ProfileMaterialSet* set = setOrReport(model, profile, result);
    if (set == nullptr) {
        return result;
    }
    Material* entry = materialOrReport(*set, material, profile, result);
    if (entry == nullptr) {
        return result;
    }

    CommonMaterial& common = entry->MutableCommon();
    for (std::size_t i = 0; i < common.features.size(); ++i) {
        if (common.features[i].id != featureId) {
            continue;
        }
        common.features.erase(common.features.begin() + static_cast<std::ptrdiff_t>(i));

        for (AnimChannel& channel : model.animChannels.channels) {
            if (channel.target.kind != TrackTarget::Kind::MaterialFeature ||
                channel.target.material.profile != profile || channel.target.sub != featureId) {
                continue;
            }
            if (bindsMaterial(*set, channel.target.material.slot, channel.target.material.look,
                              material)) {
                invalidate(channel, "the removed feature", profile, result);
            }
        }

        result.removed = true;
        return result;
    }

    result.diagnostics.error(DiagCode::IndexOutOfRange,
                             "material has no feature with id " + number(featureId),
                             ElementRef(ElementKind::Feature, material, featureId), profile);
    return result;
}

// ============================================================================
// RemoveLook
// ============================================================================

RemovalResult RemoveLook(Model& model, ProfileId profile, u32 look) {
    RemovalResult result;
    ProfileMaterialSet* set = setOrReport(model, profile, result);
    if (set == nullptr) {
        return result;
    }
    if (look >= set->looks.size()) {
        result.diagnostics.error(DiagCode::IndexOutOfRange,
                                 "look " + number(look) + " of " + number(set->looks.size()),
                                 ElementRef(ElementKind::Look, look), profile);
        return result;
    }
    if (set->looks.size() == 1) {
        // "No looks" and "one look" are different shapes, not a degenerate pair:
        // every binding is sized to the table, so an empty table has nothing to
        // resolve through.
        result.diagnostics.error(DiagCode::IndexOutOfRange,
                                 "a material set always carries at least one look",
                                 ElementRef(ElementKind::Look, look), profile);
        return result;
    }

    set->looks.looks.erase(set->looks.looks.begin() + static_cast<std::size_t>(look));
    for (SlotBinding& binding : set->slotBindings) {
        if (look < binding.byLook.size()) {
            binding.byLook.erase(binding.byLook.begin() + static_cast<std::size_t>(look));
            ++result.rewritten;
        }
    }

    for (AnimChannel& channel : model.animChannels.channels) {
        if (!IsMaterialTarget(channel.target.kind) || channel.target.material.profile != profile) {
            continue;
        }
        if (channel.target.material.look == look) {
            invalidate(channel, "the removed look", profile, result);
        } else if (channel.target.material.look > look) {
            --channel.target.material.look;
            ++result.rewritten;
        }
    }

    result.removed = true;
    return result;
}

// ============================================================================
// RemoveProfileSet
// ============================================================================

RemovalResult RemoveProfileSet(Model& model, ProfileId profile) {
    RemovalResult result;
    if (model.setFor(profile) == nullptr) {
        setOrReport(model, profile, result);
        return result;
    }

    for (std::size_t i = 0; i < model.profileSets.size(); ++i) {
        if (model.profileSets[i].profile == profile) {
            model.profileSets.erase(model.profileSets.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }

    // A section still claiming the profile would be a coverage hole by
    // construction, so the bit goes with the set.
    const ProfileMask bit = ProfileBit(profile);
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        Mesh& mesh = model.meshes[m];
        for (std::size_t s = 0; s < mesh.sections.size(); ++s) {
            MeshSection& section = mesh.sections[s];
            if ((section.profiles & bit) == 0) {
                continue;
            }
            section.profiles &= ~bit;
            ++result.rewritten;
            if (section.profiles == kNoProfiles) {
                // Reported, not removed: dropping geometry is not a side effect
                // this operation gets to have.
                ++result.invalidated;
                result.diagnostics.warn(
                    DiagCode::SectionUndrawn,
                    "section '" + section.name + "' is now drawn by no profile",
                    ElementRef(ElementKind::Section, static_cast<u32>(s), static_cast<u32>(m)),
                    profile);
            }
        }
    }

    result.removed = true;
    return result;
}


// ============================================================================
// RemoveSlot
// ============================================================================

namespace {

/// Whether @p set binds @p slot at every look.
bool boundEverywhere(const ProfileMaterialSet& set, u32 slot) {
    if (slot >= set.slotBindings.size() || set.slotBindings[slot].byLook.empty()) {
        return false;
    }
    for (const u32 material : set.slotBindings[slot].byLook) {
        if (material == kInvalidIndex) {
            return false;
        }
    }
    return true;
}

bool bindsAnywhere(const ProfileMaterialSet& set, u32 material) {
    for (const SlotBinding& binding : set.slotBindings) {
        for (const u32 bound : binding.byLook) {
            if (bound == material) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool SlotCoversSection(const Model& model, const MeshSection& section, u32 slot,
                       ProfileId* missing) {
    for (const ProfileMaterialSet& set : model.profileSets) {
        if ((section.profiles & ProfileBit(set.profile)) != 0 && !boundEverywhere(set, slot)) {
            if (missing != nullptr) {
                *missing = set.profile;
            }
            return false;
        }
    }
    return true;
}

bool SlotCanReplace(const Model& model, u32 slot, u32 replacement, std::string* why) {
    const u32 slots = static_cast<u32>(model.materialSlots.size());
    const auto refuse = [&](const std::string& reason) {
        if (why != nullptr) {
            *why = reason;
        }
        return false;
    };
    if (slot >= slots) {
        return refuse("slot " + number(slot) + " of " + number(slots));
    }
    const bool named = replacement != kInvalidIndex && replacement < slots && replacement != slot;
    for (const Mesh& mesh : model.meshes) {
        for (const MeshSection& section : mesh.sections) {
            if (section.materialSlot != slot) {
                continue;
            }
            if (!named) {
                return refuse("section '" + section.name + "' uses slot '" +
                              model.materialSlots[slot] + "' and no other slot was named for it");
            }
            ProfileId missing = ProfileId::Generic;
            if (!SlotCoversSection(model, section, replacement, &missing)) {
                return refuse("slot '" + model.materialSlots[replacement] + "' is not bound in " +
                              ToString(missing) + ", which section '" + section.name +
                              "' is drawn in");
            }
        }
    }
    for (const Node& node : model.nodes.nodes) {
        bool linked = false;
        ForEachMaterialLink(node.payload, [&](const u32& link) { linked |= link == slot; });
        if (!linked) {
            continue;
        }
        if (!named) {
            return refuse("node '" + node.name + "' uses slot '" + model.materialSlots[slot] +
                          "' and no other slot was named for it");
        }
        // A node draws in whatever set draws the slot it names.
        for (const ProfileMaterialSet& set : model.profileSets) {
            if (boundEverywhere(set, slot) && !boundEverywhere(set, replacement)) {
                return refuse("slot '" + model.materialSlots[replacement] + "' is not bound in " +
                              ToString(set.profile) + ", which node '" + node.name +
                              "' draws in");
            }
        }
    }
    return true;
}

RemovalResult RemoveSlot(Model& model, u32 slot, u32 replacement) {
    RemovalResult result;
    const u32 slots = static_cast<u32>(model.materialSlots.size());
    if (slot >= slots) {
        result.diagnostics.error(DiagCode::IndexOutOfRange,
                                 "slot " + number(slot) + " of " + number(slots),
                                 ElementRef(ElementKind::Slot, slot));
        return result;
    }

    // --- the users, and whether the replacement covers them -------------------
    std::string why;
    if (!SlotCanReplace(model, slot, replacement, &why)) {
        result.diagnostics.error(DiagCode::SlotNotBound, why, ElementRef(ElementKind::Slot, slot));
        return result;
    }

    const auto renumber = [&](u32 index) {
        if (index == kInvalidIndex || index < slot) {
            return index;
        }
        return index == slot ? kInvalidIndex : index - 1;
    };

    // --- the users take the replacement ----------------------------------------
    for (Mesh& mesh : model.meshes) {
        for (MeshSection& section : mesh.sections) {
            if (section.materialSlot == slot) {
                section.materialSlot = replacement;
            }
            const u32 to = renumber(section.materialSlot);
            if (to != section.materialSlot) {
                section.materialSlot = to;
                ++result.rewritten;
            }
        }
    }
    for (Node& node : model.nodes.nodes) {
        ForEachMaterialLink(node.payload, [&](u32& link) {
            if (link == slot) {
                link = replacement;
            }
            const u32 to = renumber(link);
            if (to != link) {
                link = to;
                ++result.rewritten;
            }
        });
    }

    // --- the channels: they animated this slot, and die with it ---------------
    for (AnimChannel& channel : model.animChannels.channels) {
        if (!IsMaterialTarget(channel.target.kind)) {
            continue;
        }
        u32& target = channel.target.material.slot;
        if (target == slot) {
            invalidate(channel, "the removed slot", channel.target.material.profile, result);
        } else if (target != kInvalidIndex && target > slot) {
            --target;
            ++result.rewritten;
        }
    }

    // --- every set: the binding, and a material nothing else binds -------------
    for (ProfileMaterialSet& set : model.profileSets) {
        if (slot >= set.slotBindings.size()) {
            continue;
        }
        const std::vector<u32> bound = set.slotBindings[slot].byLook;
        set.slotBindings.erase(set.slotBindings.begin() + static_cast<std::ptrdiff_t>(slot));
        // Highest first, so an earlier removal cannot renumber a later one.
        std::vector<u32> orphans;
        for (const u32 material : bound) {
            if (material != kInvalidIndex && material < set.materials.size() &&
                !bindsAnywhere(set, material) &&
                std::find(orphans.begin(), orphans.end(), material) == orphans.end()) {
                orphans.push_back(material);
            }
        }
        std::sort(orphans.rbegin(), orphans.rend());
        for (const u32 material : orphans) {
            RemovalResult removed = RemoveMaterial(model, set.profile, material);
            result.rewritten += removed.rewritten;
        }
    }

    model.materialSlots.erase(model.materialSlots.begin() + static_cast<std::ptrdiff_t>(slot));
    result.remap.resize(slots);
    for (u32 i = 0; i < slots; ++i) {
        result.remap[i] = renumber(i);
    }
    result.removed = true;
    return result;
}

// ============================================================================
// RemoveTexture, CheckTextureReferencers — the §7.4 table
// ============================================================================

namespace {

/// Every texture index in @p document the §7.4 table lists, as `f(u32&)`.
/// One walk for the removal and the check, so the two cannot list different
/// rows. @p f also receives what the row is, for the check's message.
enum class TextureRow : u8 { CommonInput, NativeLayer, PayloadLink, FlipbookKey };

/// What one call of the walker is visiting. `mirrored` marks a common input of
/// a material whose native block names the same texture on the same layer: a
/// rewrite must reach both, a count must not see one reference twice.
struct TextureReferencer {
    TextureRow row;
    ElementRef where;
    bool mirrored = false;
};

template <class DocumentT, class F>
void forEachTextureReferencer(DocumentT& document, F&& f) {
    for (std::size_t m = 0; m < document.models.size(); ++m) {
        auto& model = document.models[m];
        for (auto& set : model.profileSets) {
            for (auto& material : set.materials) {
                // Through `InitCommon` on the mutable walk: the removal renumbers
                // the common and the block alike, so neither goes stale (§7.1).
                auto& common = [&]() -> auto& {
                    if constexpr (std::is_const_v<DocumentT>) {
                        return material.Common();
                    } else {
                        return material.InitCommon();
                    }
                }();
                const u32 index = static_cast<u32>(&material - set.materials.data());
                const native::MdxMaterial* block =
                    std::get_if<native::MdxMaterial>(&material.Native());
                for (u32 ordinal = 0; ordinal < common.ordinalCount(); ++ordinal) {
                    if (auto* input = common.inputAt(ordinal)) {
                        f(input->texture,
                          TextureReferencer{TextureRow::CommonInput,
                                            ElementRef(ElementKind::Layer, index, ordinal),
                                            block != nullptr});
                    }
                }
                if (block == nullptr) {
                    continue;
                }
                const TextureReferencer where{TextureRow::NativeLayer,
                                              ElementRef(ElementKind::Material, index)};
                if constexpr (std::is_const_v<DocumentT>) {
                    for (const native::MdxLayer& layer : block->layers) {
                        if (layer.subTextures.empty()) {
                            f(layer.textureId, where);
                        }
                        for (const native::MdxSubTexture& sub : layer.subTextures) {
                            f(sub.textureId, where);
                        }
                    }
                } else {
                    native::MdxMaterial edited = *block;
                    for (native::MdxLayer& layer : edited.layers) {
                        if (layer.subTextures.empty()) {
                            f(layer.textureId, where);
                        }
                        for (native::MdxSubTexture& sub : layer.subTextures) {
                            f(sub.textureId, where);
                        }
                    }
                    // The sync state is carried over: renumbering both halves
                    // together edits neither out of step with the other.
                    const NativeSync sync = material.sync();
                    if (sync == NativeSync::NativeAuthoritative) {
                        material.SetNativeAuthoritative(std::move(edited));
                    } else {
                        material.SetNativeInSync(std::move(edited));
                        if (sync == NativeSync::CommonEdited) {
                            material.MutableCommon();
                        }
                    }
                }
            }
        }
        for (std::size_t n = 0; n < model.nodes.nodes.size(); ++n) {
            ForEachTextureLink(model.nodes.nodes[n].payload, [&](auto& texture) {
                f(texture, TextureReferencer{TextureRow::PayloadLink,
                                             ElementRef(ElementKind::Node, static_cast<u32>(n))});
            });
        }
        // KMTF: a Warcraft III flipbook's keys are texture indices. A ribbon's
        // KRTX is a Node target and a cell of its own grid, and an `.m3`
        // layer's `TextureIndex` is its `currentFrame` — a frame of that
        // layer's own atlas. Both stay out.
        for (auto& channel : model.animChannels.channels) {
            const ProfileId profile = channel.target.material.profile;
            if (channel.target.kind != TrackTarget::Kind::MaterialLayer ||
                channel.target.channel != Channel::TextureIndex ||
                (profile != ProfileId::Wc3Classic && profile != ProfileId::Wc3Reforged) ||
                channel.valueType != geom::AttrType::U32) {
                continue;
            }
            const TextureReferencer where{TextureRow::FlipbookKey,
                                          ElementRef(ElementKind::Channel, channel.id)};
            const auto each = [&](auto& bytes) {
                for (std::size_t at = 0; at + sizeof(u32) <= bytes.size(); at += sizeof(u32)) {
                    u32 value = 0;
                    std::memcpy(&value, bytes.data() + at, sizeof(u32));
                    const u32 before = value;
                    f(value, where);
                    if constexpr (!std::is_const_v<DocumentT>) {
                        if (value != before) {
                            std::memcpy(bytes.data() + at, &value, sizeof(u32));
                        }
                    }
                }
            };
            each(channel.initValue);
            for (auto& clip : document.clips) {
                if (clip.model != m) {
                    continue;
                }
                for (auto& container : clip.containers) {
                    for (auto& track : container.subTracks) {
                        if (track.channel == channel.id) {
                            each(track.values);
                        }
                    }
                }
            }
        }
    }
}

/// A set holding a block the §7.4 table has no rows for, or null.
const ProfileMaterialSet* unauditedSet(const Document& document) {
    for (const Model& model : document.models) {
        for (const ProfileMaterialSet& set : model.profileSets) {
            for (const Material& material : set.materials) {
                const NativeKind kind = material.nativeKind();
                if (kind != NativeKind::None && kind != NativeKind::Mdx) {
                    return &set;
                }
            }
        }
    }
    return nullptr;
}

const char* rowName(TextureRow row) {
    switch (row) {
    case TextureRow::CommonInput:
        return "a material input";
    case TextureRow::NativeLayer:
        return "an MDX layer";
    case TextureRow::PayloadLink:
        return "an emitter";
    case TextureRow::FlipbookKey:
        return "a flipbook key";
    }
    return "?";
}

} // namespace

RemovalResult RemoveTexture(Document& document, u32 texture, u32 replacement) {
    RemovalResult result;
    const u32 count = static_cast<u32>(document.textures.size());
    if (texture >= count) {
        result.diagnostics.error(DiagCode::IndexOutOfRange,
                                 "texture " + number(texture) + " of " + number(count),
                                 ElementRef(ElementKind::Texture, texture));
        return result;
    }
    if (const ProfileMaterialSet* set = unauditedSet(document)) {
        result.diagnostics.error(DiagCode::OperationUnsupported,
                                 std::string("a ") + ToString(set->profile) +
                                     " material holds a native block whose texture references "
                                     "this operation does not know",
                                 ElementRef(ElementKind::Texture, texture), set->profile);
        return result;
    }

    u32 users = 0;
    forEachTextureReferencer(static_cast<const Document&>(document),
                             [&](const u32& index, const TextureReferencer&) {
                                 users += index == texture;
                             });
    const bool named = replacement != kInvalidIndex && replacement < count && replacement != texture;
    if (users != 0 && !named) {
        result.diagnostics.error(DiagCode::TextureUnresolved,
                                 number(users) + " reference(s) name texture " + number(texture) +
                                     " and no other texture was named for them",
                                 ElementRef(ElementKind::Texture, texture));
        return result;
    }

    forEachTextureReferencer(document, [&](u32& index, const TextureReferencer&) {
        if (index == texture) {
            index = replacement;
            ++result.rewritten;
        }
        // `kInvalidIndex` is also MDX's "no texture" (-1), and stays.
        if (index != kInvalidIndex && index > texture) {
            --index;
            ++result.rewritten;
        }
    });

    document.textures.erase(document.textures.begin() + static_cast<std::ptrdiff_t>(texture));
    result.remap.resize(count);
    for (u32 i = 0; i < count; ++i) {
        result.remap[i] = i < texture ? i : (i == texture ? kInvalidIndex : i - 1);
    }
    result.removed = true;
    return result;
}

void CheckTextureReferencers(const Document& document, Diagnostics& out) {
    const u32 count = static_cast<u32>(document.textures.size());
    forEachTextureReferencer(document, [&](const u32& index, const TextureReferencer& ref) {
        if (index == kInvalidIndex || index < count) {
            return;
        }
        out.error(DiagCode::IndexOutOfRange,
                  std::string(rowName(ref.row)) + " names texture " + number(index) + " of " +
                      number(count),
                  ref.where);
    });
}

TextureReferencerCount CountTextureReferencers(const Document& document, u32 texture) {
    TextureReferencerCount count;
    forEachTextureReferencer(document, [&](const u32& index, const TextureReferencer& ref) {
        if (index != texture || ref.mirrored) {
            return;
        }
        switch (ref.row) {
        case TextureRow::CommonInput:
        case TextureRow::NativeLayer:
            ++count.materialLayers;
            break;
        case TextureRow::PayloadLink:
            ++count.payloadLinks;
            break;
        case TextureRow::FlipbookKey:
            ++count.flipbookKeys;
            break;
        }
    });
    return count;
}

// ============================================================================
// CheckMaterialReferencers
// ============================================================================

void CheckMaterialReferencers(const Model& model, u32 modelIndex, Diagnostics& out) {
    (void)modelIndex;

    for (const ProfileMaterialSet& set : model.profileSets) {
        // --- SlotBinding -> materials -----------------------------------------
        for (std::size_t slot = 0; slot < set.slotBindings.size(); ++slot) {
            const std::vector<u32>& byLook = set.slotBindings[slot].byLook;
            for (std::size_t look = 0; look < byLook.size(); ++look) {
                const u32 material = byLook[look];
                if (material == kInvalidIndex || material < set.materials.size()) {
                    continue;
                }
                out.error(
                    DiagCode::IndexOutOfRange,
                    "binding names material " + number(material) + " of " +
                        number(set.materials.size()),
                    ElementRef(ElementKind::Slot, static_cast<u32>(slot), static_cast<u32>(look)),
                    set.profile);
            }
        }

        // --- MaterialFeature -> ordinal ---------------------------------------
        for (std::size_t m = 0; m < set.materials.size(); ++m) {
            const CommonMaterial& common = set.materials[m].Common();
            const u32 ordinals = common.ordinalCount();
            for (const MaterialFeature& feature : common.features) {
                if (feature.layer == kWholeMaterial || feature.layer < ordinals) {
                    continue;
                }
                out.error(DiagCode::IndexOutOfRange,
                          std::string(ToString(feature.kind())) + " feature targets ordinal " +
                              number(feature.layer) + " of " + number(ordinals),
                          ElementRef(ElementKind::Feature, static_cast<u32>(m), feature.id),
                          set.profile);
            }
        }
    }

    // --- MeshSection -> materialSlots -----------------------------------------
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        for (std::size_t s = 0; s < mesh.sections.size(); ++s) {
            if (mesh.sections[s].materialSlot < model.materialSlots.size()) {
                continue;
            }
            out.error(DiagCode::IndexOutOfRange,
                      "section names material slot " + number(mesh.sections[s].materialSlot) +
                          " of " + number(model.materialSlots.size()),
                      ElementRef(ElementKind::Section, static_cast<u32>(s), static_cast<u32>(m)));
        }
    }

    // --- Emitter payload -> materialSlots (§10.9) -----------------------------
    for (std::size_t n = 0; n < model.nodes.nodes.size(); ++n) {
        ForEachMaterialLink(model.nodes.nodes[n].payload, [&](const u32& slot) {
            if (slot == kInvalidIndex || slot < model.materialSlots.size()) {
                return;
            }
            out.error(DiagCode::IndexOutOfRange,
                      "emitter names material slot " + number(slot) + " of " +
                          number(model.materialSlots.size()),
                      ElementRef(ElementKind::Node, static_cast<u32>(n)));
        });
    }

    // --- AnimChannel -> target.material ---------------------------------------
    //
    // P6 settled that there is no `Actor` row: the default look is a field on the
    // set and the range check for it lives in `Validate`.
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (!IsMaterialTarget(channel.target.kind)) {
            continue;
        }
        const MaterialChannelRef& ref = channel.target.material;
        const ElementRef where(ElementKind::Channel, channel.id);

        const ProfileMaterialSet* set = model.setFor(ref.profile);
        if (set == nullptr) {
            out.error(DiagCode::ProfileNotCarried,
                      "channel " + number(channel.id) + " drives a " + ToString(ref.profile) +
                          " material and the model carries no such set",
                      where, ref.profile);
            continue;
        }
        if (ref.slot >= model.materialSlots.size()) {
            out.warn(DiagCode::AnimChannelInvalidated,
                     "channel " + number(channel.id) + " names material slot " + number(ref.slot) +
                         " of " + number(model.materialSlots.size()),
                     where, ref.profile);
            continue;
        }
        if (ref.look >= set->looks.size()) {
            out.error(DiagCode::IndexOutOfRange,
                      "channel " + number(channel.id) + " names look " + number(ref.look) + " of " +
                          number(set->looks.size()),
                      where, ref.profile);
            continue;
        }

        // The ordinal is the half that shifts under editing, so it is the half
        // worth checking against the material it actually resolves to.
        if (channel.target.kind != TrackTarget::Kind::MaterialLayer) {
            continue;
        }
        // `kWholeMaterial` is a legal ordinal here for the same reason it is on
        // a feature: a WoW `M2Color` multiplies the whole batch, not one stage.
        if (channel.target.sub == kWholeMaterial) {
            continue;
        }
        const Material* material = Resolve(model, ref.slot, ref.profile, ref.look);
        if (material != nullptr && channel.target.sub >= material->Common().ordinalCount()) {
            out.error(DiagCode::IndexOutOfRange,
                      "channel " + number(channel.id) + " targets ordinal " +
                          number(channel.target.sub) + " of " +
                          number(material->Common().ordinalCount()),
                      where, ref.profile);
        }
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout
