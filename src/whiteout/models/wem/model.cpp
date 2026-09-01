// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/model.h>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// ProfileMaterialSet
// ============================================================================

void ProfileMaterialSet::resizeBindings(std::size_t slotCount) {
    if (looks.empty()) {
        looks = LookTable::Single();
    }
    slotBindings.resize(slotCount);
    for (SlotBinding& binding : slotBindings) {
        binding.byLook.resize(looks.size(), kInvalidIndex);
    }
}

// ============================================================================
// Model
// ============================================================================

const ProfileMaterialSet* Model::setFor(ProfileId profile) const {
    for (const ProfileMaterialSet& set : profileSets) {
        if (set.profile == profile) {
            return &set;
        }
    }
    return nullptr;
}

ProfileMaterialSet* Model::setFor(ProfileId profile) {
    return const_cast<ProfileMaterialSet*>(static_cast<const Model*>(this)->setFor(profile));
}

u32 Model::slotIndex(const std::string& name) const {
    for (std::size_t i = 0; i < materialSlots.size(); ++i) {
        if (materialSlots[i] == name) {
            return static_cast<u32>(i);
        }
    }
    return kInvalidIndex;
}

u32 Model::addSlot(const std::string& name) {
    const u32 existing = slotIndex(name);
    if (existing != kInvalidIndex) {
        return existing;
    }
    materialSlots.push_back(name);
    const u32 index = static_cast<u32>(materialSlots.size() - 1);
    // The parallel-array invariant is the set's, so grow every set with the slot
    // rather than leaving them to notice later.
    for (ProfileMaterialSet& set : profileSets) {
        set.resizeBindings(materialSlots.size());
    }
    return index;
}

ProfileMask Model::drawnProfiles() const {
    ProfileMask mask = kNoProfiles;
    for (const Mesh& mesh : meshes) {
        for (const MeshSection& section : mesh.sections) {
            mask |= section.profiles;
        }
    }
    return mask;
}

const Material* Resolve(const Model& model, u32 slot, ProfileId profile, u32 look) {
    const ProfileMaterialSet* set = model.setFor(profile);
    if (set == nullptr || slot >= set->slotBindings.size()) {
        return nullptr;
    }
    const SlotBinding& binding = set->slotBindings[slot];
    if (look >= binding.byLook.size()) {
        return nullptr;
    }
    const u32 material = binding.byLook[look];
    if (material >= set->materials.size()) {
        return nullptr;
    }
    return &set->materials[material];
}

// ============================================================================
// Document
// ============================================================================

bool Document::carries(ProfileId profile) const {
    for (ProfileId declared : profiles) {
        if (declared == profile) {
            return true;
        }
    }
    return false;
}

void Document::declare(ProfileId profile) {
    if (!carries(profile)) {
        profiles.push_back(profile);
    }
}

ProfileMask Document::declaredMask() const {
    ProfileMask mask = kNoProfiles;
    for (ProfileId declared : profiles) {
        mask |= ProfileBit(declared);
    }
    return mask;
}

} // namespace wem
} // namespace models
} // namespace whiteout
