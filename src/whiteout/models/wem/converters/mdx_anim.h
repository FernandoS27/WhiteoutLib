// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_anim.h
 * @brief WC3 animation import (design §10.8.3, the MDX row).
 *
 * ### One timeline, N clips
 *
 * MDX has no per-sequence tracks. Every track in the file spans **one global
 * timeline in milliseconds**, and a `Sequence` is a window `[intervalStart,
 * intervalEnd]` onto it. So the import is a slice, once per sequence per track.
 *
 * The slice **keeps the two bracketing keys** — the last key before the window
 * and the first one after it — with their times rebased and therefore negative
 * or past the clip's end. That is what makes the clip evaluate identically to
 * the global track: WC3 interpolates across the window's edges, and a slice that
 * dropped the neighbours would flatten the first and last spans to a hold.
 * §10.8.2 already says a sub-track's times are not clamped to its clip, and this
 * is the case that needs it. Shipped exporters put keys on the boundaries, so on
 * most content the bracket keys are the boundary keys and nothing is added.
 *
 * A track keyed to a `globalSequenceId` is **not** sliced into the sequences: it
 * runs on its own clock, so it becomes one auto-play clip per global sequence,
 * flagged `AutoPlay | WorldClocked` — the same three-format unification M2's
 * global sequences and M3's SEQS flag 0x2 arrive at.
 *
 * ### What is deliberately not imported
 *
 * Particle, ribbon and corn emitter **property** tracks — emission rate,
 * lifespan, latitude, widths. §18 puts the systems themselves out of scope: WEM
 * stores the emitter's placement and an `AssetKey`, and animating a property of
 * a system it does not hold would be storing half of one. Emitter *visibility*
 * is imported, because that is a property of the node.
 */

#include <unordered_map>
#include <vector>

#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/mdx/types.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/profile.h>

namespace whiteout {
namespace models {
namespace wem {
namespace mdx_anim {

/// The joins the `.mdx` does not carry, all of them decided by the node and
/// material import that ran first.
struct Context {
    /// `mdx::Node::objectId` -> `Model::nodes` index.
    const std::unordered_map<u32, u32>* byObjectId = nullptr;

    /// `mdx::Model::cameras[i]` -> `Model::nodes` index. Cameras have no
    /// `objectId`, so they cannot go through the map above.
    std::vector<u32> cameraNodes;

    /// Per profile, per `mdx::Model::materials[]`, per layer: the WEM ordinal
    /// that layer became, or `kInvalidIndex`. `mdx_core::ImportMaterial` fills
    /// it, because the ordinal is a position in the *filtered* stack.
    struct ProfileLayers {
        ProfileId profile = ProfileId::Generic;
        std::vector<std::vector<u32>> byMaterial;
    };
    std::vector<ProfileLayers> layerOrdinals;
};

/// Appends the clips, the channel table and the auto-play clips to
/// `document.models[model]` and `document.clips`.
void Import(const mdx::Model& source, const Context& context, Document& document, u32 model,
            Diagnostics& out);

/// Where each WEM node and material layer landed in the `.mdx` being written.
///
/// The mirror of `Context` above, and it exists for the same reason: an `.mdx`
/// keeps its animation on the *records* — a bone's tracks are on the bone, a
/// layer's alpha is on the layer — so an exporter has to know which record each
/// WEM target became. Only `toMdx` knows that, because only `toMdx` decides
/// which typed array a node goes into.
struct ExportContext {
    /// The typed array a node was written to. `Camera` is separate because a
    /// camera is not a node chunk in MDX at all.
    enum class Slot : u8 {
        None = 0,
        Bone,
        Helper,
        Light,
        Attachment,
        ParticleEmitter,
        ParticleEmitter2,
        RibbonEmitter,
        CornEmitter,
        EventObject,
        CollisionShape,
        Camera,
    };

    struct NodeSlot {
        Slot slot = Slot::None;
        u32 index = 0; ///< Index within that array.
    };

    /// Parallel to `Model::nodes`.
    std::vector<NodeSlot> nodeSlots;

    /// Per `Model::meshes[]`, the geosets its sections became.
    ///
    /// One entry per mesh once, several now: a geoset carries one material, so
    /// `toMdx` writes one per SECTION. A `Kind::Section` channel still names
    /// only a mesh (`TrackTarget::mesh`), so it fans out over that mesh's run.
    std::vector<std::vector<u32>> geosetsOfMesh;

    /// Per material slot, the layer index each WEM ordinal became. The identity
    /// map on every shipped material — `ImportMaterial` numbers ordinals over
    /// the *filtered* stack and the export writes exactly that stack — but
    /// stated rather than assumed, because the two are only equal as long as
    /// nothing between them reorders a layer.
    std::vector<std::vector<u32>> layerOfOrdinal;
};

/// Writes `document`'s clips back onto `out` as sequences, global sequences and
/// per-record tracks — the inverse of @ref Import (design §10.8.3).
///
/// The two directions are not symmetric in one place, and it is the interesting
/// one: import SLICED one global timeline into a clip per sequence, keeping the
/// bracketing keys, so export MERGES the clips back onto that timeline. A key
/// two clips share — which is exactly what a bracket key is — is written once.
///
/// A clip carrying no `intervalStart` (one that came from another format, or
/// from an editor) is given a fresh window after the last one, because MDX has
/// nowhere else to put it: the timeline is the only clock the format has.
void Export(const Document& document, u32 model, ProfileId profile, const ExportContext& context,
            mdx::Model& out, Diagnostics& diagnostics);

} // namespace mdx_anim
} // namespace wem
} // namespace models
} // namespace whiteout
