// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_writer.h
 * @brief MDX file writer
 *
 * This file provides the Writer class for writing MDX model files.
 * The writer converts Model structures back to binary MDX format.
 *
 * @example Basic writing
 * @code
 * mdx::Model model;
 * // ... populate model data ...
 *
 * mdx::Writer writer;
 * writer.write("output.mdx", model);
 * @endcode
 */

#include <memory>
#include "../compatibility.h"
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
 * @brief Writer for MDX model files
 *
 * The Writer takes an Model structure and writes it to disk in binary
 * MDX format. It automatically handles chunk serialization and size calculation.
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
     * @brief Write an MDX file to disk
     * @param filePath Path where the MDX file should be written
     * @param mdx MDX file data to write
     * @throws std::runtime_error If file cannot be created or written
     */
    void write(const std::string& filePath, const Model& mdx);

    /**
     * @brief Write an MDX model to a byte buffer
     * @param mdx MDX file data to write
     * @return Vector containing the binary MDX data
     */
    std::vector<u8> write(const Model& mdx);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace mdx
} // namespace whiteout
