// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief WEM file writer
 *
 * This file provides the Writer class for writing WEM model files.
 * The writer converts Model structures to binary WEM format.
 *
 * @example Basic writing
 * @code
 * wem::Model model;
 * // ... populate model data ...
 *
 * wem::Writer writer;
 * writer.write("output.wem", model);
 * @endcode
 */

#include <memory>
#include <string>
#include <vector>

#include "../../common_types.h"
#include "structures.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// WEM Writer
// ============================================================================

/**
 * @brief Writer for WEM model files
 *
 * The Writer takes a Model structure and writes it to disk in binary
 * WEM format. It automatically handles chunk serialization and size calculation.
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
     * @brief Write a WEM file to disk
     * @param filePath Path where the WEM file should be written
     * @param model Model data to write
     * @return True on success
     * @throws std::runtime_error If file cannot be created or written
     */
    bool write(const std::string& filePath, const Model& model);

    /**
     * @brief Write a WEM model to a byte buffer
     * @param model Model data to write
     * @return Vector containing the binary WEM data
     */
    std::vector<u8> write(const Model& model);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace wem
} // namespace models
} // namespace whiteout
