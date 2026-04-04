// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_writer.h
 * @brief MDX/MDL file writer
 *
 * This file provides the Writer class for writing MDX binary and MDL text
 * model files. The writer converts Model structures to binary MDX or text MDL
 * format based on file extension or explicit format parameter.
 *
 * @example Basic writing
 * @code
 * mdx::Model model;
 * // ... populate model data ...
 *
 * mdx::Writer writer;
 * writer.write("output.mdx", model);  // binary MDX
 * writer.write("output.mdl", model);  // text MDL
 * @endcode
 */

#include <memory>
#include <string>
#include "../../compatibility.h"
#include "parser.h"
#include "structures.h"
#include "types.h"

namespace whiteout {
namespace common {
class BinaryWriter;
}

namespace mdx {

// Use BinaryWriter from Common namespace
using common::BinaryWriter;

// ============================================================================
// MDX Writer
// ============================================================================

/**
 * @brief Writer for MDX/MDL model files
 *
 * The Writer takes a Model structure and writes it to disk in binary MDX
 * format or text MDL format. It automatically handles chunk serialization
 * and size calculation for MDX, and text formatting for MDL.
 *
 * The format is selected either by file extension (.mdl → text, .mdx → binary)
 * or by an explicit MDLXFormat parameter.
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide implementation details.
 */
class Writer {
public:
    /// @brief Construct a new Writer
    Writer();

    /// @brief Destructor (defined in .cpp for incomplete type)
    ~Writer();

    /**
     * @brief Write a model file to disk
     *
     * The format is detected from the file extension: `.mdl` for text MDL
     * format, `.mdx` (or any other extension) for binary MDX format.
     *
     * @param filePath Path where the file should be written
     * @param mdlx Model data to write
     * @throws std::runtime_error If file cannot be created or written
     */
    void write(const std::string& filePath, const Model& mdlx);

    /**
     * @brief Write a model to a byte buffer
     * @param mdx Model data to write
     * @param format Output format (MDX binary or MDL text, defaults to MDX)
     * @return Vector containing the binary MDX data or UTF-8 MDL text
     */
    std::vector<u8> write(const Model& mdx, MDLXFormat format = MDLXFormat::MDX);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace mdx
} // namespace whiteout
