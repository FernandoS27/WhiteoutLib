// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m3_core.h
 * @brief SC2 / Heroes material conversion, both directions (design §7.2.1, §7.2.6).
 *
 * ### An M3 material is reached through a map, not by index
 *
 * `MODL` holds one array per material *type* plus a `MaterialMap` of
 * `{type, index}` pairs, and it is the map a region names. So the unit of
 * conversion here is a `MaterialMap` entry, not a `StandardMaterial` — a
 * converter that indexed the standard array directly would silently work on
 * every model until it met a volume material.
 *
 * ### MADD is the load-time truth, and it is kept twice
 *
 * On a v30+ file the loader converts every standard, displacement and reflection
 * material into a `DataDrivenMaterial` and consumes only that. `MADD` therefore
 * *is* the material, and it carries things the standard form has nowhere to put
 * (`blendMode` in `unknown124`, the environment op in `EnvioControl`). But the
 * runtime also restores a standard material from it, and that restoration is what
 * the common model can be derived from. So import restores first, maps the
 * restoration, and keeps the authored blob beside it in the native block.
 *
 * ### The nine non-standard kinds have no faithful generic projection
 *
 * A volume material is not a layer stack; neither is a lens flare. Their common
 * material is a best-effort composite over whatever texture layers they hold,
 * plus a `LossyKindConversion`, and their truth stays native. Pretending
 * otherwise would make a generic consumer draw something the game never drew.
 */

#include <string>
#include <vector>

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/materials/material.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m3_core {

/// What the converter must supply that an `m3` material does not carry.
struct Context {
    /// `MODL` version, 23..30+. Decides which material arrays exist at all, and
    /// therefore lands on every native block as `sourceVersion`.
    u32 modelVersion = 23;

    /// `.m3` layers name textures by **path**, not by index, so the document's
    /// texture table is keyed the same way here. A path that is not in it
    /// resolves to "no texture" plus a `TextureUnresolved`, unless
    /// `internUnknownPaths` is set.
    ///
    /// Mutable because interning happens inside `toDocument`, during an import
    /// that takes the context by const reference. See the flag below.
    mutable std::vector<std::pair<std::string, u32>> texturesByPath;

    /// Adds an unseen path to `texturesByPath` and returns its new index rather
    /// than reporting it unresolved.
    ///
    /// An importer wants this: it *is* the thing building the document's
    /// texture table, and there is no earlier pass that could have pre-filled
    /// it — the layers a material reads are the material's own business, and
    /// enumerating them from outside would mean a second copy of the per-kind
    /// slot lists. A fixture wants it off, where an unknown path is a mistake.
    bool internUnknownPaths = false;

    /// When true, a `MADD` whose exact restoration is refused falls back to
    /// `approximateStandardMaterial()` — a likeness inferred from node types and
    /// texture filenames, never a conversion. Off by default: an approximation
    /// that arrives unasked-for is indistinguishable from data.
    bool approximateShaderGraphs = false;

    /// The document index for @p path. Trailing NULs are dropped first: a
    /// shipped `.m3` string carries its terminator inside the `std::string`,
    /// because the `Reference` count includes it, so two spellings of one path
    /// would otherwise intern twice.
    u32 toDocument(const std::string& path) const;
    std::string toPath(u32 documentTextureId) const;
};

/// One `MaterialMap` entry -> one `wem::Material`.
///
/// Yields a `Composite` (§7.2.6) with the native block attached `InSync`. A map
/// entry naming a type the model has no array for, or an index past its end,
/// produces a material with an empty body and an `IndexOutOfRange`.
Material ImportMaterial(const m3::Model& model, const m3::MaterialMap& entry, ProfileId profile,
                        const Context& context, Diagnostics& out);

/// Convenience: every `materialMaps` entry, in order. The result is index-aligned
/// with `model.materialMaps`, which is the index space a region refers through.
std::vector<Material> ImportMaterials(const m3::Model& model, ProfileId profile,
                                      const Context& context, Diagnostics& out);

/// The inverse. With a native block present and not `CommonEdited` (§7.1) this
/// writes the block's own body back into @p model and returns the map entry that
/// names it; otherwise it projects the common material onto a `StandardMaterial`,
/// which is the only M3 kind a generic body can become.
m3::MaterialMap ExportMaterial(const Material& material, ProfileId profile, const Context& context,
                               m3::Model& model, Diagnostics& out);

} // namespace m3_core
} // namespace wem
} // namespace models
} // namespace whiteout
