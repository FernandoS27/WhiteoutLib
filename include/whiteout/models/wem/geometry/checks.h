// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file checks.h
 * @brief Per-mesh structural and manifold checks (WEM v3, design §5.7, §5.10).
 *
 * These are the mesh half of `Validate`. They live beside the kernel rather than
 * in `validate.cpp` because the thing they check is here; `validate.cpp` owns the
 * *registration*, and its document-level rules call into these.
 *
 * `CheckManifold` answers "could a strict half-edge library take this mesh right
 * now" in one call. After a clean import the answer is yes by construction — the
 * set is here to catch a bad edit.
 */

#include "../diagnostics.h"
#include "mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/**
 * @brief Structural invariants — a violation means the mesh is corrupt.
 *
 * Checks the connectivity relations (C1, C2, C7), index ranges, attribute layer
 * sizes (C9) and the skin binding's CSR shape. Connectivity checks are skipped
 * when the mesh has none built, since there is then nothing to be inconsistent.
 *
 * @param meshIndex Reported in every `ElementRef`, so a document-level caller can
 *                  say which mesh.
 */
void CheckStructural(const Mesh& mesh, u32 meshIndex, Diagnostics& out);

/**
 * @brief The §5.10 contract: C4 (2-manifold edges), C5 (one fan per vertex),
 *        C6 (no repeated or duplicated face vertex sets).
 *
 * Requires connectivity; a mesh without it reports nothing, because the face set
 * alone cannot answer C5.
 */
void CheckManifold(const Mesh& mesh, u32 meshIndex, Diagnostics& out);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
