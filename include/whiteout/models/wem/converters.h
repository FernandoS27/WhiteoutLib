// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file converters.h
 * @brief Format converters between WEM and MDX/M2/M3 model formats
 *
 * Provides bidirectional conversion between WEM intermediate format and the
 * three engine-specific model formats via the factory-based converter system.
 *
 * Two usage levels are available:
 *
 * 1. **Generic (registry-based):** Use ConverterRegistry to find converters
 *    by format ID and call importFromBytes()/exportToBytes() for uniform
 *    byte-level I/O.
 *
 * 2. **Typed (direct):** Use MdxConverter, M2Converter, or M3Converter
 *    directly with format-specific typed methods (fromMdx, toMdx, etc.)
 *    for struct-to-struct conversion.
 *
 * Legacy free functions (fromMdx, toMdx, etc.) remain for backward
 * compatibility and delegate to the typed converter classes.
 *
 * @example Registry-based conversion
 * @code
 * auto* conv = wem::ConverterRegistry::instance().find("mdx");
 * auto result = conv->importFromBytes(mdxFileData);
 * @endcode
 *
 * @example Typed conversion
 * @code
 * wem::MdxConverter conv;
 * auto [wemModel, issues] = conv.fromMdx(mdxModel);
 * @endcode
 */

#include "converter_base.h"
#include "../mdx/types.h"
#include "../mdx/structures.h"
#include "../m2/structures.h"
#include "../m3/structures.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Format-Specific Typed Results
// ============================================================================

struct MdxConvertResult {
    mdx::Model model;
    std::vector<std::string> issues;
};

struct M2ConvertResult {
    m2::FileSystem fileSystem;
    std::vector<std::string> issues;
};

struct M3ConvertResult {
    m3::Model model;
    std::vector<std::string> issues;
};

// ============================================================================
// MdxConverter
// ============================================================================

/**
 * @brief Converter between WEM and Warcraft III MDX format
 *
 * Supports both byte-level and typed struct-to-struct conversion.
 */
class MdxConverter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;
    ConvertResult importFromBytes(std::span<const u8> data) const override;
    ExportResult exportToBytes(const Model& model, u32 version = 0) const override;

    /** @brief Convert an MDX model to WEM representation (mesh + material only) */
    ConvertResult fromMdx(const mdx::Model& mdxModel) const;

    /** @brief Convert a WEM model to MDX representation (mesh + material only) */
    MdxConvertResult toMdx(const Model& wemModel, u32 targetVersion = 800) const;
};

// ============================================================================
// M2Converter
// ============================================================================

/**
 * @brief Converter between WEM and World of Warcraft M2 format
 *
 * Byte-level import is not supported because M2 requires a multi-file bundle
 * (base .m2 + .skin + .anim files). Use fromM2() with a parsed FileSystem.
 * Byte-level export writes only the base .m2 file.
 */
class M2Converter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;
    ExportResult exportToBytes(const Model& model, u32 version = 0) const override;

    /** @brief Convert an M2 file system (model bundle) to WEM representation */
    ConvertResult fromM2(const m2::FileSystem& m2Files) const;

    /** @brief Convert a WEM model to M2 representation (mesh + material only) */
    M2ConvertResult toM2(const Model& wemModel, u32 targetVersion = 274) const;
};

// ============================================================================
// M3Converter
// ============================================================================

/**
 * @brief Converter between WEM and StarCraft II / Heroes of the Storm M3 format
 *
 * Supports both byte-level and typed struct-to-struct conversion.
 */
class M3Converter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;
    ConvertResult importFromBytes(std::span<const u8> data) const override;
    ExportResult exportToBytes(const Model& model, u32 version = 0) const override;

    /** @brief Convert an M3 model to WEM representation (mesh + material only) */
    ConvertResult fromM3(const m3::Model& m3Model) const;

    /** @brief Convert a WEM model to M3 representation (mesh + material only) */
    M3ConvertResult toM3(const Model& wemModel, u32 targetVersion = 30) const;
};

// ============================================================================
// Legacy Free Functions (backward compatibility)
// ============================================================================

ConvertResult fromMdx(const mdx::Model& mdxModel);
MdxConvertResult toMdx(const Model& wemModel, u32 targetVersion = 800);

ConvertResult fromM2(const m2::FileSystem& m2Files);
M2ConvertResult toM2(const Model& wemModel, u32 targetVersion = 274);

ConvertResult fromM3(const m3::Model& m3Model);
M3ConvertResult toM3(const Model& wemModel, u32 targetVersion = 30);

} // namespace wem
} // namespace models
} // namespace whiteout
