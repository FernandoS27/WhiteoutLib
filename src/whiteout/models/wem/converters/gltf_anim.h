// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file gltf_anim.h
 * @brief Clips → glTF animations (GLTF_DESIGN §8). Internal to the converter.
 */

#include <span>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/models/gltf/gltf.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

#include "gltf_bin.h"

namespace whiteout {
namespace models {
namespace wem {
namespace gltf_anim {

/**
 * @brief Exports every clip of @p document as one glTF animation each.
 *
 * @p modelNodeBase maps each `Document::models` index to the glTF index of its
 * first tree node (the converter's numbering: synthetic root at `base - 1`).
 * Containers flatten by priority; node Translation/Rotation/Scale sub-tracks
 * become channels and everything else drops with counted diagnostics.
 */
void Export(const Document& document, gltf::Asset& asset, gltf_detail::BinBuilder& bin,
            std::span<const u32> modelNodeBase, Diagnostics& diagnostics);

/**
 * @brief Imports every glTF animation as one `Clip` on `document.models[0]`.
 *
 * One opaque container at priority 0 per clip, duration = the longest sampler
 * input, `looping = true` (what most DCC clips intend). @p wemIndex maps glTF
 * node indices to `models[0]`'s node indices. Channels are shared across
 * clips: one `AnimChannel` per (node, property), allocated on first use.
 */
void Import(const gltf::Asset& source, Document& document, std::span<const u32> wemIndex,
            Diagnostics& diagnostics);

} // namespace gltf_anim
} // namespace wem
} // namespace models
} // namespace whiteout
