// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m2_core.h
 * @brief WoW material conversion, both directions (design §7.2.2, §7.2.6).
 *
 * ### A WoW material is a join, and the join is the work
 *
 * Nothing in an `.m2` is "a material". A draw is a `Batch` in the `.skin`, which
 * names a two-field render record, a `shaderId`, and four **combo tables** it
 * indexes with `textureComboIndex + unit`. Import resolves that join into
 * `native::M2Material` — the *resolved* batch — and export rebuilds the tables.
 *
 * That is also why `M2Material` is the one native block whose shape is authored:
 * there is no struct anywhere to mirror.
 *
 * ### `shaderId` is what makes `Combiners` a kind
 *
 * The id resolves through `s_modelShaderEffect` to pixel shaders literally named
 * `Combiners_Opaque_Mod2x` — stage 0 opaque, stage 1 mod2x. The chain is the
 * material. A row past the transcribed table produces a diagnostic and the
 * bit-field path's answer, **never row 0**; the row-0 fallback is the recorded
 * black-box artifact and reproducing it would be reproducing a bug.
 *
 * ### The weight goes on unit 0 and nowhere else
 *
 * The shipped rule is that only texture unit 0 takes the batch's transparency,
 * and the *product* of colour and transparency is what draws. That is the
 * converter's rule to apply, not the schema's, so it is applied here.
 */

#include <vector>

#include <whiteout/models/m2/structures.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/materials/material.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m2_core {

/// What the converter must supply that a `Batch` does not carry.
struct Context {
    /// The `.m2` version the record was read from.
    u32 sourceVersion = 0;

    /// `m2::Model::textures[i]` -> `Document::textures[]`.
    std::vector<u32> textureIndexMap;

    u32 toDocument(u32 m2TextureId) const;
    u32 toM2(u32 documentTextureId) const;
};

/// One `.skin` batch -> one `wem::Material`, with the join resolved and the
/// native block attached `InSync`.
///
/// @p model supplies the combo tables, the render records and the texture
/// records; @p batch is the draw. A combo index past the end of its table
/// resolves that unit to nothing and reports `IndexOutOfRange` — the alternative
/// is a unit that silently samples texture 0.
Material ImportBatch(const m2::Model& model, const m2::Batch& batch, const Context& context,
                     Diagnostics& out);

/// The inverse. Appends this material's render record, texture entries and combo
/// rows to @p model and returns the `Batch` that names them, so exporting a set
/// of materials in order rebuilds the tables in order.
///
/// With a native block present and not `CommonEdited` (§7.1) the resolved batch
/// is written back verbatim; otherwise the `Combiners` chain is projected onto a
/// `shaderId`, which is lossy in exactly the way §7.2.2 describes.
m2::Batch ExportMaterial(const Material& material, const Context& context, m2::Model& model,
                         Diagnostics& out);

} // namespace m2_core
} // namespace wem
} // namespace models
} // namespace whiteout
