// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m2_anim.h
 * @brief WoW animation import (design §10.8.3, the Wow row).
 *
 * The easy one, and worth saying why: an `.m2` track is **already** per
 * sequence — `AnimationTrack<T>::timestamps` is a vector of vectors, one inner
 * array per sequence — so there is nothing to slice. One sequence is one clip,
 * one container, and the inner array at the sequence's index is the sub-track.
 *
 * Three things that are not:
 *
 * - **Global sequences.** A track with a `globalSequenceId` keeps its keys in
 *   inner array 0 and loops on the world clock. It becomes one auto-play clip
 *   per global loop — the same `AutoPlay | WorldClocked` MDX's `globalSeqId` and
 *   M3's SEQS flag 0x2 arrive at.
 * - **Alias sequences.** `SequenceFlag::IsAlias` means the sequence has no data
 *   of its own and `aliasNext` names the one that does. The clip is still
 *   imported — a host asking for it by name must find it — and the chain rides
 *   `ClipNative`, because following it is playback policy and WEM does not own
 *   the blender.
 * - **Fixed-point values.** `i16` weights and colours are `x / 32767`, and a
 *   `u8` visibility is a flag, not a byte to scale. Getting the second one wrong
 *   is a model that draws at 1/255 opacity and looks invisible rather than
 *   broken.
 *
 * ### Where a texture track lands
 *
 * A batch's weight and transform are indices into `textureWeights` and
 * `textureTransforms`, resolved per texture unit and stored on the material's
 * `native::M2Material` block. Import reads them back from there rather than
 * redoing the combo-table join: the unit ordinal **is** the `Combiners` stage
 * ordinal, so the block already answers "which stage does this track drive".
 *
 * `M2Color` is the exception. It multiplies the whole batch, not one stage, so
 * its channels target `kWholeMaterial` — the same "not one of its layers"
 * ordinal `MaterialFeature::layer` uses.
 */

#include <vector>

#include <whiteout/models/m2/structures.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m2_anim {

/// Where the node import put each satellite array. Recomputed rather than
/// passed, in `ImportNodes`' own order — bones, attachments, lights, events,
/// ribbons, cameras.
struct NodeBases {
    u32 bone = 0;
    u32 attachment = 0;
    u32 light = 0;
    u32 event = 0;
    u32 ribbon = 0;
    u32 camera = 0;

    static NodeBases Of(const m2::Model& source);
};

/// `Model::materialSlots[slotOfBatch[skin][batch]]` — the slot each `.skin`
/// batch became.
struct Context {
    NodeBases bases;
    std::vector<std::vector<u32>> slotOfBatch;
};

void Import(const m2::Model& source, const Context& context, Document& document, u32 model,
            Diagnostics& out);

/// Where each WEM node landed in the `.m2` being written.
///
/// The satellite arrays are not addressable by a base offset on the way out the
/// way `NodeBases` makes them on the way in: export walks the node tree and
/// writes whatever it finds, so a document with no lights simply has none and
/// every later base would be wrong. The map is the honest form of the same
/// join.
struct ExportContext {
    enum class Slot : u8 { None = 0, Bone, Attachment, Light, Event, Camera };

    struct NodeSlot {
        Slot slot = Slot::None;
        u32 index = 0;
    };

    /// Parallel to `Model::nodes`.
    std::vector<NodeSlot> nodeSlots;
};

/// Writes `document`'s clips back onto `out` — the inverse of @ref Import.
///
/// Nothing is sliced or merged: an `.m2` track is already one inner array per
/// sequence, so a clip IS an inner array. The material tracks are placed
/// through the native block, which kept the resolved weight / transform /
/// colour indices that `appendCombos` writes back into the combo tables.
void Export(const Document& document, u32 model, const ExportContext& context, m2::Model& out,
            Diagnostics& diagnostics);

} // namespace m2_anim
} // namespace wem
} // namespace models
} // namespace whiteout
