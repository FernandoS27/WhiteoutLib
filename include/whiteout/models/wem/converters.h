// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file converters.h
 * @brief The three shipped format converters (WEM v3, design §14).
 *
 * Each one has a byte-level entry point from `FormatConverter` and a **typed**
 * one taking the parsed format struct. The typed entry point is the real
 * interface: `.m2` import needs a sibling bundle that bytes alone cannot supply,
 * and every converter benefits from a caller that has already parsed once.
 *
 * `D3Converter` is P6 and lands in its own header — it needs an `AssetSource`,
 * which none of these do.
 */

#include <span>
#include <vector>

#include "../m2/structures.h"
#include "../m3/structures.h"
#include "../mdx/structures.h"
#include "../mdx/types.h"
#include "converter_base.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// MdxConverter
// ============================================================================

/**
 * @brief Warcraft III `.mdx`, both directions, serving both WC3 profiles.
 *
 * One import produces up to **two** material sets from one file by partitioning
 * layers on §7.2.1's HD test. A section whose material has layers for only one
 * profile gets a mask naming only that profile, which is what makes a
 * classic-only file produce no Reforged set at all rather than an empty one.
 */
class MdxConverter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    std::span<const ProfileId> profiles() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;

    Result<Document> importFromBytes(std::span<const u8> data) const override;
    Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                          u32 version = 0) const override;

    Result<Document> fromMdx(const mdx::Model& source) const;

    /// @p profile picks which set's materials are written and which sections
    /// are drawn; @p targetVersion is the `.mdx` version stamped on the result.
    Result<mdx::Model> toMdx(const Document& document, ProfileId profile,
                             u32 targetVersion = 800) const;
};

// ============================================================================
// M2Converter
// ============================================================================

/**
 * @brief World of Warcraft `.m2`.
 *
 * `importFromBytes` is unsupported and says so: an `.m2` is a bundle — the base
 * file carries no geometry batches at all, those live in the `.skin` files — so
 * the typed entry point taking an already-assembled `m2::Model` is the only
 * honest byte-free interface. Export writes the base `.m2` only.
 */
class M2Converter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    std::span<const ProfileId> profiles() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;

    Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                          u32 version = 0) const override;

    /// @p sourceVersion is the `.m2` header version the bundle was read from.
    /// `m2::Model` does not carry it — the parser consumes it and keeps nothing
    /// — but the native block records it, so the caller has to say.
    Result<Document> fromM2(const m2::Model& source, u32 sourceVersion = 274) const;
    Result<m2::Model> toM2(const Document& document, ProfileId profile,
                           u32 targetVersion = 274) const;
};

// ============================================================================
// M3Converter
// ============================================================================

/**
 * @brief StarCraft II / Heroes of the Storm `.m3`.
 *
 * The profile follows the `MODL` version — v30+ is Heroes, where `MADD` is the
 * load-time truth — and a caller that knows better can override it. Geometry
 * arrives in SC2's basis and is rebased into WEM's canonical space on import
 * (§6.2); the rebase is an axis permutation with determinant +1, so it is
 * bit-exact and preserves winding.
 */
class M3Converter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    std::span<const ProfileId> profiles() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;

    Result<Document> importFromBytes(std::span<const u8> data) const override;
    Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                          u32 version = 0) const override;

    /// The profile `fromM3` picks for a model of @p modelVersion.
    static ProfileId ProfileForVersion(u32 modelVersion);

    /// @p profileOverride of `ProfileId::Count` means "decide from the version".
    Result<Document> fromM3(const m3::Model& source,
                            ProfileId profileOverride = ProfileId::Count) const;
    Result<m3::Model> toM3(const Document& document, ProfileId profile,
                           u32 targetVersion = 30) const;

    /**
     * @brief Merges an external animation file (`.m3a`) into an imported model.
     *
     * Nothing in a `.m3` names its `.m3a` — the caller decides which pair up —
     * and the join inside is the **animId** alone. The merge therefore adds
     * clips and containers that reference channels @p document already declares;
     * a channel the base model never declared is skipped rather than invented,
     * because an external file can supply new motion for a target but not a new
     * target (§10.8.1).
     *
     * @return how many clips were added, or an error when @p model is not in the
     *         document.
     */
    Result<u32> mergeAnimation(Document& document, u32 model, const m3::Model& external) const;
};

/// Registers the three built-in converters. Called by `ConverterRegistry`'s
/// constructor; exposed so a host that builds its own registry can reuse it.
void RegisterBuiltinConverters(ConverterRegistry& registry);

} // namespace wem
} // namespace models
} // namespace whiteout
