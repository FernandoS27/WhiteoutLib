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

#include "../gltf/gltf.h"
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

// ============================================================================
// GltfConverter
// ============================================================================

/// What `toGltf` may leave out. Rescaling is **not** here — a caller that wants
/// another scale runs `RescaleDocument` on a staged copy first, the same
/// division of labour as the MDX and M3 export drivers.
struct GltfWriteOptions {
    /// Drop meshes above the base level of detail, matching the other exporters.
    bool baseLodOnly = true;
    /// Parent a model referenced by an `AttachmentPayload` under its attach
    /// point — how a D3 actor's child models ride along (§7).
    bool bakeChildModels = true;
};

/**
 * @brief glTF 2.0 / GLB — the first crossing outside the Blizzard family
 *        (GLTF_DESIGN).
 *
 * glTF is not a game, so it brings **no profile of its own**: import produces a
 * `Generic`-profile document with a `PBRDeferred` set (`NativeSync::Absent`,
 * there being no native block to record), and export accepts any profile the
 * document carries, lowering that set to metallic-roughness with declared loss.
 * `profiles()` therefore answers `{Generic}` — the first converter ever to
 * serve it, which §6.1 designed it for; "`Generic` is a source, never a
 * `DeriveProfile` target" is untouched.
 *
 * The basis change (Blizzard +X-forward/+Z-up ⇄ glTF +Z-forward/+Y-up) is a
 * pure cyclic permutation baked into the data at this boundary; neither
 * `CoordSpace` enum grows a value for it.
 */
class GltfConverter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    std::span<const ProfileId> profiles() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;

    /// Sniffs the GLB magic; anything else is taken as `.gltf` JSON text.
    Result<Document> importFromBytes(std::span<const u8> data) const override;

    /// A self-contained `.glb`: images stay URI references (the library never
    /// sees a pixel — embedding is the app driver's job), geometry is the BIN
    /// chunk.
    Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                          u32 version = 0) const override;

    Result<Document> fromGltf(const gltf::Asset& source) const;
    Result<gltf::Asset> toGltf(const Document& document, ProfileId profile,
                               const GltfWriteOptions& options = {}) const;
};

/// Registers the built-in converters. Called by `ConverterRegistry`'s
/// constructor; exposed so a host that builds its own registry can reuse it.
void RegisterBuiltinConverters(ConverterRegistry& registry);

} // namespace wem
} // namespace models
} // namespace whiteout
