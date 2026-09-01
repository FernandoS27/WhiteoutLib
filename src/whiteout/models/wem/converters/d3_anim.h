// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file d3_anim.h
 * @brief Diablo III animation import (design §10.8.3, the Diablo3 row).
 *
 * ### One permutation is one clip
 *
 * An `.ani` is a small set of *permutations* — variants of one action, picked at
 * play time by weight — and each carries its own frame count, rate, bone list
 * and curves. So the clip is the permutation, and the `.ani` is a group of them
 * that an anim set names as one.
 *
 * ### The join is a bone **name**
 *
 * Unlike every other field D3 joins by index, a permutation names its bones as
 * 64-byte strings and matches them against the appearance's `BoneStructure`
 * names — 4-byte hashes at runtime, strings on disk. That is what lets one
 * `.ani` drive several appearances, and it is why a name the model does not
 * carry is a skipped track and not an error.
 *
 * ### Frames are not seconds and the rate is per permutation
 *
 * `fps = flFramesPerTick * 60` (`Anim_InitPlaybackState`), so a key at frame *f*
 * is at `f / fps` seconds and the clip runs `(frameCount - 1) / fps`. The
 * "-1" is the engine's: the last frame is the end of the last *span*, not a
 * frame of its own.
 *
 * ### The rotation is a signed 16-bit quaternion, w last
 *
 * `x / 32767` per component, then normalised. The `u16` fields are signed data;
 * reading them unsigned mirrors every rotation past a half turn.
 */

#include <vector>

#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>
#include <whiteout/sno/d3/native/types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace d3_anim {

/// Appends one clip per permutation of @p source to @p document, driving
/// `document.models[model]`. Returns the clip index of each permutation, in
/// permutation order, with `kInvalidIndex` for one that produced nothing.
std::vector<u32> ImportAnim(const sno::d3::native::Anim& source, Document& document, u32 model,
                            Diagnostics& out);

} // namespace d3_anim
} // namespace wem
} // namespace models
} // namespace whiteout
