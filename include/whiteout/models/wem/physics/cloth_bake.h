// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file cloth_bake.h
 * @brief A cloth's simulation baked without new nodes in the scene
 *        (EDIT_MODE_PHYSICS_CLOTH_DESIGN.md §10).
 *
 * *Into its bones* (§10.2) is SSDR's transform step with the weights fixed,
 * Dem Bones' transforms-only mode: the drawn faces' points, where the
 * simulation puts them, are the targets; every bone but the cloth's stays
 * where the animation has it; each cloth bone, root to tip and twice over,
 * takes the rigid motion that best carries its share of the points to theirs
 * (weighted Procrustes). Its T and R are written; its scale is the
 * animation's.
 */

#include <array>
#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../anim/animator.h"
#include "../anim/channel.h"
#include "../diagnostics.h"
#include "../document.h"
#include "../profile.h"
#include "bake.h"

namespace whiteout {
namespace models {
namespace wem {

/// The vertices @p cloth draws: those of its bindings' faces, ascending.
std::vector<u32> ClothDrawnVertices(const Model& model, const Cloth& cloth);

/// A *Full detail* cloth's drivers (§10.3): its free particles, by cage
/// vertex ascending, and the holder they hang from, the bone outside the
/// cloth's own its pins' anchors weigh most on (`kInvalidNode`, the model
/// root, when none does).
struct ClothDrivers {
    u32 holder = kInvalidNode;
    std::vector<u32> vertices;
};
ClothDrivers ClothDriversOf(const Model& model, const Cloth& cloth);

/// @p cloth's driver @p driver's @p channel (`ClothDriverTranslation` or
/// `ClothDriverRotation`) on @p model, declared when missing.
u32 ClothDriverChannel(Model& model, u32 cloth, u32 driver, Channel channel);
/// The same, or `kInvalidIndex` when it is not declared.
u32 FindClothDriverChannel(const Model& model, u32 cloth, u32 driver, Channel channel);

/// Where @p vertex of @p mesh stands as its skin carries it in @p pose: its
/// rest position skinned, turned as its skin turns it. The frame a particle
/// holds where its cloth is off: "as authored" (§10.4).
Matrix44f AnchoredFrame(const Mesh& mesh, u32 vertex, const Pose& pose);

/// @p clip's driver frames of @p cloth at @p seconds, model space, where a
/// bake keyed them (§10.3): each driver's keys relative to the holder's frame
/// in @p pose. False when the clip keys none of them.
bool DriverFramesAt(const Document& document, u32 model, const Cloth& cloth, const ClothDrivers& drivers, u32 clip,
                    f32 seconds, const Pose& pose, std::vector<Matrix44f>& frames);

/// Where a clip baked in full detail puts @p cloth's particles at @p seconds
/// (§10.3): its drivers by their keys, its pinned particles where their
/// anchors carry them in @p pose; each one's cage vertex in @p particleVertex.
/// False when the clip keys none of its drivers.
bool DriverParticlesAt(const Document& document, u32 model, const Cloth& cloth, u32 clip, f32 seconds,
                       const Pose& pose, std::vector<u32>& particleVertex, std::vector<Matrix44f>& frames);

/// @p cloth's drawn faces where @p frames put its particles (§9.2), StarCraft
/// II's formula: each lane's particle takes its rest off and its frame on.
/// @p particleVertex names each frame's cage vertex; a vertex whose lanes name
/// none of them is left out.
struct ClothDrawn {
    u32 mesh = 0;
    std::vector<u32> vertices;
    std::vector<Vector3f> positions;
    std::vector<Matrix44f> rotations; ///< Each one's turn, for its normal.
};
ClothDrawn SkinClothDrawn(const Model& model, const Cloth& cloth, std::span<const u32> particleVertex,
                          std::span<const Matrix44f> frames);

/// *Full detail* made bones for a target that runs no cloth (§10.3), on the
/// export's copy: a bone per driver under its holder at its particle's rest,
/// the drawn faces skinned to them (a pinned particle's share going to its
/// anchors, @p target's influences kept), and every clip of the model keyed:
/// from the drivers where a bake wrote them, else "as authored" (§10.4). The
/// cloth itself is left for `DropClothForProfile`. Returns the bones made.
u32 ExpandClothToBones(Document& document, ProfileId target, Diagnostics& out);

/// The best rigid motion carrying @p from onto @p to, each pair weighed by
/// @p weights (Horn's quaternion method): `to ≈ rotation(from) + translation`.
/// None when fewer than three pairs weigh anything.
bool FitRigid(std::span<const Vector3f> from, std::span<const Vector3f> to, std::span<const f32> weights,
              Transform& out);

/// One cloth's bones fitted to its simulation (§10.2). A cursor over the
/// document, as `Animator` is: it must not outlive it or an edit.
class ClothBoneFit {
public:
    /// @p particleVertex: each of the run's particles of @p cloth, its cage
    /// vertex, in the run's order.
    ClothBoneFit(const Document& document, u32 model, const Cloth& cloth, std::span<const u32> particleVertex);

    /// Whether there is anything to fit: a cloth bone skinning a drawn point.
    bool fits() const {
        return !bones_.empty() && !drawn_.empty();
    }

    /// @p pose's cloth bones moved so the drawn faces' skin lands where
    /// @p frames (the cloth's particles, the run's order) put them: their T
    /// and R, the frames composed again. Returns the largest distance left
    /// between a skinned point and its target.
    f32 fit(std::span<const ClothParticleFrame> frames, Pose& pose) const;

    /// The drawn faces' size at rest: their bounding box's diagonal.
    f32 size() const {
        return size_;
    }
    /// The cloth bones that skin a drawn point, root to tip.
    std::span<const u32> bones() const {
        return bones_;
    }

private:
    struct Lane {
        u32 particle = 0;
        f32 weight = 0.0f;
        Vector3f offset{0, 0, 0}; ///< The point less its particle's rest.
    };
    struct Influence {
        u32 bone = 0;
        f32 weight = 0.0f;
    };

    const Document* document_ = nullptr;
    u32 model_ = 0;
    Animator animator_;
    Animator::Composition composition_;
    std::vector<u32> bones_;
    std::vector<u32> drawn_;
    std::vector<Vector3f> rest_;
    std::vector<std::array<Lane, 4>> lanes_;
    std::vector<std::vector<Influence>> skin_;
    f32 size_ = 0.0f;
};

} // namespace wem
} // namespace models
} // namespace whiteout
