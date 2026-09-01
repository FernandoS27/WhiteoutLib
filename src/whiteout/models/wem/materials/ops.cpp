// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/materials/ops.h>

#include <string>

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

    // P6 adds the `Actor` row (defaultLook, ActorEvent::lookName); P7 adds
    // `AnimChannel::target.material`. Each is one loop here, not one mechanism.
}

} // namespace wem
} // namespace models
} // namespace whiteout
