
#pragma once

// ============================================================================
// Lazy `.anim` loading — the client's own scheme, in this parser's shape.
//
// A `.m2` keeps only some of its sequences' keys; the rest live in `.anim`
// siblings, one file per (animation id, variation). A character model can have
// a hundred of them and several megabytes of keys, and a viewer that plays one
// sequence has no use for the other ninety-nine.
//
// The client never reads them at load. CM2Model::LoadSequence is called when
// something asks a model to *play* a sequence; it walks the variation chain and
// hands each entry that is not already resident to
// CM2Shared::LoadLowPrioritySequence, which reads `%s%04d-%02d.anim`
// asynchronously and, on arrival, re-runs the model's fixup pass with
// `s_loadingSequence` set — resolving that one sequence's slice of every track
// against the file that just landed, and nothing else.
//
// Parser::setLazyAnimations(true) puts this parser on the same footing. The
// parse reads the keys the model carries itself and stops there, keeping each
// deferred sequence's KeySpanRef per track; loadSequence() below reads the
// sibling and fills in that one sequence's sub-arrays. The difference from the
// client is that the read here is synchronous and the keys are copied out of
// the buffer rather than pointed into it, so the `.anim` is dropped again once
// its keys are in the model.
//
// Off by default: an eagerly parsed model needs no follow-up call, and every
// existing caller keeps that behaviour.
// ============================================================================

#include "structures.h"

namespace whiteout {
namespace m2 {

/// @brief True when @p model was parsed with lazy animations and therefore has
///        sequences whose keys are not in memory yet.
bool hasLazyAnimations(const Model& model);

/// @brief True when @p sequenceIndex still needs its `.anim` read.
///
/// False for a sequence the model carries itself, for one already loaded, and
/// for one whose sibling turned out to be missing — the three cases in which
/// calling loadSequence() would do no useful work. This is the predicate a
/// per-frame caller wants, so a model missing half its `.anim` files does not
/// retry them every frame.
bool sequenceKeysPending(const Model& model, u32 sequenceIndex);

/// @brief Read @p sequenceIndex's keys from its `.anim` sibling and fill in
///        that sequence's slice of every track in @p model.
///
/// Returns true when the sequence's keys are resident afterwards, including the
/// case where they already were. Safe to call on an eagerly parsed model (true,
/// no work) and on an out-of-range index (false).
///
/// The file is named the way CM2Shared::LoadLowPrioritySequence names it: an
/// alias sequence has no file of its own, so the alias chain is followed to the
/// sequence that does, and its keys are read into the alias's slots.
bool loadSequence(Model& model, u32 sequenceIndex);

/// @brief Drop the keys @p sequenceIndex got from a `.anim`, so a later
///        loadSequence() reads them again.
///
/// Does nothing for keys the model carries itself: those were read from a
/// buffer the parse no longer holds, and dropping them would be permanent.
void unloadSequence(Model& model, u32 sequenceIndex);

/// @brief unloadSequence() for every streamed sequence.
void unloadAllSequences(Model& model);

} // namespace m2
} // namespace whiteout
