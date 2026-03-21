// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file converter_base.h
 * @brief Base class and registry for extensible WEM format converters
 *
 * Provides the FormatConverter abstract interface and ConverterRegistry
 * factory for format conversion. New format converters can be added by
 * subclassing FormatConverter and registering with the registry.
 *
 * @example Adding a new format converter
 * @code
 * class FbxConverter : public wem::FormatConverter {
 * public:
 *     std::string formatId() const override { return "fbx"; }
 *     std::string formatName() const override { return "Autodesk FBX"; }
 *     bool supportsImport() const override { return true; }
 *     bool supportsExport() const override { return false; }
 *     u32 defaultExportVersion() const override { return 0; }
 *
 *     ConvertResult importFromBytes(std::span<const u8> data) const override {
 *         // Parse FBX data and convert to WEM...
 *     }
 * };
 *
 * // Register at any point before use
 * wem::ConverterRegistry::instance().registerConverter(
 *     std::make_shared<FbxConverter>());
 * @endcode
 */

#include <memory>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include "structures.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Conversion Results
// ============================================================================

/**
 * @brief Result of importing a model to WEM format
 */
struct ConvertResult {
    Model model;                     ///< The converted WEM model
    std::vector<std::string> issues; ///< Warnings about lossy or dropped data
};

/**
 * @brief Result of exporting a WEM model to a format's binary representation
 */
struct ExportResult {
    std::vector<u8> data;            ///< The exported binary data
    std::vector<std::string> issues; ///< Warnings about lossy or dropped data
};

// ============================================================================
// FormatConverter — abstract base for extensible format converters
// ============================================================================

/**
 * @brief Abstract base class for bidirectional WEM format converters.
 *
 * Subclass this to add support for a new model format. Override the
 * import/export methods as needed and register with ConverterRegistry.
 * Each converter also provides typed methods for direct struct-to-struct
 * conversion (declared on the concrete subclass).
 */
class FormatConverter {
public:
    virtual ~FormatConverter() = default;

    /** @brief Short lowercase format identifier (e.g. "mdx", "m2", "m3") */
    virtual std::string formatId() const = 0;

    /** @brief Human-readable format name (e.g. "Warcraft III MDX") */
    virtual std::string formatName() const = 0;

    /** @brief Whether this converter supports import (format -> WEM) */
    virtual bool supportsImport() const = 0;

    /** @brief Whether this converter supports export (WEM -> format) */
    virtual bool supportsExport() const = 0;

    /** @brief Default target format version for export */
    virtual u32 defaultExportVersion() const = 0;

    /**
     * @brief Import a model from raw bytes in this format
     * @param data Raw binary data to parse and convert
     * @return Converted WEM model with conversion issues
     *
     * Default implementation returns an error result.
     */
    virtual ConvertResult importFromBytes(std::span<const u8> data) const;

    /**
     * @brief Export a WEM model to raw bytes in this format
     * @param model The WEM model to export
     * @param version Target format version (0 = use defaultExportVersion())
     * @return Exported binary data with conversion issues
     *
     * Default implementation returns an error result.
     */
    virtual ExportResult exportToBytes(const Model& model, u32 version = 0) const;
};

// ============================================================================
// ConverterRegistry — central converter factory
// ============================================================================

/**
 * @brief Singleton registry for discovering and accessing format converters.
 *
 * Built-in converters (MDX, M2, M3) are registered automatically on first
 * access. External converters can be registered via registerConverter().
 */
class ConverterRegistry {
public:
    /** @brief Access the singleton registry instance */
    static ConverterRegistry& instance();

    /**
     * @brief Register a format converter
     * @param converter The converter to register (replaces existing with same formatId)
     */
    void registerConverter(std::shared_ptr<FormatConverter> converter);

    /**
     * @brief Find a converter by format ID
     * @param formatId The format identifier (e.g. "mdx")
     * @return Pointer to the converter, or nullptr if not found
     */
    const FormatConverter* find(const std::string& formatId) const;

    /**
     * @brief Get all registered converters
     * @return Vector of pointers to all registered converters
     */
    std::vector<const FormatConverter*> all() const;

private:
    ConverterRegistry();
    ~ConverterRegistry();

    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace wem
} // namespace models
} // namespace whiteout
