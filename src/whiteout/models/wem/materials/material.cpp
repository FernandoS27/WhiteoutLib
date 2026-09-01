// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/materials/looks.h>
#include <whiteout/models/wem/materials/material.h>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// NativeSync
// ============================================================================

const char* ToString(NativeSync sync) {
    switch (sync) {
    case NativeSync::Absent:
        return "absent";
    case NativeSync::InSync:
        return "in_sync";
    case NativeSync::NativeAuthoritative:
        return "native_authoritative";
    case NativeSync::CommonEdited:
        return "common_edited";
    }
    return "invalid";
}

// ============================================================================
// Material
// ============================================================================

CommonMaterial& Material::MutableCommon() {
    // Both transitions, not just the InSync one: an edit to a lossy view still
    // makes the view the newest truth.
    if (sync_ == NativeSync::InSync || sync_ == NativeSync::NativeAuthoritative) {
        sync_ = NativeSync::CommonEdited;
    }
    return common_;
}

void Material::SetNativeInSync(NativeMaterial native) {
    native_ = std::move(native);
    sync_ = hasNative() ? NativeSync::InSync : NativeSync::Absent;
}

void Material::SetNativeAuthoritative(NativeMaterial native) {
    native_ = std::move(native);
    sync_ = hasNative() ? NativeSync::NativeAuthoritative : NativeSync::Absent;
}

void Material::ClearNative() {
    native_ = NativeMaterial{};
    sync_ = NativeSync::Absent;
}

// ============================================================================
// LookTable
// ============================================================================

u32 LookTable::find(const std::string& name) const {
    for (std::size_t i = 0; i < looks.size(); ++i) {
        if (looks[i].name == name) {
            return static_cast<u32>(i);
        }
    }
    return kInvalidIndex;
}

u32 LookTable::add(const std::string& name, i32 weight) {
    Look look;
    look.name = name;
    look.weight = weight;
    looks.push_back(std::move(look));
    return static_cast<u32>(looks.size() - 1);
}

LookTable LookTable::Single() {
    LookTable table;
    table.looks.resize(1);
    return table;
}

} // namespace wem
} // namespace models
} // namespace whiteout
