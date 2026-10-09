// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file animator.h
 * @brief WEM's evaluator: a mix of plays, each clip's tracks read by its own
 *        rule, and the layers combined by StarCraft II's blend
 *        (WEM_ANIMATION_RUNTIME_DESIGN.md §3).
 *
 * A play is one clip at one time. Its layers are `LayeredContainers` of the
 * clip; every layer of every play is sorted by priority, highest first, and a
 * tie goes to a concurrent global loop, then to the newer host play, and last
 * to a global that is not concurrent. Each channel then spends a budget of 1
 * down that list as `SampleRef` does (`M3ModelAdapter`): a transparent layer
 * without a track abstains, an opaque one contributes the channel's rest, a
 * play contributes once, and the contributions combine lowest first by the
 * smoothstep. With one play and no layers that disagree this is exactly the
 * clip read by its rule, which is what the editor's sampler relies on.
 *
 * The rest an opaque layer fills with is the main profile's storage policy
 * (`rests.h`); a node's transform rests where its rig does. The evaluator does
 * not billboard (that needs a camera) and runs no pose stage.
 */

#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../document.h"
#include "clip.h"
#include "track_read.h"

namespace whiteout {
namespace models {
namespace wem {

/// One clip at one time. @ref weight is the host's envelope times its own
/// weight: the evaluator does not fade.
struct Play {
    u32 clip = kInvalidIndex;
    f32 seconds = 0; ///< Since the play began, unwrapped.
    f32 weight = 1;
    bool loop = true;
};

/// The plays, newest first, and the world clock every global loop reads.
struct Mix {
    std::vector<Play> plays;
    f32 worldSeconds = 0;
    /// Whether the model's global loops (`IsGlobalLoop`) play under the host
    /// plays, as they always do in game.
    bool globals = true;
};

/// One model's pose.
struct Pose {
    /// Every node's translation, rotation and scale: offsets from the pivots
    /// on a pivot rig, the bone's local transform on an explicit-bind one.
    std::vector<Transform> local;
    /// Model space: rows 0..2 the node's axes, row 3 its origin.
    std::vector<Matrix44f> frame;
    /// What a vertex is skinned by: the identity at rest.
    std::vector<Matrix44f> skinning;
    /// Every channel of the model's table, by index, as one element of its
    /// value type. Node transforms included.
    std::vector<std::vector<u8>> channelValues;
};

/// @p node's `mdx::Node::NodeFlag` bits, as `toMdx` writes them: the raw word
/// the import kept, with the bits WEM names rewritten from `Node::flags`.
u32 MdxNodeFlags(const Node& node);

/**
 * @brief One model's evaluator.
 *
 * A cursor over the document, not a copy of it: it must not outlive the
 * document or survive an edit, and a holder rebuilds it on every edit
 * generation.
 */
class Animator {
public:
    /// Rests under the main profile's storage.
    Animator(const Document& document, u32 model);
    /// Rests under @p storage's: what a conversion to another game compares.
    Animator(const Document& document, u32 model, Game storage);

    /// Each channel's rest, by its place in the model's table.
    using Rests = std::vector<std::vector<u8>>;

    /// Every channel's rest, found now. Which rest a channel plays depends on
    /// whether any clip keys it, so finding one walks every clip of the model.
    Rests resolveRests() const;

    /// With the rests given (`resolveRests`, of an animator over the same
    /// document and model): it then reads no clip but the ones it is asked to
    /// play. For a caller that edits one clip per thread, where a rest found
    /// on demand would read the clips the other threads are writing.
    Animator(const Document& document, u32 model, Rests rests);

    /// No such model.
    bool empty() const {
        return model_ == nullptr;
    }

    /// `sample` then `compose`.
    void evaluate(const Mix& mix, Pose& out) const;

    /// The channel values and the nodes' local transforms of @p mix; the frames
    /// are left as they were. With @p nodesOnly, only the node transforms are
    /// evaluated and every other channel value is left empty: all a skeleton
    /// needs.
    void sample(const Mix& mix, Pose& out, bool nodesOnly = false) const;

    /// @p pose's `local` transforms from its channel values, as `sample` ends:
    /// for a host that changes the values between the two, as a blend of its
    /// own does.
    void place(Pose& pose) const;

    /// @p pose's frames and skinning matrices from its `local` transforms,
    /// parents first, by the model's rig: Warcraft III's composition with the
    /// nodes' inherit flags on a pivot rig (a camera stands at its bind plus its
    /// translation), `S·R·T` onto the parent on an explicit-bind one.
    void compose(Pose& pose) const;

    /// The order `compose` places the nodes in, parents first, and the parent
    /// each is composed onto: `kInvalidNode` for a root, a camera, a node under
    /// a camera, an M3 bone in model space, or where a cycle is cut.
    struct Composition {
        std::vector<u32> order;
        std::vector<u32> parent;
    };
    Composition composition() const;

    /// One node of @p pose composed as `compose` composes it: its frame and
    /// skinning matrix from its `local` and @p parent's frame. For a caller that
    /// re-composes part of a pose, in `composition()`'s order.
    void composeNode(Pose& pose, u32 node, u32 parent) const;

private:
    struct Layer {
        u32 play = 0;
        i32 priority = 0;
        bool transparent = false;
        const std::vector<const SubTrack*>* tracks = nullptr; ///< By channel index.
        const Clip* clip = nullptr;
        SampleWindow window;
        f32 weight = 1;
    };

    /// A clip's layers, resolved to one sub-track slot per channel.
    struct ClipLayers {
        std::vector<SubTrackContainer> owned; ///< Only when track sets split it.
        const std::vector<SubTrackContainer>* containers = nullptr;
        std::vector<std::vector<const SubTrack*>> tracks;
        bool resolved = false;
    };

    const ClipLayers& layersOf(u32 clip) const;
    std::vector<Layer> layersFor(const Mix& mix) const;
    const std::vector<u8>& restOf(std::size_t channel) const;

    const Document* document_ = nullptr;
    const Model* model_ = nullptr;
    u32 modelIndex_ = kInvalidIndex;
    Game storage_ = Game::Warcraft;
    /// Each channel's rest under the storage, found the first time it is read.
    mutable std::vector<std::vector<u8>> rests_;
    mutable std::vector<u8> restKnown_;
    std::vector<u8> held_;
    /// An explicit-bind rig's inverse binds, found once: every compose reads them.
    std::vector<Matrix44f> inverseBinds_;
    mutable std::vector<ClipLayers> clips_;
};

} // namespace wem
} // namespace models
} // namespace whiteout
