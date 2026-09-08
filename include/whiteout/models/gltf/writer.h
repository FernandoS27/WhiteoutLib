// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief `gltf::Writer` — `gltf::Asset` to `.gltf` text or `.glb` bytes.
 *
 * Deterministic by construction: members emit in one fixed order, floats print
 * through the json module's `%.9g`, and defaults are omitted the same way every
 * run — two exports of one asset are byte-identical, which is what makes the
 * golden gates possible (GLTF_DESIGN §11).
 *
 * The writer serializes exactly what the asset says. It does not move image
 * bytes into buffers or rewrite URIs — embedding decisions are the caller's
 * (the app-side export driver packs images before calling `ToGlb`).
 */

#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "gltf.h"

namespace whiteout {
namespace models {
namespace gltf {

class Writer {
public:
    /// The JSON body, minified. `buffers[0]`'s `uri` decides its story: empty
    /// means "the GLB BIN chunk", so a caller writing a standalone `.gltf`
    /// names every buffer.
    static std::string ToJsonText(const Asset& asset);

    /**
     * @brief The full `.glb`: header, JSON chunk (space-padded to 4), and a
     *        BIN chunk from `buffers[0].data` when it is non-empty.
     *
     * `buffers[0].byteLength` is written from the data's actual size; the
     * asset's own field is not trusted to have kept up.
     */
    static std::vector<u8> ToGlb(const Asset& asset);
};

} // namespace gltf
} // namespace models
} // namespace whiteout
