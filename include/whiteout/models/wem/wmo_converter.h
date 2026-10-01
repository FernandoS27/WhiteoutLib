// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file wmo_converter.h
 * @brief World of Warcraft `.wmo` -> `Document`, import only.
 *
 * A WMO is a root and one file per group, the groups found by FileDataID, so,
 * like `.m2`, the typed entry point over the parsed `wow::wmo::Model` is the only
 * honest one. It writes the World of Warcraft profile, an `.m2`'s: the same
 * combiner vocabulary, so every export an `.m2` reaches, a WMO reaches.
 *
 * ### What 12.1 draws, and what crosses
 *
 * One mesh per LOD 0 group that draws, one section per drawn batch, every vertex
 * rigid to one root bone. A WMO material blends some layers by a vertex colour's
 * alpha (MOCV set 1, MOC2), which no combiner stage reads; the chain takes the
 * layer the batch's vertices mostly show. The file's colour sets ride beside the
 * geometry as `wmo.*` layers, which no exporter asks for, and `color0` is what
 * another format can use of them: the light MOCV gives a vertex-lit batch.
 *
 * ### Doodads are `.m2` models
 *
 * Each placed doodad of the active sets becomes an attachment node on the root,
 * at its MODD transform, naming its model by FileDataID or path. Converting the
 * model is the `.m2` converter's job, and finding it the caller's: append it
 * (`AppendDocument`) and point the attachments at it.
 */

#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/models/wow/wmo/parser.h>

#include "converter_base.h"
#include "document.h"

namespace whiteout {
namespace models {
namespace wem {

struct WmoImportOptions {
    /// The doodad sets on besides set 0, as `wmo::placedDoodads` takes them.
    std::vector<u16> doodadSets;
};

class WmoConverter {
public:
    Result<Document> fromWmo(const wow::wmo::Model& source,
                             const WmoImportOptions& options = {}) const;
};

} // namespace wem
} // namespace models
} // namespace whiteout
