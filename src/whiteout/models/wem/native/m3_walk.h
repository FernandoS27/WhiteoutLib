// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// Every value a StarCraft II mirror holds of the kinds a caller names, reached
/// through the mirrors' own `reflect`: a merge renumbers the AnimRefs a block
/// names its streams by, and the texture walker reads the paths its layers
/// name. The mirrors reflect with `field`, `optional` and `inlineList` alone.

#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

#include <whiteout/models/wem/native/m3_material.h>
#include <whiteout/models/wem/nodes/node.h>

namespace whiteout {
namespace models {
namespace wem {
namespace native {

template <class F>
struct M3Walker {
    F& f;

    template <class U>
    M3Walker& field(const char*, U& value) {
        visit(value);
        return *this;
    }
    template <class U>
    M3Walker& optional(const char*, std::optional<U>& value) {
        if (value) {
            visit(*value);
        }
        return *this;
    }
    template <class U>
    M3Walker& inlineList(const char*, std::vector<U>& values) {
        for (U& value : values) {
            visit(value);
        }
        return *this;
    }
    M3Walker& since(u32) {
        return *this;
    }

    template <class U>
    void visit(U& value) {
        if constexpr (std::is_invocable_v<F&, U&>) {
            f(value);
        } else if constexpr (requires { value.reflect(*this); }) {
            value.reflect(*this);
        }
    }
};

/// @p f over everything in @p block: its body and the standard material a
/// `MADD` restores. `M3Material::reflect` is a variant's, so it is not walked.
template <class F>
void WalkM3Material(M3Material& block, F&& f) {
    M3Walker<std::remove_reference_t<F>> walker{f};
    std::visit([&](auto& body) { walker.visit(body); }, block.body);
    if (block.restoredStandard) {
        walker.visit(*block.restoredStandard);
    }
}

/// @p f over the StarCraft II mirrors a node keeps: its payload's (`LITE`,
/// `CAM_`, `PROJ`, `SSGS`, `ATVL`) and its shadow box.
template <class F>
void WalkM3NodeRecords(Node& node, F&& f) {
    M3Walker<std::remove_reference_t<F>> walker{f};
    if (auto* light = std::get_if<LightPayload>(&node.payload); light != nullptr && light->sc2) {
        walker.visit(*light->sc2);
    } else if (auto* camera = std::get_if<CameraPayload>(&node.payload); camera != nullptr && camera->sc2) {
        walker.visit(*camera->sc2);
    } else if (auto* projector = std::get_if<ProjectorPayload>(&node.payload)) {
        walker.visit(projector->record);
    } else if (auto* collision = std::get_if<CollisionPayload>(&node.payload);
               collision != nullptr && collision->shape.sc2) {
        walker.visit(*collision->shape.sc2);
    } else if (auto* attachment = std::get_if<AttachmentPayload>(&node.payload)) {
        for (M3AttachmentVolume& volume : attachment->sc2Volumes) {
            walker.visit(volume);
        }
    }
    if (node.sc2ShadowBox) {
        walker.visit(*node.sc2ShadowBox);
    }
}

} // namespace native
} // namespace wem
} // namespace models
} // namespace whiteout
